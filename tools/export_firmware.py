"""Export the application and checksum for committing to the public repository."""
from pathlib import Path
import hashlib

MAX_IMAGE_SIZE = 0x1E0000  # Arduino min_spiffs OTA slot size.


def export_bundle(binary, destination):
    data = Path(binary).read_bytes()
    if not 0 < len(data) <= MAX_IMAGE_SIZE:
        raise ValueError("Firmware must fit the 0x1e0000-byte OTA slot")
    destination = Path(destination)
    destination.mkdir(exist_ok=True)
    checksum = hashlib.sha256(data).hexdigest()
    for name, contents in (("firmware.bin", data), ("firmware.sha256", (checksum + "\n").encode())):
        path = destination / name
        if not path.exists() or path.read_bytes() != contents:
            path.write_bytes(contents)
    return checksum


if "Import" in globals():
    Import("env")

    def export_action(source, target, env):
        checksum = export_bundle(str(source[0]), Path(env.subst("$PROJECT_DIR")) / "bundle")
        print(f"Bundle ready to commit; SHA-256: {checksum}")

    # Also repairs a missing/stale export when the compiled binary is unchanged.
    bundle = env.Command(
        ["$PROJECT_DIR/bundle/firmware.bin", "$PROJECT_DIR/bundle/firmware.sha256"],
        "$BUILD_DIR/${PROGNAME}.bin", export_action,
    )
    env.AlwaysBuild(bundle)
    env.Depends("buildprog", bundle)
