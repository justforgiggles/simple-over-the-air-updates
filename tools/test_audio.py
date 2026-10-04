"""Compile and exercise the actual portable microphone conversion/wire code."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioTests(unittest.TestCase):
    def test_wire_and_microphone(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A C++ compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "audio.cpp"
            source.write_text(r'''
#include <cassert>
#include <cstring>
#include "sonic_wire.h"
int main() {
    uint8_t header[sonic::kHeaderBytes];
    assert(sonic::buildStreamHeader(header, sonic::kCodecMulaw, 16000, 320) == 12);
    const uint8_t expected[] = {'S', 'B', '0', '2', 1, 1, 0x40, 1, 0x80, 0x3e, 0, 0};
    assert(memcmp(header, expected, sizeof(expected)) == 0);
    uint8_t record[sonic::kRecordHeaderBytes];
    // A frame-aligned position beyond 32 bits, exercising all significant bytes.
    sonic::buildRecordHeader(record, false, 0x0001020304050640ULL);
    const uint8_t audio[] = {1, 0x40, 6, 5, 4, 3, 2, 1, 0};
    assert(memcmp(record, audio, sizeof(audio)) == 0);
    sonic::buildRecordHeader(record, false, 0);
    for (size_t i = 1; i < sizeof(record); ++i) assert(record[i] == 0);
    assert(record[0] == 1);

    assert(sonic::slotToSample(0, 14) == 0);
    assert(sonic::slotToSample(1234 * 16384, 14) == 1234);
    assert(sonic::slotToSample(-1234 * 16384, 14) == -1234);
    assert(sonic::slotToSample(INT32_MAX, 14) == INT16_MAX);
    assert(sonic::slotToSample(INT32_MIN, 14) == INT16_MIN);
    assert(sonic::slotToSample(INT32_MIN, 31) == -1);
    const int16_t samples[] = {0, 32124, -32124, INT16_MAX, INT16_MIN};
    const uint8_t codes[] = {0xff, 0x80, 0x00, 0x80, 0x00};
    uint8_t encoded[sizeof(codes)];
    assert(sonic::encodeFrame(samples, sizeof(codes), encoded) == sizeof(codes));
    assert(memcmp(encoded, codes, sizeof(codes)) == 0);

    // Independently decode every G.711 code and check its canonical encoding.
    for (int code = 0; code < 256; ++code) {
        const int inverse = (~code) & 255;
        const int magnitude = (((inverse & 15) * 8 + 132) << ((inverse >> 4) & 7)) - 132;
        const int16_t sample = (inverse & 128) ? -magnitude : magnitude;
        assert(sonic::encodeMulawSample(sample) == (code == 0x7f ? 0xff : code));
    }
}
''')
            executable = Path(directory) / "audio"
            subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "firmware/include"), str(source),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
