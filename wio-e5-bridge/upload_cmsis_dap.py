"""PIO extra script: upload via CMSIS-DAP iface 2 using the Pico SDK's OpenOCD.

PIO's ststm32 platform has no native cmsis-dap upload action, AND PIO's
bundled OpenOCD (xPack ~0.12.0-01004 from Jan 2023) is too old to support
the `cmsis-dap usb interface N` command needed to select interface 2 on
the FreeWili multiprobe.

The Pico SDK ships a newer OpenOCD with the multi-interface patches. Anyone
working on FreeWili already has the Pico SDK installed, so we use that.

Override the OpenOCD location with the PICO_OPENOCD_DIR env var if your
install lives somewhere non-standard.
"""
import glob
import os

Import("env")  # type: ignore  # noqa: F821 (provided by SCons/PIO)


def find_pico_openocd():
    override = os.environ.get("PICO_OPENOCD_DIR")
    if override:
        return override
    home = os.path.expanduser("~")
    base = os.path.join(home, ".pico-sdk", "openocd")
    if not os.path.isdir(base):
        return None
    versions = sorted(glob.glob(os.path.join(base, "*")))
    return versions[-1] if versions else None


openocd_dir = find_pico_openocd()
if not openocd_dir:
    raise RuntimeError(
        "OpenOCD not found. Install the Pico SDK (which ships a recent OpenOCD "
        "with CMSIS-DAP multi-interface support) or set PICO_OPENOCD_DIR to "
        "point at an OpenOCD install with the same patches."
    )

bin_name = "openocd.exe" if os.name == "nt" else "openocd"
openocd_bin = os.path.join(openocd_dir, bin_name)
openocd_scripts = os.path.join(openocd_dir, "scripts")

if not os.path.isfile(openocd_bin):
    raise RuntimeError(
        f"OpenOCD binary not found at {openocd_bin}. "
        "Check PICO_OPENOCD_DIR or your Pico SDK install."
    )

## Use the ELF, not the .bin. The .bin needs an explicit offset (STM32WL's
## main flash is at 0x08000000, not 0x00000000), and "program *.bin" with no
## offset silently writes nothing — OpenOCD just warns "no flash bank found
## for address 0x00000000" and reports success. The ELF carries section
## addresses, so no offset is needed.
elf_path = "$BUILD_DIR/${PROGNAME}.elf"

env.Replace(  # type: ignore  # noqa: F821
    UPLOADER=openocd_bin,
    UPLOADERFLAGS=[
        "-s", openocd_scripts,
        "-c", "adapter driver cmsis-dap",
        "-c", "cmsis-dap usb interface 2",
        "-c", "transport select swd",
        "-c", "adapter speed 500",
        "-f", "target/stm32wlx.cfg",
        "-c", "program {%s} reset exit" % elf_path,
    ],
    UPLOADCMD='"$UPLOADER" $UPLOADERFLAGS',
)
