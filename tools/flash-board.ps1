<#
.SYNOPSIS
    Bring a fresh FreeWili 2 from blank to running Meshtastic in one shot.

.DESCRIPTION
    Sequence (all SWD over the on-board RP2040 multiprobe / CMSIS-DAP):

      1. RDP unlock the WIO-E5 over SWD (iface 2): `stm32wlx unlock 0` writes
         RDP=0xAA to the option register, `option_load 0` triggers OBL_LAUNCH
         which mass-erases and reboots the chip at RDP Level 0 (~25 s). Skipped
         automatically if the chip is already at RDP=0xAA.

      2. Flash wio-e5-bridge to the WIO-E5 (iface 2).

      3. Flash meshtastic-freewili to the Display RP2350B (iface 0).

    Requires OpenOCD with CMSIS-DAP multi-interface support. The Pico SDK
    ships one at ~/.pico-sdk/openocd/<ver>/. Override with PICO_OPENOCD_DIR.

.PARAMETER ArtifactsDir
    Where to look for the release-named ELFs (wio-e5-bridge.elf,
    meshtastic-freewili.elf). Defaults to tools/artifacts/.

.PARAMETER UseLocalBuilds
    Ignore ArtifactsDir; pull the freshest ELF out of each subproject's
    build dir.

.PARAMETER ForceUnlock
    Run the SWD unlock even if RDP already reads 0xAA. Useful if you want to
    deliberately mass-erase the WIO-E5 again.

.PARAMETER SkipMeshtastic
    Skip step 3. Use when you only want to set up the WIO-E5.

.EXAMPLE
    pwsh tools/flash-board.ps1
    Full fresh-board flow using artifacts in tools/artifacts/.

.EXAMPLE
    pwsh tools/flash-board.ps1 -UseLocalBuilds
    Use locally-built ELFs from each subproject's build dir.
#>

[CmdletBinding()]
param(
    [string]$ArtifactsDir = (Join-Path $PSScriptRoot 'artifacts'),
    [switch]$UseLocalBuilds,
    [switch]$ForceUnlock,
    [switch]$SkipMeshtastic
)

$RepoRoot = Split-Path -Parent $PSScriptRoot

function Find-OpenOcd {
    $override = $env:PICO_OPENOCD_DIR
    $base = if ($override) { $override } else { Join-Path $HOME '.pico-sdk\openocd' }
    if (-not (Test-Path $base)) {
        throw "OpenOCD not found at $base. Install the Pico SDK or set PICO_OPENOCD_DIR."
    }
    $binName = if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'openocd.exe' } else { 'openocd' }
    $direct = Join-Path $base $binName
    if (Test-Path $direct) {
        return [pscustomobject]@{ Bin = $direct; Scripts = (Join-Path $base 'scripts') }
    }
    $versions = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue | Sort-Object Name
    if (-not $versions) { throw "No OpenOCD version directory under $base" }
    $picked = $versions[-1].FullName
    $bin = Join-Path $picked $binName
    if (-not (Test-Path $bin)) { throw "OpenOCD binary missing: $bin" }
    return [pscustomobject]@{ Bin = $bin; Scripts = (Join-Path $picked 'scripts') }
}

function Resolve-Artifact {
    param(
        [Parameter(Mandatory)] [string]$ReleaseName,
        [Parameter(Mandatory)] [string]$LocalGlob
    )
    if (-not $UseLocalBuilds) {
        $p = Join-Path $ArtifactsDir $ReleaseName
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    $globPath = Join-Path $RepoRoot $LocalGlob
    $hits = @(Get-ChildItem -Path $globPath -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
    if ($hits.Count -gt 0) { return $hits[0].FullName }
    return $null
}

function Invoke-OpenOcd {
    param(
        [Parameter(Mandatory)] [pscustomobject]$Ocd,
        [Parameter(Mandatory)] [int]$Interface,
        [Parameter(Mandatory)] [string]$TargetCfg,
        [Parameter(Mandatory)] [string]$ElfPath,
        [switch]$Rp2350  # On RP2350 SMP, "program ... reset exit" leaves both cores halted -
                         # the new firmware never runs. Workaround: explicit "reset run" after program.
    )
    $elfFwd = $ElfPath -replace '\\','/'
    $programCmd = if ($Rp2350) { "program `"$elfFwd`" verify" } else { "program `"$elfFwd`" verify reset exit" }
    $cmdArgs = @(
        '-s', $Ocd.Scripts,
        '-c', 'adapter driver cmsis-dap',
        '-c', "cmsis-dap usb interface $Interface",
        '-c', 'transport select swd',
        '-c', 'adapter speed 500',
        '-f', $TargetCfg,
        '-c', $programCmd
    )
    if ($Rp2350) {
        $cmdArgs += @('-c', 'reset run', '-c', 'shutdown')
    }
    Write-Host "    > $($Ocd.Bin) $($cmdArgs -join ' ')" -ForegroundColor DarkGray
    & $Ocd.Bin @cmdArgs
    return $LASTEXITCODE
}

Write-Host "==> Locating OpenOCD..."
$ocd = Find-OpenOcd
Write-Host "    $($ocd.Bin)"

Write-Host "==> Locating artifacts..."
$bridge = Resolve-Artifact -ReleaseName 'wio-e5-bridge.elf'        -LocalGlob 'wio-e5-bridge/.pio/build/wio-e5-bridge/firmware.elf'
$mesh   = Resolve-Artifact -ReleaseName 'meshtastic-freewili.elf'  -LocalGlob 'meshtastic-firmware/.pio/build/freewili/firmware-freewili-*.elf'

if (                          -not $bridge) { throw "Missing wio-e5-bridge.elf (try -UseLocalBuilds or drop the release ELF in $ArtifactsDir)" }
if (-not $SkipMeshtastic -and -not $mesh  ) { throw "Missing meshtastic-freewili.elf (try -UseLocalBuilds or drop the release ELF in $ArtifactsDir)" }

                            Write-Host "    bridge:     $bridge"
if (-not $SkipMeshtastic) { Write-Host "    meshtastic: $mesh" }

# ---- Step 1: SWD-based RDP unlock ----

Write-Host ""
Write-Host "==> Step 1/3: check + unlock WIO-E5 RDP over SWD (iface 2)" -ForegroundColor Cyan

# Read FLASH_OPTR (0x58004020). Low byte = RDP: 0xAA = Level 0, anything else
# but 0xCC = Level 1. (0xCC = Level 2, permanently locked - we can't recover.)
# OpenOCD's Tcl strips backslashes in -l paths, so convert to forward slashes.
$rdpLog = New-TemporaryFile
$rdpLogFwd = ($rdpLog.FullName) -replace '\\','/'
& $ocd.Bin -s $ocd.Scripts -c 'adapter driver cmsis-dap' -c 'cmsis-dap usb interface 2' -c 'transport select swd' -c 'adapter speed 500' -f target/stm32wlx.cfg -c 'init' -c 'mdw 0x58004020 1' -c 'exit' -l $rdpLogFwd | Out-Null
$rdpLine = Get-Content $rdpLog.FullName | Where-Object { $_ -match '^0x58004020:' } | Select-Object -First 1
Remove-Item $rdpLog.FullName -ErrorAction SilentlyContinue
if (-not $rdpLine) { throw "Couldn't read WIO-E5 FLASH_OPTR over SWD (iface 2). Check the multiprobe + power." }
$optr = [Convert]::ToUInt32(($rdpLine -split '\s+')[1], 16)
$rdp  = $optr -band 0xff
$rdpHex = '0x{0:x2}' -f $rdp
Write-Host "    FLASH_OPTR = 0x{0:x8}, RDP byte = {1}" -f $optr, $rdpHex

if ($rdp -eq 0xCC) {
    throw "WIO-E5 is at RDP Level 2 (0xCC) - permanently locked, cannot recover."
}

if ($rdp -ne 0xAA -or $ForceUnlock) {
    Write-Host "    Running stm32wlx unlock 0 + option_load 0 (mass erase + reboot, ~25 s)..." -ForegroundColor Yellow
    & $ocd.Bin -s $ocd.Scripts -c 'adapter driver cmsis-dap' -c 'cmsis-dap usb interface 2' -c 'transport select swd' -c 'adapter speed 500' -f target/stm32wlx.cfg -c 'init' -c 'reset halt' -c 'stm32wlx unlock 0' -c 'stm32wlx option_load 0' -c 'exit' 2>&1 | Out-String -Stream | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    Write-Host "    Waiting 30 s for mass erase to complete..."
    Start-Sleep -Seconds 30
} else {
    Write-Host "    Already at RDP Level 0 - skipping unlock." -ForegroundColor Green
}

# ---- Step 2: bridge firmware ----

Write-Host ""
Write-Host "==> Step 2/3: flash WIO-E5 bridge firmware (iface 2)" -ForegroundColor Cyan
$bridgeAttempts = 3
$rc = 1
for ($i = 1; $i -le $bridgeAttempts -and $rc -ne 0; $i++) {
    if ($i -gt 1) {
        Write-Host "    Attempt $i/$bridgeAttempts after 5 s wait..." -ForegroundColor Yellow
        Start-Sleep -Seconds 5
    }
    $rc = Invoke-OpenOcd -Ocd $ocd -Interface 2 -TargetCfg 'target/stm32wlx.cfg' -ElfPath $bridge
}
if ($rc -ne 0) {
    throw "Bridge flash failed after $bridgeAttempts attempts."
}

# ---- Step 3: meshtastic firmware ----

if (-not $SkipMeshtastic) {
    Write-Host ""
    Write-Host "==> Step 3/3: flash meshtastic firmware to Display CPU (iface 0)" -ForegroundColor Cyan
    $rc = Invoke-OpenOcd -Ocd $ocd -Interface 0 -TargetCfg 'target/rp2350.cfg' -ElfPath $mesh -Rp2350
    if ($rc -ne 0) { throw "Meshtastic flash failed (exit $rc)" }
}

Write-Host ""
Write-Host "DONE. Board is up." -ForegroundColor Green
