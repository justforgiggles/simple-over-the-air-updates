# Simple over-the-air updates

ESP32-C3 firmware built with PlatformIO, plus a Go service serving one application
binary. The device checks `https://sotau.barenderasmus.com/firmware` at startup and
then 60 seconds after each completed attempt. HTTPS terminates at your existing
Traefik installation on a DigitalOcean Droplet.

The firmware uses ESP-IDF 5.3.1 instead of Arduino so its bootloader can be built
with **rollback and anti-rollback disabled**. Application code lives in
[`firmware/src/application.cpp`](firmware/src/application.cpp).

## Build and flash the ESP32-C3

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
or the PlatformIO VS Code extension. Run these commands from the repository root:

```sh
cp firmware/include/config.example.h firmware/include/config.h
# Edit config.h: Wi-Fi SSID/password, HTTPS URL, and optional timing settings.
pio run
```

`config.h` is ignored by Git. Credentials are hardcoded into the compiled binary,
as requested. A build without this file uses placeholders and prints a warning;
configure real credentials before flashing or publishing.

The build produces:

- `dist/firmware.bin`: the application binary to publish for OTA.
- `dist/firmware.sha256`: its SHA-256 checksum (also the ETag, enclosed in quotes).
- `.pio/build/esp32c3/`: bootloader, partition table, application, and debug ELF.

The target is `esp32-c3-devkitm-1`, 4 MB flash, with UART serial at 115200 baud.
For **first provisioning**, erase any previous partition/OTA state, then upload:

```sh
pio run --target erase     # First provisioning only: clears the entire flash.
pio run --target upload
pio device monitor
```

Use `--upload-port /dev/your-serial-port` if automatic detection is ambiguous.
The initial USB upload installs the bootloader and partition table. Subsequent
OTA updates replace only the application. Keep the partition layout fixed;
bootloader, partition, or framework changes need separate compatibility testing
and may require another USB provisioning.

Both OTA slots are `0x1f0000` bytes (1,984 KiB). The build checks image fit.
Edit `applicationSetup()` and `applicationLoop()` for your application. The main
loop yields between calls, and the updater runs in its own FreeRTOS task. Each
application call must finish within 30 seconds; a blocked call triggers recovery.
ESP-IDF APIs are available; these are not Arduino `setup()`/`loop()` callbacks.

## Deploy Go with Docker and Traefik

Copy this repository to the Droplet, with Docker Engine, Compose, and Traefik
already installed. Point your domain's DNS record at the Droplet. Traefik must
have a working HTTPS entrypoint and ACME certificate resolver, with ports 80/443
configured as needed for your ACME challenge.

On the Droplet, from the project directory:

```sh
cp .env.example .env
# Set the existing Traefik network, entrypoint, resolver, and hostname in .env.
mkdir -p releases
chmod 755 releases
docker compose up -d --build
```

Defaults are network `traefik`, entrypoint `websecure`, resolver `letsencrypt`,
and hostname `sotau.barenderasmus.com`. The Compose project joins the existing
external network; it does not install a second Traefik. Go listens on container
port 8080 without exposing a host port. The container runs as UID/GID 65532 with
a read-only filesystem and a read-only mount of the **releases directory**.

Until you publish a binary, the endpoint returns `503`. No database, firmware
history, upload API, or server-side version counter is needed. Treat this URL as
a public download: anyone with access can download the binary, which contains
the compiled Wi-Fi credentials. Use dedicated device Wi-Fi credentials.

The ESP32 verifies the hostname and certificate chain with ESP-IDF's bundled
root certificates. Traefik should serve a publicly trusted certificate and its
full chain. Devices need DNS and outbound NTP (UDP 123) for certificate date
validation; they retry if time is unavailable. Never disable TLS verification.
Update the certificate bundle/toolchain before its roots become obsolete.

## Publish an update

After changing application code, build locally and publish over SSH:

```sh
pio run
./tools/publish.sh deploy@your-droplet /opt/sotau/releases
```

The remote directory must be the directory mounted by Compose, writable by your
SSH user and traversable by the container user. The script requires local
Python 3, SSH/SCP, and remote `mktemp`/`sha256sum`. It uploads to a unique temporary
file, checks its checksum, sets readable permissions, then atomically renames it
to `firmware.bin`. Downloads keep seeing complete files. There is no service
restart. Do not overwrite the live file in place.

Publishing an older `firmware.bin` works too: ETags are compared for equality,
not ordered. To publish an archived image, put it in `dist/firmware.bin` and run
the publication script. A sequence A → B → A is supported; identical bytes have
the same ETag and do not trigger another installation.

Only publish `firmware.bin`, not a merged flash image or bootloader binary.

## Endpoint and update behavior

| Request | Response |
| --- | --- |
| `OPTIONS /firmware` | `204`, strong `ETag` containing the quoted lowercase SHA-256 of the complete binary |
| `GET /firmware` | `200`, binary bytes, matching `ETag`, `Content-Length`, and `application/octet-stream` |
| `GET` with stale `If-Match` | `412`; device checks again next cycle |
| Missing, empty, unreadable, or oversized file | `503` |
| Unsupported method / other path | `405` / `404` |

Responses use `Cache-Control: no-store`. The device sends the ETag obtained with
`OPTIONS` as `If-Match` on `GET`, then verifies the received ETag, size, ESP32-C3
image structure, and SHA-256 of the bytes read back from flash before selecting
the new boot partition. Truncated, corrupt, or mismatched downloads are discarded.
Do not configure Traefik middleware that caches, compresses, redirects, or rewrites
this endpoint.

The device hashes its running image at boot, including the image's appended
digest, so no separately persisted ETag can become stale after power loss or a
USB flash. The server reads a bounded snapshot for each request so headers and
body always refer to the same publication.

Connection/read timeouts default to 10 seconds, download duration to 120 seconds,
and watchdog timeout to 30 seconds. Wi-Fi association and initial time waits are
bounded. A fresh HTTPS client resolves the hostname each attempt using normal
DNS TTL caching; it does not pin an IP address. DNS changes become visible after
upstream/device cache expiry. Ordinary network failures retry next cycle. A task
that stalls beyond the watchdog timeout resets the device.

## Recovery and its limits

Normal operation feeds the application watchdog only after application code
returns. The updater feeds its own watchdog independently. After a panic or
watchdog reset, startup saves the failed image's identity in NVS and enters
**updater-only recovery mode**, skipping application setup and its loop. That
mode survives power loss and continues polling while the server is unavailable.

Publish different firmware bytes to recover. Once that image boots, the recovery
latch is cleared and the application runs again. There is no blacklist: an older
image may be published again later. Republishing exactly the currently failed
binary leaves the device in recovery because its ETag has not changed.

Downloads write to the inactive slot. Power loss before a verified update is
selected leaves the current application bootable; ESP-IDF's redundant OTA metadata
protects boot selection. The device never intentionally switches back to a
previous application after a crash.

This recovery depends on startup, networking, NVS, watchdog configuration, and
the updater remaining functional. Keep those parts intact across releases.
Broken credentials, early startup faults, flash corruption, repeated deliberate
software restarts, or application logic errors that keep feeding the watchdog
are not covered. Fixes for those cases may require USB access. This is not an
independent recovery image or a guarantee against every possible firmware bug.

## Checks

```sh
pio run
(cd server && go test -race ./... && go vet ./...)
python3 tools/test_publish.py
docker compose config --quiet
docker build -t sotau:local .
```

For local server testing (Go 1.25 or newer):

```sh
go -C server run . -addr=127.0.0.1:8080 -firmware=../dist/firmware.bin
curl -i -X OPTIONS http://127.0.0.1:8080/firmware
curl -f http://127.0.0.1:8080/firmware -o /tmp/sotau-download.bin
```

The Go tests cover endpoint status codes, ETags and body consistency, conditional
downloads, A → B → A, size limits, and replacement during a download.
The publication test uses local SSH/SCP stubs to check atomic replacement,
checksum failure, cleanup, permissions, and argument validation without a server.

Before deploying devices remotely, run this acceptance checklist on a physical
ESP32-C3. Compilation and HTTP tests cannot validate Wi-Fi, watchdog resets, or
flash behavior:

1. Flash A; publish A and verify `Firmware unchanged`. Publish B, verify its new
   application message and ETag after reboot, then publish A again.
2. Disable Wi-Fi, stop the service, and temporarily break DNS. Confirm the main
   application stays responsive, retries are logged, and updates resume once
   connectivity returns. Move DNS to another working server and wait for TTLs.
3. Serve a truncated download, a wrong digest/image, an oversized file, and a
   changed publication between OPTIONS/GET. Confirm no invalid image boots.
4. Cut power during download and near reboot. Confirm the device boots either
   the existing image or the fully verified replacement and can update again.
5. Publish a test image that calls `abort()` from `applicationLoop()`. Confirm
   the next boot logs `mode=recovery` and does not run application setup. Power
   cycle it and verify recovery persists. Publish a fixed image and verify normal
   execution resumes. Repeat with an application loop that never returns, and
   with a crash in `applicationSetup()`.
6. Keep the endpoint unavailable while in recovery, then restore it with a fixed
   release. Confirm remote recovery without USB access. Verify an invalid TLS
   certificate is rejected and a subsequently valid certificate works.

Architecture references: [ESP-IDF OTA](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32c3/api-reference/system/ota.html),
[watchdogs](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32c3/api-reference/system/wdts.html),
and [Traefik Docker routing](https://doc.traefik.io/traefik/v3.1/routing/providers/docker/).
