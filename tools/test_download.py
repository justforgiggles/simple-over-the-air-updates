"""Exercise the installer's actual read loop with a scripted HTTP transport."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DownloadTests(unittest.TestCase):
    def test_transient_timeout_and_deadline(self):
        source = (ROOT / "bootstrap.cpp").read_text()
        # Compile the production function unchanged, with only platform calls stubbed.
        read_body = source[source.index("int readBody("):source.index("bool fetchChecksum(")]
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "read.cpp"
            cpp.write_text(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <vector>
using esp_http_client_handle_t = int;
constexpr int ESP_ERR_HTTP_EAGAIN = 0x7007;
#define ESP_ERROR_CHECK(value) assert((value) == 0)
uint64_t clockMs;
int timeoutMs, feeds, reads;
std::vector<int> results;
uint64_t milliseconds() { return clockMs; }
int esp_task_wdt_reset() { ++feeds; return 0; }
void vTaskDelay(int ticks) { clockMs += ticks; }
void esp_http_client_set_timeout_ms(int, int ms) {
    assert(ms > 0 && ms <= 1000);
    timeoutMs = ms;
}
int esp_http_client_read(int, char* buffer, int size) {
    assert(reads < static_cast<int>(results.size()));
    const int result = results[reads++];
    if (result == -ESP_ERR_HTTP_EAGAIN) clockMs += timeoutMs;
    if (result > 0) {
        assert(result <= size);
        std::fill(buffer, buffer + result, 'x');
    }
    return result;
}
void reset(std::initializer_list<int> script) {
    clockMs = 0; feeds = reads = timeoutMs = 0; results = script;
}
''' + read_body + r'''
int main() {
    constexpr int again = -ESP_ERR_HTTP_EAGAIN;
    char buffer[8] = {};
    reset({again, again, 3});
    assert(readBody(1, buffer, 8, 10000) == 3);
    assert(reads == 3 && feeds == 3 && buffer[0] == 'x' && buffer[3] == 0);
    reset({again, again, again, 8});
    assert(readBody(1, buffer, 8, 2500) == again);
    assert(reads == 3 && feeds == 3 && timeoutMs == 498);
    assert(clockMs == 2501); // One final scheduler tick; never restart the budget.
    reset({0, 8});
    assert(readBody(1, buffer, 8, 10000) == 0 && reads == 1);
    reset({-1, 8});
    assert(readBody(1, buffer, 8, 10000) == -1 && reads == 1);
    reset({8});
    assert(readBody(1, buffer, 8, 0) == again && reads == 0);
    reset({2, again, 6});
    assert(readBody(1, buffer, 8, 10000) == 2);
    assert(readBody(1, buffer + 2, 6, 10000) == 6 && reads == 3);
}
''')
            executable = Path(directory) / "read"
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
        self.assertEqual(source, (ROOT / "version-2.cpp").read_text())


if __name__ == "__main__":
    unittest.main()
