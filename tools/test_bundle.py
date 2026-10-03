"""Host checks for the real checksum parser, bundle export, and image boundaries."""
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from export_firmware import export_bundle, MAX_IMAGE_SIZE

ROOT = Path(__file__).resolve().parents[1]


def validate_image(data):
    if not 32 <= len(data) <= MAX_IMAGE_SIZE:
        raise ValueError("invalid image size")
    if data[0] != 0xE9 or struct.unpack_from("<H", data, 12)[0] != 5 or data[23] != 1:
        raise ValueError("expected ESP32-C3 image with appended SHA-256")
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("expected application image, not bootloader")
    offset = 24
    for _ in range(data[1]):
        if offset + 8 > len(data):
            raise ValueError("truncated segment")
        size = struct.unpack_from("<I", data, offset + 4)[0]
        offset += 8 + size
    offset = (offset + 16) & ~15  # Image checksum/padding, then appended SHA-256.
    if offset + 32 != len(data) or hashlib.sha256(data[:offset]).digest() != data[offset:]:
        raise ValueError("truncated, corrupt, or padded image")


class BundleTests(unittest.TestCase):
    def test_real_checksum_parser(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A C++ compiler is needed for the parser check")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "parser.cpp"
            source.write_text('''
#include <cassert>
#include <string>
#define SOTAU_HOST_TEST
#include "bootstrap.cpp"
int main() {
    char output[65];
    std::string valid(64, 'a');
    valid += '\\n';
    assert(ota::parseChecksum(valid.data(), valid.size(), output));
    assert(std::string(output) == valid.substr(0, 64));
    for (auto bad : {std::string(), valid.substr(0, 64), valid + "x",
                    std::string(64, 'G') + "\\n", std::string(64, 'A') + "\\n",
                    std::string(64, 'a') + " ", std::string(64, 'a') + "\\r\\n",
                    std::string(64, 'a') + "  firmware.bin\\n"}) {
        assert(!ota::parseChecksum(bad.data(), bad.size(), output));
    }
    valid[4] = '\\0';
    assert(!ota::parseChecksum(valid.data(), valid.size(), output));
}
''')
            executable = Path(directory) / "parser"
            subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT), str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_export(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input.bin"
            destination = Path(directory) / "bundle"
            for contents in (b"A", b"B", b"A"):
                source.write_bytes(contents)
                checksum = export_bundle(source, destination)
                self.assertEqual(checksum, hashlib.sha256(contents).hexdigest())
                self.assertEqual((destination / "firmware.bin").read_bytes(), contents)
                self.assertEqual((destination / "firmware.sha256").read_bytes(),
                                 (checksum + "\n").encode())
            # No change must preserve the exported file, and missing metadata is repaired.
            timestamp = (destination / "firmware.bin").stat().st_mtime_ns
            (destination / "firmware.sha256").unlink()
            export_bundle(source, destination)
            self.assertEqual((destination / "firmware.bin").stat().st_mtime_ns, timestamp)
            for invalid in (b"", b"x" * (MAX_IMAGE_SIZE + 1)):
                source.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    export_bundle(source, destination)
                self.assertEqual((destination / "firmware.bin").read_bytes(), b"A")

    def test_published_image(self):
        data = (ROOT / "bundle/firmware.bin").read_bytes()
        validate_image(data)
        checksum = (ROOT / "bundle/firmware.sha256").read_bytes()
        self.assertEqual(checksum, (hashlib.sha256(data).hexdigest() + "\n").encode())
        for bad in (data[:-1], data + b"padding", data[:100] + b"broken" + data[106:]):
            with self.assertRaises(ValueError):
                validate_image(bad)


if __name__ == "__main__":
    unittest.main()
