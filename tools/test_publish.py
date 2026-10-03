"""Exercise the publication script locally; SSH and SCP are replaced with stubs."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PublishTest(unittest.TestCase):
    def test_atomic_publication(self):
        with tempfile.TemporaryDirectory(prefix="sotau-publish-") as directory:
            root = Path(directory)
            for name in ("tools", "dist", "releases", "bin"):
                (root / name).mkdir()
            script = root / "tools/publish.sh"
            shutil.copyfile(Path(__file__).with_name("publish.sh"), script)
            (root / "dist/firmware.bin").write_bytes(b"new firmware")
            published = root / "releases/firmware.bin"
            published.write_bytes(b"old firmware")
            stubs = {
                "ssh": '#!/bin/sh\nexec sh -c "$2"\n',
                "scp": '''#!/bin/sh
cp "$1" "${2#*:}"
if [ "${SOTAU_CORRUPT_UPLOAD:-0}" = 1 ]; then
    printf corrupt >> "${2#*:}"
fi
''',
                "sha256sum": '#!/bin/sh\nexec shasum -a 256 "$@"\n',
            }
            for name, content in stubs.items():
                path = root / "bin" / name
                path.write_text(content)
                path.chmod(0o755)
            environment = dict(os.environ, PATH=f"{root / 'bin'}:{os.environ['PATH']}")
            command = ["sh", str(script), "deploy@droplet", str(root / "releases")]

            failed = subprocess.run(command, env=dict(environment, SOTAU_CORRUPT_UPLOAD="1"),
                                    capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(published.read_bytes(), b"old firmware")
            self.assertEqual(list((root / "releases").iterdir()), [published])

            success = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(success.returncode, 0, success.stderr)
            self.assertEqual(published.read_bytes(), b"new firmware")
            self.assertEqual(published.stat().st_mode & 0o777, 0o644)
            self.assertEqual(list((root / "releases").iterdir()), [published])

            invalid = subprocess.run(command[:-1] + ["/bad;path"], env=environment,
                                     capture_output=True, text=True)
            self.assertEqual(invalid.returncode, 2)
            self.assertEqual(published.read_bytes(), b"new firmware")


if __name__ == "__main__":
    unittest.main()
