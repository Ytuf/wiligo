<#
.SYNOPSIS
    Bring a fresh FreeWili 2 from blank to running Meshtastic in one shot.
    Production-line hardened: structured exit codes, JSON logging, sanity
    check, multi-probe disambiguation, retry loops, and -Loop mode.

.DESCRIPTION
    Sequence (all SWD over the on-board RP2040 multiprobe / CMSIS-DAP):

      1. RDP unlock the WIO-E5 over SWD (iface 2): `stm32wlx unlock 0` writes
         RDP=0xAA to the option register, `option_load 0` triggers OBL_LAUNCH
         which mass-erases and reboots the chip at RDP Level 0 (~25 s). Skipped
         automatically if the chip is already at RDP=0xAA.

      2. Flash wio-e5-bridge to the WIO-E5 (iface 2).

      3. Flash meshtastic-freewili to the Display RP2350B (iface 0).

      4. (post) Sanity check: read g_uart_byte_count on iface 0 to confirm
         the freshly-flashed Display CPU is actually running AND receiving
         UART bytes from the freshly-flashed WIO-E5 bridge. Exits non-zero
         if the firmware never starts incrementing the counter within 10 s.

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

.PARAMETER Serial
    USB serial of the multiprobe to target (e.g. E664B4950787792F). Passed to
    OpenOCD as `cmsis-dap serial <serial>`. When omitted, the first matching
    probe wins; if 2+ probes are present the script refuses and exits with
    code 2 to prevent flashing the wrong board.

.PARAMETER JsonLog
    Path to a JSON log file. One JSON object is appended per stage plus a
    final summary line, all using Compress format (one object per line).

.PARAMETER SkipSanityCheck
    Skip the post-flash UART sanity check. Use for dev/CI where you don't
    intend to actually boot the firmware (e.g. flashing on an unpowered
    rail or a board with no bridge attached).

.PARAMETER Loop
    After each board (success or failure), wait for the multiprobe serial
    number to change (poll Get-PnpDevice every 2 s, timeout 5 min) and
    process the new board. Ctrl+C to exit. Designed for production-line
    operators who swap boards in/out continuously.

.EXAMPLE
    pwsh tools/flash-board.ps1
    Full fresh-board flow using artifacts in tools/artifacts/.

.EXAMPLE
    pwsh tools/flash-board.ps1 -UseLocalBuilds
    Use locally-built ELFs from each subproject's build dir.

.EXAMPLE
    pwsh tools/flash-board.ps1 -Serial E664B4950787792F -JsonLog C:\line\boards.jsonl
    Target a specific multiprobe and emit structured logs.

.EXAMPLE
    pwsh tools/flash-board.ps1 -Loop -JsonLog C:\line\shift.jsonl
    Production line: flash board after board until Ctrl+C.

.NOTES
    Exit codes:
      0 = success
      1 = prereq missing (OpenOCD, artifacts, pwsh too old, nm missing, ...)
      2 = hardware not detected (multiprobe missing / wrong firmware / ambiguous)
      3 = unlock failed (stm32wlx unlock returned error, or option_load did
          not actually clear RDP)
      4 = bridge flash failed
      5 = meshtastic flash failed
      6 = post-flash sanity check failed
      7 = SWD wedge detected (cannot read IDR / DAP) - power-cycle required
#>

[CmdletBinding()]
param(
    # $PSScriptRoot is resolved later (see Get-ScriptDir) to tolerate
    # embedded-host scenarios where it's empty in the param block.
    [string]$ArtifactsDir,
    [switch]$UseLocalBuilds,
    [switch]$ForceUnlock,
    [switch]$SkipMeshtastic,
    [string]$Serial,
    [string]$JsonLog,
    [switch]$SkipSanityCheck,
    [switch]$Loop
)

function Get-ScriptDir {
    if ($PSScriptRoot) { return $PSScriptRoot }
    if ($MyInvocation.MyCommand.Path) {
        return (Split-Path -Parent $MyInvocation.MyCommand.Path)
    }
    if ($PSCommandPath) { return (Split-Path -Parent $PSCommandPath) }
    return (Get-Location).Path
}

$ScriptDir = Get-ScriptDir
$RepoRoot  = Split-Path -Parent $ScriptDir
if (-not $ArtifactsDir) {
    $ArtifactsDir = Join-Path $ScriptDir 'artifacts'
}

# ---- exit codes ---------------------------------------------------------

$EXIT_OK              = 0
$EXIT_PREREQ          = 1
$EXIT_HW_NOT_DETECTED = 2
$EXIT_UNLOCK_FAILED   = 3
$EXIT_BRIDGE_FAILED   = 4
$EXIT_MESH_FAILED     = 5
$EXIT_SANITY_FAILED   = 6
$EXIT_SWD_WEDGE       = 7

# ---- logging helpers ----------------------------------------------------

$Script:RunStartTicks = (Get-Date).Ticks
$Script:BoardSerial   = if ($Serial) { $Serial } else { '' }

function Write-ErrorLine {
    param([int]$Code, [string]$Message)
    Write-Host "[E$Code] $Message" -ForegroundColor Red
}

function Write-JsonLine {
    param([hashtable]$Obj)
    if (-not $JsonLog) { return }
    try {
        $line = $Obj | ConvertTo-Json -Compress -Depth 4
        $line | Out-File -FilePath $JsonLog -Encoding utf8 -Append
    } catch {
        Write-Host "(warn) failed to append to JsonLog: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

function Write-StageLog {
    param(
        [string]$Stage,
        [string]$Status,     # success | failed | skipped
        [long]$DurationMs,
        [int]$ErrorCode = 0,
        [string]$ErrorMessage = ''
    )
    Write-JsonLine @{
        stage         = $Stage
        status        = $Status
        duration_ms   = $DurationMs
        error_code    = $ErrorCode
        error_message = $ErrorMessage
        board_serial  = $Script:BoardSerial
        timestamp     = (Get-Date).ToString('o')
    }
}

function Exit-Flash {
    param([int]$Code, [string]$Message = '')
    if ($Message) { Write-ErrorLine -Code $Code -Message $Message }
    $totalMs = [long](((Get-Date).Ticks - $Script:RunStartTicks) / 10000)
    $result  = if ($Code -eq 0) { 'pass' } else { 'fail' }
    Write-JsonLine @{
        result            = $result
        exit_code         = $Code
        duration_total_ms = $totalMs
        board_serial      = $Script:BoardSerial
        timestamp         = (Get-Date).ToString('o')
    }
    exit $Code
}

# ---- discovery helpers --------------------------------------------------

function Find-OpenOcd {
    $override = $env:PICO_OPENOCD_DIR
    $base = if ($override) { $override } else { Join-Path $HOME '.pico-sdk\openocd' }
    if (-not (Test-Path $base)) {
        return $null
    }
    $binName = if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'openocd.exe' } else { 'openocd' }
    $direct = Join-Path $base $binName
    if (Test-Path $direct) {
        return [pscustomobject]@{ Bin = $direct; Scripts = (Join-Path $base 'scripts') }
    }
    $versions = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue | Sort-Object Name
    if (-not $versions) { return $null }
    $picked = $versions[-1].FullName
    $bin = Join-Path $picked $binName
    if (-not (Test-Path $bin)) { return $null }
    return [pscustomobject]@{ Bin = $bin; Scripts = (Join-Path $picked 'scripts') }
}

function Find-Nm {
    $cmd = Get-Command 'arm-none-eabi-nm' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.2 Rel1\bin\arm-none-eabi-nm.exe',
        'C:\Program Files\Arm GNU Toolchain arm-none-eabi\13.2 Rel1\bin\arm-none-eabi-nm.exe'
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

function Get-MultiprobeSerials {
    # Returns a [string[]] of unique USB serial numbers. PowerShell unwraps
    # arrays through `return`, so we always return via the pipeline as a
    # single object (the array itself) and let the caller wrap in @().
    # -Present filters out historical/cached USB entries (Windows remembers
    # every device ever plugged into a port). Without it, a host that has
    # touched 8 different multiprobes over its life shows all 8 even when
    # only 1 is physically connected.
    $devs = Get-PnpDevice -Class USB -Present -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_2E8A&PID_000C' }
    $set = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($d in $devs) {
        # InstanceId looks like: USB\VID_2E8A&PID_000C\E664B49507713D2F
        # Composite parents share the same serial as their children, so a
        # HashSet de-dups naturally.
        $parts = $d.InstanceId -split '\\'
        if ($parts.Length -ge 3) {
            [void]$set.Add($parts[-1])
        }
    }
    # Force [string[]] to avoid PowerShell collapsing a 1-element collection.
    return [string[]]@($set)
}

function Resolve-Artifact {
    param(
        [Parameter(Mandatory)] [string]$ReleaseName,
        [Parameter(Mandatory)] [string]$LocalGlob
    )
    # Default mode: only the artifacts dir is consulted. We deliberately do
    # NOT fall back to local builds when the artifact is missing - that would
    # silently flash a stale dev ELF in production. Pass -UseLocalBuilds to
    # opt into the build-dir hunt.
    if ($UseLocalBuilds) {
        $globPath = Join-Path $RepoRoot $LocalGlob
        $hits = @(Get-ChildItem -Path $globPath -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
        if ($hits.Count -gt 0) { return $hits[0].FullName }
        return $null
    }
    $p = Join-Path $ArtifactsDir $ReleaseName
    if (Test-Path $p) { return (Resolve-Path $p).Path }
    return $null
}

# ---- OpenOCD invocation with retry --------------------------------------

function Build-OpenOcdBaseArgs {
    param(
        [Parameter(Mandatory)] [pscustomobject]$Ocd,
        [Parameter(Mandatory)] [int]$Interface
    )
    $base = @(
        '-s', $Ocd.Scripts,
        '-c', 'adapter driver cmsis-dap'
    )
    if ($Script:BoardSerial) {
        # `adapter serial XXX` is the canonical OpenOCD 0.12 syntax for
        # picking a specific CMSIS-DAP probe by USB serial. The
        # `cmsis-dap serial` subcommand was added in newer OpenOCD trees
        # and does NOT exist in the Pico SDK build we use.
        $base += @('-c', "adapter serial $($Script:BoardSerial)")
    }
    $base += @(
        '-c', "cmsis-dap usb interface $Interface",
        '-c', 'transport select swd',
        '-c', 'adapter speed 500'
    )
    return $base
}

function Invoke-OpenOcdRaw {
    param(
        [Parameter(Mandatory)] [pscustomobject]$Ocd,
        [Parameter(Mandatory)] [int]$Interface,
        [Parameter(Mandatory)] [string]$TargetCfg,
        [Parameter(Mandatory)] [string[]]$Commands,
        [string]$LogFile
    )
    $args = Build-OpenOcdBaseArgs -Ocd $Ocd -Interface $Interface
    $args += @('-f', $TargetCfg)
    foreach ($c in $Commands) { $args += @('-c', $c) }
    if ($LogFile) {
        $lf = $LogFile -replace '\\','/'
        $args += @('-l', $lf)
    }
    Write-Host "    > $($Ocd.Bin) $($args -join ' ')" -ForegroundColor DarkGray
    & $Ocd.Bin @args | Out-Null
    return $LASTEXITCODE
}

function Invoke-OpenOcdWithRetry {
    param(
        [Parameter(Mandatory)] [pscustomobject]$Ocd,
        [Parameter(Mandatory)] [int]$Interface,
        [Parameter(Mandatory)] [string]$TargetCfg,
        [Parameter(Mandatory)] [string[]]$Commands,
        [int]$Attempts = 3,
        [int]$SleepSeconds = 10,
        [string]$LogFile
    )
    $rc = -1
    for ($i = 1; $i -le $Attempts; $i++) {
        if ($i -gt 1) {
            Write-Host "    Retry $i/$Attempts after ${SleepSeconds}s (Multiprobe not found - check USB / replug)..." -ForegroundColor Yellow
            Start-Sleep -Seconds $SleepSeconds
        }
        $rc = Invoke-OpenOcdRaw -Ocd $Ocd -Interface $Interface -TargetCfg $TargetCfg -Commands $Commands -LogFile $LogFile
        if ($rc -eq 0) { return 0 }
    }
    return $rc
}

function Invoke-OpenOcdProgram {
    param(
        [Parameter(Mandatory)] [pscustomobject]$Ocd,
        [Parameter(Mandatory)] [int]$Interface,
        [Parameter(Mandatory)] [string]$TargetCfg,
        [Parameter(Mandatory)] [string]$ElfPath,
        [switch]$Rp2350,
        [int]$Attempts = 3,
        [int]$SleepSeconds = 10
    )
    $elfFwd = $ElfPath -replace '\\','/'
    $cmds = @()
    if ($Rp2350) {
        # RP2350 SMP: "program ... reset exit" leaves both cores halted.
        # Workaround: program/verify, then explicit "reset run" + shutdown.
        $cmds += "program `"$elfFwd`" verify"
        $cmds += 'reset run'
        $cmds += 'shutdown'
    } else {
        $cmds += "program `"$elfFwd`" verify reset exit"
    }
    return (Invoke-OpenOcdWithRetry -Ocd $Ocd -Interface $Interface -TargetCfg $TargetCfg -Commands $cmds -Attempts $Attempts -SleepSeconds $SleepSeconds)
}

# ---- target-specific helpers --------------------------------------------

function Read-WordSwd {
    param(
        [Parameter(Mandatory)] [pscustomobject]$Ocd,
        [Parameter(Mandatory)] [int]$Interface,
        [Parameter(Mandatory)] [string]$TargetCfg,
        [Parameter(Mandatory)] [UInt32]$Address,
        [int]$Attempts = 3,
        [int]$SleepSeconds = 10
    )
    $tmp = New-TemporaryFile
    try {
        $hex = '0x{0:x8}' -f $Address
        $rc = Invoke-OpenOcdWithRetry -Ocd $Ocd -Interface $Interface -TargetCfg $TargetCfg `
            -Commands @('init', "mdw $hex 1", 'exit') -LogFile $tmp.FullName `
            -Attempts $Attempts -SleepSeconds $SleepSeconds
        if ($rc -ne 0) { return $null }
        $line = Get-Content $tmp.FullName | Where-Object { $_ -match "^${hex}:" } | Select-Object -First 1
        if (-not $line) { return $null }
        $val = ($line -split '\s+')[1]
        return [Convert]::ToUInt32($val, 16)
    } finally {
        Remove-Item $tmp.FullName -ErrorAction SilentlyContinue
    }
}

function Read-SymbolAddress {
    param(
        [Parameter(Mandatory)] [string]$NmExe,
        [Parameter(Mandatory)] [string]$ElfPath,
        [Parameter(Mandatory)] [string]$Symbol
    )
    $out = & $NmExe $ElfPath 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    $line = $out | Where-Object { $_ -match "\s$([Regex]::Escape($Symbol))$" } | Select-Object -First 1
    if (-not $line) { return $null }
    $addrHex = ($line -split '\s+')[0]
    try { return [Convert]::ToUInt32($addrHex, 16) } catch { return $null }
}

# ---- pre-flight ---------------------------------------------------------

function Invoke-PreFlight {
    param(
        [Parameter(Mandatory)] [pscustomobject]$Ocd
    )

    Write-Host "==> Pre-flight: verify multiprobe enumerates on all 3 interfaces..." -ForegroundColor Cyan
    $ifaceNames = @{ 0 = 'DISPLAY'; 1 = 'MAIN'; 2 = 'WIO-E5' }
    $ifaceTargets = @{ 0 = 'target/rp2350.cfg'; 1 = 'target/rp2350.cfg'; 2 = 'target/stm32wlx.cfg' }
    foreach ($iface in 0,1,2) {
        Write-Host "    iface $iface ($($ifaceNames[$iface]))..."
        $tmp = New-TemporaryFile
        try {
            # Just init + exit. Any failure here means the multiprobe firmware
            # doesn't expose this CMSIS-DAP interface.
            $rc = Invoke-OpenOcdWithRetry -Ocd $Ocd -Interface $iface -TargetCfg $ifaceTargets[$iface] `
                -Commands @('init', 'exit') -LogFile $tmp.FullName `
                -Attempts 3 -SleepSeconds 10
            if ($rc -ne 0) {
                Write-StageLog -Stage 'preflight' -Status 'failed' -DurationMs 0 `
                    -ErrorCode $EXIT_HW_NOT_DETECTED `
                    -ErrorMessage "iface $iface ($($ifaceNames[$iface])) failed to enumerate"
                Exit-Flash -Code $EXIT_HW_NOT_DETECTED `
                    -Message "Multiprobe firmware too old; expected 3 interfaces (iface $iface = $($ifaceNames[$iface]) did not enumerate). Re-flash the on-board RP2040 with the latest multiprobe firmware."
            }
        } finally {
            Remove-Item $tmp.FullName -ErrorAction SilentlyContinue
        }
    }
    Write-Host "    All 3 interfaces OK." -ForegroundColor Green
}

# ---- the work for one board ---------------------------------------------

function Invoke-FlashOneBoard {
    $Script:RunStartTicks = (Get-Date).Ticks

    # -------- prereqs --------

    Write-Host "==> Locating OpenOCD..."
    $ocd = Find-OpenOcd
    if (-not $ocd) {
        Exit-Flash -Code $EXIT_PREREQ -Message "OpenOCD not found under `$HOME\.pico-sdk\openocd or `$env:PICO_OPENOCD_DIR. Install the Pico SDK or set PICO_OPENOCD_DIR."
    }
    Write-Host "    $($ocd.Bin)"

    $nm = Find-Nm
    if (-not $SkipSanityCheck -and -not $SkipMeshtastic -and -not $nm) {
        Exit-Flash -Code $EXIT_PREREQ -Message "arm-none-eabi-nm not on PATH. Required for sanity-check symbol lookup. Install the Arm GNU Toolchain or pass -SkipSanityCheck."
    }

    Write-Host "==> Locating artifacts..."
    $bridge = Resolve-Artifact -ReleaseName 'wio-e5-bridge.elf'        -LocalGlob 'wio-e5-bridge/.pio/build/wio-e5-bridge/firmware.elf'
    $mesh   = Resolve-Artifact -ReleaseName 'meshtastic-freewili.elf'  -LocalGlob 'meshtastic-firmware/.pio/build/freewili/firmware-freewili-*.elf'
    if (-not $bridge) {
        Exit-Flash -Code $EXIT_PREREQ -Message "Missing wio-e5-bridge.elf (try -UseLocalBuilds or drop the release ELF in $ArtifactsDir)."
    }
    if (-not $SkipMeshtastic -and -not $mesh) {
        Exit-Flash -Code $EXIT_PREREQ -Message "Missing meshtastic-freewili.elf (try -UseLocalBuilds or drop the release ELF in $ArtifactsDir)."
    }
    Write-Host "    bridge:     $bridge"
    if (-not $SkipMeshtastic) { Write-Host "    meshtastic: $mesh" }

    # -------- multiprobe disambiguation --------

    Write-Host "==> Enumerating multiprobes..."
    $serials = @(Get-MultiprobeSerials)
    Write-Host "    Found $($serials.Count) probe(s): $($serials -join ', ')"

    if ($Serial) {
        if ($serials -notcontains $Serial) {
            Exit-Flash -Code $EXIT_HW_NOT_DETECTED `
                -Message "Multiprobe with serial '$Serial' not enumerated. Present serials: $($serials -join ', '). Check USB / replug."
        }
        $Script:BoardSerial = $Serial
    } else {
        if ($serials.Count -eq 0) {
            Exit-Flash -Code $EXIT_HW_NOT_DETECTED `
                -Message "No FreeWili multiprobes detected (VID 0x2e8a PID 0x000c). Plug in the board and try again."
        }
        if ($serials.Count -gt 1) {
            Exit-Flash -Code $EXIT_HW_NOT_DETECTED `
                -Message "Multiple multiprobes present ($($serials.Count)): $($serials -join ', '). Pass -Serial <usb-serial> to disambiguate, or unplug all but one."
        }
        $Script:BoardSerial = $serials[0]
        Write-Host "    Using single probe: $($Script:BoardSerial)"
    }

    # -------- pre-flight per-interface init --------

    Invoke-PreFlight -Ocd $ocd

    # -------- Step 1: SWD-based RDP unlock --------

    Write-Host ""
    Write-Host "==> Step 1/3: check + unlock WIO-E5 RDP over SWD (iface 2)" -ForegroundColor Cyan
    $stageStart = (Get-Date)

    $optr = Read-WordSwd -Ocd $ocd -Interface 2 -TargetCfg 'target/stm32wlx.cfg' -Address ([UInt32]0x58004020)
    if ($null -eq $optr) {
        $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
        Write-StageLog -Stage 'unlock' -Status 'failed' -DurationMs $durMs `
            -ErrorCode $EXIT_SWD_WEDGE -ErrorMessage 'Could not read FLASH_OPTR over SWD - cannot read DAP/IDR'
        Write-Host ""
        Write-Host "    SWD wedge detected. Recovery:" -ForegroundColor Yellow
        Write-Host "      1. Unplug the FreeWili from USB." -ForegroundColor Yellow
        Write-Host "      2. Wait 5 s for the SWD bus capacitance to drain." -ForegroundColor Yellow
        Write-Host "      3. Re-plug and re-run this script." -ForegroundColor Yellow
        Write-Host "      4. If repeat: see wio-e5-unlock/README.md for the WIO_RST recovery helper." -ForegroundColor Yellow
        Exit-Flash -Code $EXIT_SWD_WEDGE `
            -Message "Cannot read WIO-E5 FLASH_OPTR over SWD - SWD bus wedged. Power-cycle the board and re-run."
    }

    $rdp = $optr -band 0xff
    Write-Host ("    FLASH_OPTR = 0x{0:x8}, RDP byte = 0x{1:x2}" -f $optr, $rdp)

    if ($rdp -eq 0xCC) {
        $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
        Write-StageLog -Stage 'unlock' -Status 'failed' -DurationMs $durMs `
            -ErrorCode $EXIT_UNLOCK_FAILED -ErrorMessage 'WIO-E5 at RDP Level 2 (0xCC) - permanently locked'
        Exit-Flash -Code $EXIT_UNLOCK_FAILED `
            -Message "WIO-E5 is at RDP Level 2 (0xCC) - permanently locked. This board cannot be recovered. Scrap or RMA."
    }

    if ($rdp -ne 0xAA -or $ForceUnlock) {
        Write-Host "    Running stm32wlx unlock 0 + option_load 0 (mass erase + reboot, ~25 s)..." -ForegroundColor Yellow
        $unlockRc = Invoke-OpenOcdWithRetry -Ocd $ocd -Interface 2 -TargetCfg 'target/stm32wlx.cfg' `
            -Commands @('init', 'reset halt', 'stm32wlx unlock 0', 'stm32wlx option_load 0', 'exit') `
            -Attempts 3 -SleepSeconds 10
        if ($unlockRc -ne 0) {
            $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
            Write-StageLog -Stage 'unlock' -Status 'failed' -DurationMs $durMs `
                -ErrorCode $EXIT_UNLOCK_FAILED -ErrorMessage "stm32wlx unlock returned $unlockRc"
            Exit-Flash -Code $EXIT_UNLOCK_FAILED `
                -Message "stm32wlx unlock failed (OpenOCD exit $unlockRc). Check SWD wiring and power on the WIO-E5 rail."
        }
        Write-Host "    Waiting 30 s for mass erase to complete..."
        Start-Sleep -Seconds 30

        # Verify RDP cleared - belt and braces
        $optrPost = Read-WordSwd -Ocd $ocd -Interface 2 -TargetCfg 'target/stm32wlx.cfg' -Address ([UInt32]0x58004020)
        if ($null -eq $optrPost) {
            $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
            Write-StageLog -Stage 'unlock' -Status 'failed' -DurationMs $durMs `
                -ErrorCode $EXIT_SWD_WEDGE -ErrorMessage 'Post-unlock FLASH_OPTR read failed'
            Exit-Flash -Code $EXIT_SWD_WEDGE `
                -Message "Post-unlock SWD read failed. Power-cycle the board and re-run."
        }
        $rdpPost = $optrPost -band 0xff
        if ($rdpPost -ne 0xAA) {
            $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
            Write-StageLog -Stage 'unlock' -Status 'failed' -DurationMs $durMs `
                -ErrorCode $EXIT_UNLOCK_FAILED `
                -ErrorMessage ("option_load did not clear RDP; post-RDP=0x{0:x2}" -f $rdpPost)
            Exit-Flash -Code $EXIT_UNLOCK_FAILED `
                -Message ("option_load 0 did not actually clear RDP (post-RDP=0x{0:x2}, expected 0xAA). Try -ForceUnlock again or scope the WIO-E5 power rail." -f $rdpPost)
        }
        Write-Host "    RDP successfully cleared." -ForegroundColor Green
        $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
        Write-StageLog -Stage 'unlock' -Status 'success' -DurationMs $durMs
    } else {
        Write-Host "    Already at RDP Level 0 - skipping unlock." -ForegroundColor Green
        $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
        Write-StageLog -Stage 'unlock' -Status 'skipped' -DurationMs $durMs
    }

    # -------- Step 2: bridge firmware --------

    Write-Host ""
    Write-Host "==> Step 2/3: flash WIO-E5 bridge firmware (iface 2)" -ForegroundColor Cyan
    $stageStart = (Get-Date)
    $rc = Invoke-OpenOcdProgram -Ocd $ocd -Interface 2 -TargetCfg 'target/stm32wlx.cfg' -ElfPath $bridge `
        -Attempts 3 -SleepSeconds 10
    $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
    if ($rc -ne 0) {
        Write-StageLog -Stage 'bridge' -Status 'failed' -DurationMs $durMs `
            -ErrorCode $EXIT_BRIDGE_FAILED -ErrorMessage "OpenOCD exit $rc"
        Exit-Flash -Code $EXIT_BRIDGE_FAILED `
            -Message "Bridge flash failed after 3 attempts (OpenOCD exit $rc). Check SWD on iface 2 and WIO-E5 power."
    }
    Write-StageLog -Stage 'bridge' -Status 'success' -DurationMs $durMs

    # -------- Step 3: meshtastic firmware --------

    if ($SkipMeshtastic) {
        Write-StageLog -Stage 'meshtastic' -Status 'skipped' -DurationMs 0
    } else {
        Write-Host ""
        Write-Host "==> Step 3/3: flash meshtastic firmware to Display CPU (iface 0)" -ForegroundColor Cyan
        $stageStart = (Get-Date)
        $rc = Invoke-OpenOcdProgram -Ocd $ocd -Interface 0 -TargetCfg 'target/rp2350.cfg' -ElfPath $mesh -Rp2350 `
            -Attempts 3 -SleepSeconds 10
        $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
        if ($rc -ne 0) {
            Write-StageLog -Stage 'meshtastic' -Status 'failed' -DurationMs $durMs `
                -ErrorCode $EXIT_MESH_FAILED -ErrorMessage "OpenOCD exit $rc"
            Exit-Flash -Code $EXIT_MESH_FAILED `
                -Message "Meshtastic flash failed after 3 attempts (OpenOCD exit $rc). Check SWD on iface 0 and Display CPU power."
        }
        Write-StageLog -Stage 'meshtastic' -Status 'success' -DurationMs $durMs
    }

    # -------- Step 4: post-flash sanity check --------

    if ($SkipSanityCheck -or $SkipMeshtastic) {
        if ($SkipMeshtastic) {
            Write-StageLog -Stage 'sanity' -Status 'skipped' -DurationMs 0 `
                -ErrorMessage '-SkipMeshtastic: nothing to sanity-check'
        } else {
            Write-StageLog -Stage 'sanity' -Status 'skipped' -DurationMs 0 `
                -ErrorMessage '-SkipSanityCheck flag'
        }
    } else {
        Write-Host ""
        Write-Host "==> Sanity: verify Display CPU is running + receiving UART from bridge" -ForegroundColor Cyan
        $stageStart = (Get-Date)

        Write-Host "    Resolving symbol addresses from staged ELF..."
        $addrCount = Read-SymbolAddress -NmExe $nm -ElfPath $mesh -Symbol 'g_uart_byte_count'
        $addrRouter = Read-SymbolAddress -NmExe $nm -ElfPath $mesh -Symbol 'g_router_send_entry'
        if ($null -eq $addrCount) {
            $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
            Write-StageLog -Stage 'sanity' -Status 'failed' -DurationMs $durMs `
                -ErrorCode $EXIT_SANITY_FAILED `
                -ErrorMessage 'g_uart_byte_count not found in meshtastic ELF'
            Exit-Flash -Code $EXIT_SANITY_FAILED `
                -Message "g_uart_byte_count symbol not in $mesh. Build with the sanity-check instrumentation or pass -SkipSanityCheck."
        }
        Write-Host ("    g_uart_byte_count   @ 0x{0:x8}" -f $addrCount)
        if ($null -ne $addrRouter) {
            Write-Host ("    g_router_send_entry @ 0x{0:x8}" -f $addrRouter)
        }

        Write-Host "    Waiting 10 s for firmware to boot..."
        Start-Sleep -Seconds 10

        $byteCount = Read-WordSwd -Ocd $ocd -Interface 0 -TargetCfg 'target/rp2350.cfg' -Address $addrCount
        if ($null -eq $byteCount) {
            $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
            Write-StageLog -Stage 'sanity' -Status 'failed' -DurationMs $durMs `
                -ErrorCode $EXIT_SANITY_FAILED `
                -ErrorMessage 'SWD read of g_uart_byte_count failed'
            Exit-Flash -Code $EXIT_SANITY_FAILED `
                -Message "Firmware flashed but not running - check bridge UART connection. (Could not read g_uart_byte_count over SWD; Display CPU may be hung or unpowered.)"
        }
        $routerVal = $null
        if ($null -ne $addrRouter) {
            $routerVal = Read-WordSwd -Ocd $ocd -Interface 0 -TargetCfg 'target/rp2350.cfg' -Address $addrRouter
        }
        $routerStr = if ($null -ne $routerVal) { ('0x{0:x8}' -f $routerVal) } else { 'n/a' }
        Write-Host ("    g_uart_byte_count   = {0}" -f $byteCount)
        Write-Host ("    g_router_send_entry = {0}" -f $routerStr)

        $durMs = [long]((Get-Date) - $stageStart).TotalMilliseconds
        if ($byteCount -le 0) {
            Write-StageLog -Stage 'sanity' -Status 'failed' -DurationMs $durMs `
                -ErrorCode $EXIT_SANITY_FAILED `
                -ErrorMessage "g_uart_byte_count = $byteCount after 10s"
            Exit-Flash -Code $EXIT_SANITY_FAILED `
                -Message "Firmware flashed but not running - check bridge UART connection. (g_uart_byte_count = $byteCount after 10s; expected > 0.)"
        }
        Write-StageLog -Stage 'sanity' -Status 'success' -DurationMs $durMs
        Write-Host "    Sanity check PASSED." -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "DONE. Board $($Script:BoardSerial) is up." -ForegroundColor Green
    Exit-Flash -Code $EXIT_OK
}

# ---- -Loop driver -------------------------------------------------------

function Wait-ForNextBoard {
    param([string]$LastSerial)
    $timeoutSeconds = 300
    $sleepSeconds   = 2
    $elapsed = 0
    Write-Host ""
    Write-Host "READY FOR NEXT BOARD - swap board (last serial: $LastSerial). Timeout 5 min. Ctrl+C to exit." -ForegroundColor Cyan
    # First: wait for current board to disappear
    while ($elapsed -lt $timeoutSeconds) {
        $serials = @(Get-MultiprobeSerials)
        if ($serials -notcontains $LastSerial) { break }
        Start-Sleep -Seconds $sleepSeconds
        $elapsed += $sleepSeconds
    }
    if ($elapsed -ge $timeoutSeconds) {
        Write-Host "    Timed out waiting for the previous board to be unplugged." -ForegroundColor Yellow
        return $null
    }
    # Now: wait for a new board to appear
    $elapsed = 0
    while ($elapsed -lt $timeoutSeconds) {
        $serials = @(Get-MultiprobeSerials)
        $new = $serials | Where-Object { $_ -ne $LastSerial }
        if ($new -and $new.Count -gt 0) {
            # Give USB enumeration a beat to settle
            Start-Sleep -Seconds 2
            return ($new | Select-Object -First 1)
        }
        Start-Sleep -Seconds $sleepSeconds
        $elapsed += $sleepSeconds
    }
    return $null
}

# ---- entrypoint ---------------------------------------------------------

if ($Loop) {
    # In Loop mode we never let a single board's exit_code kill the process.
    # We wrap each iteration in a child pwsh? Simpler: rewire Exit-Flash to
    # `return` via a throw-based marker, but cleaner is to spawn a child.
    # To keep things dependency-free, we re-invoke ourselves per board.
    $self = $PSCommandPath
    if (-not $self) { $self = $MyInvocation.MyCommand.Path }
    if (-not $self) {
        Write-ErrorLine -Code $EXIT_PREREQ -Message "Could not resolve own script path for -Loop re-invocation."
        exit $EXIT_PREREQ
    }
    # Build the child arg list, stripping -Loop.
    $childArgs = @()
    if ($UseLocalBuilds)    { $childArgs += '-UseLocalBuilds' }
    if ($ForceUnlock)       { $childArgs += '-ForceUnlock' }
    if ($SkipMeshtastic)    { $childArgs += '-SkipMeshtastic' }
    if ($SkipSanityCheck)   { $childArgs += '-SkipSanityCheck' }
    if ($ArtifactsDir)      { $childArgs += @('-ArtifactsDir', $ArtifactsDir) }
    if ($JsonLog)           { $childArgs += @('-JsonLog', $JsonLog) }

    $lastSerial = ''
    while ($true) {
        if (-not $lastSerial) {
            $serials = @(Get-MultiprobeSerials)
            if ($Serial) {
                $useSerial = $Serial
            } elseif ($serials.Count -eq 1) {
                $useSerial = $serials[0]
            } elseif ($serials.Count -eq 0) {
                Write-Host "Loop: no multiprobe present, waiting..." -ForegroundColor Yellow
                $useSerial = Wait-ForNextBoard -LastSerial '__none__'
                if (-not $useSerial) {
                    Write-Host "Loop: timed out waiting for first board. Exiting." -ForegroundColor Yellow
                    exit $EXIT_HW_NOT_DETECTED
                }
            } else {
                Write-ErrorLine -Code $EXIT_HW_NOT_DETECTED `
                    -Message "Loop: $($serials.Count) probes present at startup. Pass -Serial or unplug all but one to seed the loop."
                exit $EXIT_HW_NOT_DETECTED
            }
        } else {
            $useSerial = Wait-ForNextBoard -LastSerial $lastSerial
            if (-not $useSerial) {
                Write-Host "Loop: no new board detected. Exiting." -ForegroundColor Yellow
                exit $EXIT_HW_NOT_DETECTED
            }
        }

        Write-Host ""
        Write-Host "================================================" -ForegroundColor Cyan
        Write-Host " LOOP: flashing board $useSerial" -ForegroundColor Cyan
        Write-Host "================================================" -ForegroundColor Cyan

        $thisArgs = @('-File', $self) + $childArgs + @('-Serial', $useSerial)
        & pwsh @thisArgs
        $rc = $LASTEXITCODE
        Write-Host ""
        if ($rc -eq 0) {
            Write-Host "Board $useSerial PASS (exit $rc)." -ForegroundColor Green
        } else {
            Write-Host "Board $useSerial FAIL (exit $rc)." -ForegroundColor Red
        }
        $lastSerial = $useSerial
    }
} else {
    Invoke-FlashOneBoard
}
