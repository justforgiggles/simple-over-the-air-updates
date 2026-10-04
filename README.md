# Simple over-the-air updates

ESP-32U (original ESP32) firmware that updates itself from this public GitHub repository. Build
with PlatformIO, commit the application and checksum, and push. No update server
or deployment service is needed.

## Build and publish

```sh
cp firmware/include/config.example.h firmware/include/config.h
# Edit config.h: Wi-Fi credentials and (for a fork) BUNDLE_URL.
pio run
python3 tools/test_bundle.py
python3 tools/test_audio.py
```

Edit `firmware/src/application.cpp` for your application. `applicationSetup()` runs
once and `applicationLoop()` runs repeatedly, separately from the updater task.
Each call must return within the 30-second watchdog period.
The application streams an INMP441 microphone continuously to
`sonic.barenderasmus.com:9000` over plain TCP, using Sonic Bridge's SB02 protocol:
16 kHz mono μ-law, 320 samples (20 ms) per frame. Silence is transmitted too.
The relay accepts one source at a time; stop any other source before testing.

Wire the microphone to the **original ESP32/ESP-32U**, not an ESP32-C3:

| INMP441 | ESP32 |
| --- | --- |
| VDD | 3.3V |
| GND | GND |
| L/R | GND (left channel) |
| SCK | GPIO32 |
| WS | GPIO25 |
| SD | GPIO33 |

Capture and TCP transmission run in separate tasks. The 16-frame queue holds
at most 320 ms of audio and drops oldest frames when full. Connection attempts
have a five-second TCP deadline and each record has a one-second write deadline;
DNS uses lwIP's bounded retries. Failed connections retry after one second,
resolve the hostname again, and discard buffered audio before resuming.
Wi-Fi connection attempts run every five seconds independently of OTA, and audio
does not require clock synchronization. Power saving is disabled for lower latency.
These bounds limit device buffering; listener and network buffering also affect
end-to-end delay, which must be measured on hardware.

`firmware/src/application.cpp` contains the relay address, pins, and `MIC_SHIFT`
(default 14). Lowering the shift increases microphone gain; raise it if samples
clip. Serial logs report sent/dropped frames and the latest frame's peak every
five seconds (32768 is full scale). I2S failures retry after five seconds while
the updater stays available. No LED output is configured by this application.

Every `pio run` exports these files, ready to commit:

- `bundle/firmware.bin`: the application image, at most `0x1E0000` bytes.
- `bundle/firmware.sha256`: exactly 64 lowercase SHA-256 digits and a newline.

After reviewing your changes, commit the source and both bundle files together
and push to `main`. For example, when only application code changed:

```sh
git add firmware/src/application.cpp bundle/firmware.bin bundle/firmware.sha256
git commit -m "Publish firmware update"
git push origin main
```

The build does not commit or push automatically. Keep the binary as a normal Git
file, **not Git LFS**. Devices use:

```text
https://raw.githubusercontent.com/justforgiggles/simple-over-the-air-updates/main/bundle/firmware.sha256
https://raw.githubusercontent.com/justforgiggles/simple-over-the-air-updates/main/bundle/firmware.bin
```

The configuration header is ignored by Git, but credentials are compiled into the
public binary and can be extracted. The installer and each published application
must have credentials for the destination Wi-Fi. Changing credentials in the
installer does not change those already compiled into a downloaded application.
A build without `config.h` uses placeholders and prints a warning; rebuild with
real credentials before using it on a device.

To publish a previously built version, restore **both** bundle files from its
commit, commit that change, and push. Do not rebuild afterward: a build exports
the current application. A → B → A is supported; versions are compared by content,
not by age or a version counter.

## Customer installation: one file, Arduino IDE

Send the customer **`bootstrap.cpp`**. It is self-contained when built by Arduino;
no files from this repository or third-party libraries need to accompany it.

1. Install the Arduino IDE and **esp32 by Espressif Systems, version 3.1.3**, through
   Boards Manager. If needed, add this Additional Boards Manager URL:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
2. Create a new sketch, paste the complete contents of `bootstrap.cpp` over the
   default sketch, and save it as `Bootstrap` (creating `Bootstrap/Bootstrap.ino`).
3. Edit `WIFI_SSID`, `WIFI_PASSWORD`, and, for a fork, `BUNDLE_URL` near the top.
   Verify the hosted application was built with suitable credentials too.
4. Select **ESP32 Dev Module**, **4MB flash**, **DIO**, **40MHz flash frequency**,
   and **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**.
   For initial provisioning over an existing sketch, select **Erase All Flash
   Before Sketch Upload: Enabled**; this clears previous application and OTA state.
5. Select the serial port and upload. Open Serial Monitor at **115200 baud**.

The installer connects to Wi-Fi, synchronizes time, downloads and verifies the
first application, then reboots into it. Failed attempts retry every 60 seconds.
Clock synchronization starts after Wi-Fi connects and has its own 30-second
timeout, with the watchdog fed throughout the wait.
Publish a configured bundle before sending the installer. The installer is a
one-time application, not a permanent recovery bootloader; the downloaded
application provides subsequent updates and recovery.

If the installer prints `Installation incomplete; retrying in 60 seconds`,
read the preceding error at 115200 baud. It identifies Wi-Fi connection or NTP
clock sync failures, the failing HTTP URL/status, or download/flash validation
errors. These diagnostics work with Arduino's **Core Debug Level: None**.
For DNS/TLS failures, select **Core Debug Level: Verbose** and upload again to
include Espressif's connection diagnostics. HTTP 404 means the configured public
repository, branch or bundle file is unavailable; publish both bundle files.

Arduino's stock bootloader may return to the installer if the very first
application fails before reaching startup. The application confirms this first
handoff immediately. Subsequent updates are written by ESP-IDF with rollback
disabled; they do not require a trial boot or revert to an older application.

## Update and recovery behavior

The application checks the checksum at startup and 10 minutes after each attempt,
including in recovery mode. The installer attempts immediately once Wi-Fi and
time synchronization are ready; only failed installer attempts wait 60 seconds.
It hashes its running image directly, including the appended image digest, so
there is no separately saved version that can drift out of sync. When the hosted
checksum differs, it downloads to the inactive OTA slot and validates the chip,
image structure, exact length, and SHA-256 read back from flash before changing
the boot selection.

Both installer and application use the same transfer/verification code in
`bootstrap.cpp`. The application keeps its networking and recovery supervision
in `firmware/src/main.cpp`.

HTTPS verifies the hostname and certificate chain. The application uses ESP-IDF's
CA bundle; the installer embeds ISRG Root X1/X2, verified against GitHub's current
raw-content certificate chain. Refresh the installer roots if GitHub changes CA.
Devices require DNS and outbound NTP (UDP 123). Fresh connections respect DNS
cache expiry and do not pin GitHub's IP address.

Network operations have 10-second timeouts, firmware downloads a 120-second
transfer budget, and stalled tasks a 30-second watchdog. Ordinary failures retry
next cycle. GitHub/CDN caching can delay publication by several minutes. If the
checksum and binary temporarily come from different commits, verification fails
and the current application remains selected.

A panic or watchdog reset puts the application into persistent **updater-only
recovery**, skipping application setup and its loop. Publish different firmware
bytes to recover; the next installed application clears the recovery latch.
Recovery survives power loss. Republishing identical failed bytes leaves it in
recovery, while publishing any different version, including an older one, works.

Startup, credentials, networking, NVS, and updater code must remain functional.
Early startup faults, broken credentials, deliberate restart loops, or logic
errors that keep feeding the watchdog can require USB access. Interrupted
application downloads leave the active slot intact; boot selection uses ESP-IDF's
redundant OTA metadata.

## PlatformIO USB provisioning and compatibility

The `esp32u` PlatformIO environment uses the `esp32dev` board target: original
dual-core Xtensa ESP32, DIO flash at 40 MHz, and a 4 MB flash layout. The ESP-32U
module and VP/VN/34/35 pin labels in `image.png` identify this processor family;
the photo does not establish flash capacity. Confirm at least 4 MB on the actual
device before provisioning. Attach the external 2.4 GHz antenna to the module's
antenna connector for reliable Wi-Fi. The application uses Wi-Fi only; Bluetooth
features are not needed for OTA.

The application uses ESP-IDF 5.3.1 and the same `min_spiffs` layout as the Arduino
installer. Both slots are `0x1E0000` bytes; the updater checks the device's actual
layout before installation. Keep the bootloader/partition layout fixed across OTA
updates, and test framework changes against the bootloader already on devices.

Developers can provision directly with PlatformIO:

```sh
pio run --target erase    # First provisioning only; erases all device data.
pio run --target upload
pio device monitor
```

Devices flashed with this project's earlier custom partition layout must be
reprovisioned over USB. OTA updates do not rewrite partitions or the bootloader.
The current bundle targets the original ESP32 only. Earlier ESP32-C3 bundles are
not compatible with this module; the installer rejects the wrong build target
and the bundle tests reject ESP32-C3 images.

## Validation

```sh
pio run
python3 tools/test_bundle.py
```

The host tests require Python 3 and a C++ compiler. They compile the real checksum
parser, check malformed metadata, test bundle export/re-export and A → B → A,
reject empty/oversized exports, and validate the published image's chip, length,
embedded digest, and external checksum. They do not simulate on-device OTA.

To check the customer sketch using Arduino CLI:

```sh
arduino-cli core install esp32:esp32@3.1.3 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
mkdir -p /tmp/sotau-sketch/Bootstrap
cp bootstrap.cpp /tmp/sotau-sketch/Bootstrap/Bootstrap.ino
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashSize=4M,FlashMode=dio,FlashFreq=40 /tmp/sotau-sketch/Bootstrap
```

Physical-device acceptance remains necessary:

- Confirm flash capacity and microphone wiring; verify audio streams without
  affecting Wi-Fi or OTA checks.
- Install through Arduino, verify the first application starts, then reboot twice
  to check the initial confirmation. Repeat initial installation with power loss.
- Publish unchanged bytes, then A → B → A; verify only changed content installs.
- Break Wi-Fi, DNS, NTP, or HTTPS temporarily and restore them; verify retries.
- Serve malformed checksums, mismatched binary/checksum pairs, truncated or corrupt
  downloads, and oversized images; verify the current application stays bootable.
- Interrupt power during downloading and boot selection, then publish a new image.
- Publish an application that aborts or hangs in setup/loop, verify recovery
  persists across power loss, then publish a fixed image and recover remotely.

References: [Arduino sketches](https://docs.arduino.cc/arduino-cli/sketch-specification),
[Arduino OTA partition layout](https://github.com/espressif/arduino-esp32/blob/3.1.3/tools/partitions/min_spiffs.csv),
[ESP-IDF OTA](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32/api-reference/system/ota.html),
[ESP32 GPIO restrictions](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32/api-reference/peripherals/gpio.html).

## Audio hardware validation

- Listen through Sonic Bridge and verify intelligible continuous audio, including
  steady background sound, and latency below a few seconds on a healthy network.
- Disconnect/reconnect Wi-Fi, restart the relay, make DNS unavailable, and stall a
  TCP receiver. Confirm retries recover with fresh audio and bounded queue use.
- Check microphone peak levels for clipping and verify failed I2S initialization
  does not stop update checks. Check for ten-minute OTA checks during streaming,
  then install a new bundle and verify streaming resumes after reboot.

The host checks validate encoding/framing and the exported image; they cannot
validate microphone wiring, sound quality, network recovery, or measured latency.
