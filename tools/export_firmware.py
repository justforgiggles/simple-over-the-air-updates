"""PlatformIO post-build action: export only the OTA application image."""
Import("env")

from pathlib import Path
import hashlib
import shutil


def export_firmware(source, target, env):
    binary = Path(str(target[0]))
    # Match the actual binary, not just the ELF section sizes, against the slot.
    if binary.stat().st_size > 0x1F0000:
        raise RuntimeError("Firmware binary exceeds the OTA slot size in partitions.csv")
    destination = Path(env.subst("$PROJECT_DIR")) / "dist"
    destination.mkdir(exist_ok=True)
    shutil.copyfile(binary, destination / "firmware.bin")
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    (destination / "firmware.sha256").write_text(digest + "  firmware.bin\n")
    print(f"OTA image: {destination / 'firmware.bin'}; ETag: \"{digest}\"")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", export_firmware)
