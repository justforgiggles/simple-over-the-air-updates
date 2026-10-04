#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

#include "application.h"
#include "sonic_wire.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define I2S_SCK 32
#define I2S_WS 25
#define I2S_SD 33

namespace {
constexpr char TAG[] = "audio";
constexpr char RELAY_HOST[] = "sonic.barenderasmus.com";
constexpr char RELAY_PORT[] = "9000";
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t FRAME_SAMPLES = 320;
// Lower shift increases gain. Tune against the reported peak (maximum 32768).
constexpr unsigned MIC_SHIFT = 14;
static_assert(MIC_SHIFT <= 31, "Invalid microphone shift");
constexpr size_t QUEUE_FRAMES = 16;
struct Frame {
    uint64_t position;
    uint8_t audio[FRAME_SAMPLES];
};
QueueHandle_t frames = nullptr;
TaskHandle_t capture = nullptr, transmitter = nullptr;
std::atomic<uint32_t> dropped{0}, sent{0}, peak{0};

int64_t milliseconds() { return esp_timer_get_time() / 1000; }

void pauseFor(unsigned ms) {
    const auto end = milliseconds() + ms;
    do {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (milliseconds() < end);
}

bool networkUp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    return netif && esp_netif_is_netif_up(netif) &&
           esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

void discardFrames() {
    Frame discarded;
    // Bounded even while the producer continues capturing.
    for (size_t i = 0; i < QUEUE_FRAMES; ++i) {
        if (xQueueReceive(frames, &discarded, 0) != pdTRUE) break;
        ++dropped;
    }
}

i2s_chan_handle_t startCapture() {
    i2s_chan_handle_t channel = nullptr;
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    config.dma_desc_num = 8;
    config.dma_frame_num = 512;
    esp_err_t err = i2s_new_channel(&config, nullptr, &channel);
    if (err == ESP_OK) {
        i2s_std_config_t standard = {};
        standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
        // Use target-aware defaults: ESP32 and C3 have different slot fields.
        standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
        standard.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
        standard.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        standard.gpio_cfg.bclk = static_cast<gpio_num_t>(I2S_SCK);
        standard.gpio_cfg.ws = static_cast<gpio_num_t>(I2S_WS);
        standard.gpio_cfg.dout = I2S_GPIO_UNUSED;
        standard.gpio_cfg.din = static_cast<gpio_num_t>(I2S_SD);
        err = i2s_channel_init_std_mode(channel, &standard);
        if (err == ESP_OK) err = i2s_channel_enable(channel);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S initialization failed: %s; retrying in 5s", esp_err_to_name(err));
        if (channel) i2s_del_channel(channel);
        return nullptr;
    }
    return channel;
}

void captureTask(void*) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    int32_t slots[FRAME_SAMPLES];
    uint64_t position = 0;
    for (;;) {
        i2s_chan_handle_t channel = startCapture();
        if (!channel) { pauseFor(5000); continue; }
        ESP_LOGI(TAG, "INMP441 capturing on SCK=%d WS=%d SD=%d", I2S_SCK, I2S_WS, I2S_SD);
        size_t pending = 0;
        for (;;) {
            ESP_ERROR_CHECK(esp_task_wdt_reset());
            size_t count = 0;
            const esp_err_t err = i2s_channel_read(channel,
                reinterpret_cast<uint8_t*>(slots) + pending, sizeof(slots) - pending, &count, 1000);
            if (err != ESP_OK || count == 0) {
                ESP_LOGW(TAG, "I2S capture failed: %s; restarting", esp_err_to_name(err));
                break;
            }
            pending += count;
            if (pending < sizeof(slots)) continue;
            pending = 0;
            Frame frame;
            frame.position = position;
            position += FRAME_SAMPLES;
            uint32_t framePeak = 0;
            for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
                const int sample = sonic::slotToSample(slots[i], MIC_SHIFT);
                frame.audio[i] = sonic::encodeMulawSample(static_cast<int16_t>(sample));
                const uint32_t magnitude = sample < 0 ? -sample : sample;
                if (magnitude > framePeak) framePeak = magnitude;
            }
            peak = framePeak;
            if (xQueueSend(frames, &frame, 0) != pdTRUE) {
                Frame discarded;
                if (xQueueReceive(frames, &discarded, 0) == pdTRUE) ++dropped;
                if (xQueueSend(frames, &frame, 0) != pdTRUE) ++dropped;
            }
        }
        i2s_channel_disable(channel);
        i2s_del_channel(channel);
        pauseFor(5000);
    }
}

// A single deadline covers all partial writes of a record, preventing a slow
// receiver from retaining stale audio indefinitely. Sockets stay nonblocking.
bool waitWritable(int socket, int64_t deadline) {
    while (networkUp()) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        const int64_t remaining = deadline - milliseconds();
        if (remaining <= 0) return false;
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socket, &writeSet);
        const int64_t waitMs = remaining < 100 ? remaining : 100;
        timeval timeout = {0, static_cast<suseconds_t>(waitMs * 1000)};
        const int result = select(socket + 1, nullptr, &writeSet, nullptr, &timeout);
        if (result > 0) return true;
        if (result < 0 && errno != EINTR) return false;
    }
    return false;
}

bool writeAll(int socket, const uint8_t* bytes, size_t size) {
    const auto deadline = milliseconds() + 1000;
    while (size) {
        if (!waitWritable(socket, deadline)) return false;
        const int count = send(socket, bytes, size, 0);
        if (count > 0) {
            bytes += count;
            size -= count;
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            return false;
        }
    }
    return true;
}

int connectRelay() {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    // lwIP DNS retries are bounded; resolve anew after every lost connection.
    if (getaddrinfo(RELAY_HOST, RELAY_PORT, &hints, &addresses) != 0) return -1;
    const auto deadline = milliseconds() + 5000;
    int connectedSocket = -1;
    for (const addrinfo* address = addresses; address; address = address->ai_next) {
        const int socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket < 0) continue;
        if (fcntl(socket, F_SETFL, O_NONBLOCK) < 0) { close(socket); continue; }
        const int result = connect(socket, address->ai_addr, address->ai_addrlen);
        bool ok = result == 0;
        if (!ok && errno == EINPROGRESS && waitWritable(socket, deadline)) {
            int error = 0;
            socklen_t length = sizeof(error);
            ok = getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
        }
        const int enabled = 1;
        if (ok) ok = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
        uint8_t header[sonic::kHeaderBytes];
        sonic::buildStreamHeader(header, sonic::kCodecMulaw, SAMPLE_RATE, FRAME_SAMPLES);
        if (ok && writeAll(socket, header, sizeof(header))) {
            connectedSocket = socket;
            break;
        }
        close(socket);
    }
    freeaddrinfo(addresses);
    return connectedSocket;
}

void transmitTask(void*) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    for (;;) {
        if (!networkUp()) { pauseFor(1000); continue; }
        const int socket = connectRelay();
        if (socket < 0) {
            ESP_LOGW(TAG, "Relay unavailable; retrying in 1s");
            pauseFor(1000);
            continue;
        }
        discardFrames();
        ESP_LOGI(TAG, "Streaming continuous 16kHz mono mu-law to %s:%s", RELAY_HOST, RELAY_PORT);
        auto lastFrame = milliseconds();
        while (networkUp()) {
            ESP_ERROR_CHECK(esp_task_wdt_reset());
            Frame frame;
            if (xQueueReceive(frames, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
                // Release the source slot if capture fails, rather than leaving
                // the relay holding a silent connection until its timeout.
                if (milliseconds() - lastFrame >= 2000) break;
                continue;
            }
            lastFrame = milliseconds();
            uint8_t record[sonic::kRecordHeaderBytes + FRAME_SAMPLES];
            sonic::buildRecordHeader(record, false, frame.position);
            memcpy(record + sonic::kRecordHeaderBytes, frame.audio, sizeof(frame.audio));
            if (!writeAll(socket, record, sizeof(record))) break;
            ++sent;
        }
        close(socket); // Never continue a partially written record on a new session.
        discardFrames();
        ESP_LOGW(TAG, "Stream disconnected; retrying in 1s");
        pauseFor(1000);
    }
}

void startTasks() {
    if (!frames) frames = xQueueCreate(QUEUE_FRAMES, sizeof(Frame));
    if (!frames) { ESP_LOGW(TAG, "Audio queue allocation failed; retrying"); return; }
    if (!capture && xTaskCreate(captureTask, "capture", 6144, nullptr, 10, &capture) != pdPASS) {
        capture = nullptr;
        ESP_LOGW(TAG, "Capture task allocation failed; retrying");
    }
    if (!transmitter && xTaskCreate(transmitTask, "transmit", 6144, nullptr, 5, &transmitter) != pdPASS) {
        transmitter = nullptr;
        ESP_LOGW(TAG, "Transmit task allocation failed; retrying");
    }
}
} // namespace

void applicationSetup() { startTasks(); }

void applicationLoop() {
    static int64_t lastStatus = 0;
    const auto now = milliseconds();
    if (now - lastStatus < 5000) return;
    lastStatus = now;
    startTasks();
    ESP_LOGI(TAG, "last_5s sent=%lu dropped=%lu peak=%lu", static_cast<unsigned long>(sent.exchange(0)),
             static_cast<unsigned long>(dropped.exchange(0)), static_cast<unsigned long>(peak.load()));
}
