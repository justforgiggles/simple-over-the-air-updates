"""Host checks of production batching, socket deadlines and retry backoff."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StreamingTests(unittest.TestCase):
    def test_transport(self):
        app = (ROOT / 'firmware/src/application.cpp').read_text()
        transport = app[app.index('bool waitWritable('):app.index('int connectRelay(')]
        batching = app[app.index('size_t readBatch('):app.index('void transmitTask(')]
        stubs = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#include <sys/select.h>
constexpr char TAG[] = "test";
template<typename... T> void logIgnored(T...) {}
#define ESP_LOGW(...) logIgnored(__VA_ARGS__)
#define ESP_ERROR_CHECK(x) assert((x) == 0)
uint64_t clockMs;
bool online = true, writable = true;
int feeds = 0;
std::vector<int> sends, waits;
std::vector<uint8_t> wire;
size_t sendIndex = 0;
int64_t milliseconds() { return clockMs; }
bool networkUp() { return online; }
int esp_task_wdt_reset() { ++feeds; return 0; }
int fakeSelect(int, fd_set*, fd_set*, fd_set*, timeval* timeout) {
    if (writable) return 1;
    clockMs += timeout->tv_usec / 1000;
    return 0;
}
int fakeSend(int, const uint8_t* data, size_t size, int) {
    assert(sendIndex < sends.size());
    int count = sends[sendIndex++];
    ++clockMs;
    if (count < 0) { errno = -count; return -1; }
    assert(static_cast<size_t>(count) <= size);
    wire.insert(wire.end(), data, data + count);
    return count;
}
#define select fakeSelect
#define send fakeSend
struct Frame { uint8_t audio[320]; };
int frames = 0;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(x) (x)
std::vector<int> arrivals;
size_t arrivalIndex = 0;
int xQueueReceive(int, Frame* frame, int timeout) {
    waits.push_back(timeout);
    assert(arrivalIndex < arrivals.size());
    int delay = arrivals[arrivalIndex++];
    clockMs += std::min(delay, timeout);
    if (delay > timeout) return 0;
    std::fill(frame->audio, frame->audio + 320, arrivalIndex);
    return pdTRUE;
}
'''
        checks = r'''
int main() {
    Frame batch[2];
    arrivals = {0, 40};
    assert(readBatch(batch) == 2 && clockMs == 40);
    assert(batch[0].audio[0] == 1 && batch[1].audio[0] == 2);
    arrivals = {0, 100}; arrivalIndex = 0; clockMs = 0;
    assert(readBatch(batch) == 1 && clockMs == 80);
    arrivals = {101}; arrivalIndex = 0; clockMs = 0;
    assert(readBatch(batch) == 0 && clockMs == 100);
    uint8_t payload[640];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = i % 251;
    sends = {100, -EAGAIN, -EINTR, 540}; clockMs = 0;
    assert(writeAll(1, payload, sizeof(payload)));
    assert(wire == std::vector<uint8_t>(payload, payload + sizeof(payload)));
    writable = false; clockMs = 0;
    assert(!writeAll(1, payload, sizeof(payload)) && errno == ETIMEDOUT);
    assert(clockMs == 3000 && feeds >= 30);
    online = false;
    assert(!writeAll(1, payload, sizeof(payload)) && errno == ENETDOWN);
    online = writable = true; sends = {100, -ECONNRESET}; sendIndex = 0;
    wire.clear();
    assert(!writeAll(1, payload, sizeof(payload)) && wire.size() == 100);
    // A new session begins from fresh bytes, not the previous partial payload.
    sends = {640}; sendIndex = 0; wire.clear();
    assert(writeAll(1, payload, sizeof(payload)) && wire.size() == 640);
    unsigned retry = 1000;
    for (auto expected : {2000U, 4000U, 8000U, 8000U}) {
        retry = nextRetry(retry); assert(retry == expected);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'stream.cpp'
            cpp.write_text(stubs + transport + batching + checks)
            binary = Path(directory) / 'stream'
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                            str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
