# M4NT1S — ESP32-C5 + S3 Firmware

Portable wireless research firmware for the **LilyGO T-Dongle-C5**. The dongle acts as a sensor; a host machine (Windows, Linux, Android/Termux) drives it over USB-Serial-JTAG.

The ESP32-S3 variant is planned and will be added later. The C5 is the current target.

---

## Hardware

| | |
|---|---|
| MCU | ESP32-C5 (RISC-V, dual-band Wi-Fi 6, BLE 5) |
| Display | 0.96" ST7735, 80×160 native, driven via LovyanGFX |
| Storage | microSD over SPI (shared with LCD bus) |
| LED | APA102 status indicator |
| Input | BOOT button (GPIO 28), USB-Serial-JTAG, ANSI over serial |
| Framework | ESP-IDF 5.5.5 |

---

## Modules

- **Wi-Fi sniffer** — promiscuous management-frame capture. Tracks APs (SSID, BSSID, channel, RSSI, security flags) and clients (probe requests, association state). Streams both to the host as framed USB packets.
- **Handshake capture** — parses EAPOL M1–M4 from promiscuous data frames, classifies by key-info bits, writes a radiotap-prefixed PCAP to `/sdcard/handshakes/` for offline analysis in Wireshark.
- **BLE advertisement analysis** — parses Apple Continuity, Samsung Buds, and Fast Pair AD payloads via NimBLE.
- **Captive portal** — HTTP server plus DNS hijack for captive-portal detection testing on your own AP.
- **UI** — LVGL 9.6 flex layout, works in both 80×160 portrait and 160×80 landscape. Host can flip orientation over USB.
- **USB protocol** — length-prefixed binary frames with CRC-16, multi-packet types (`SCAN_AP`, `SCAN_CLIENT`, `HANDSHAKE`, `STATUS`, `CREDENTIAL`, `COMMAND`).

---

## Build
idf.py set-target esp32c5
idf.py build
idf.py -p COMx flash monitor


Requires ESP-IDF v5.5.5. Dependencies (`lvgl/lvgl ^9.6.0`) are pulled automatically by the component manager.

---

## Host script

`m4nt1s-nav.py` connects to the dongle and forwards arrow keys / Enter / Esc. On POSIX, Android (Termux + root), and Windows.



The host script also parses framed RX packets and logs to `Documents/`:

- `scan_aps.csv` — every AP seen
- `scan_clients.csv` — every client seen
- `handshakes.txt` — handshake captures with path to PCAP
- `portal.txt` — captive-portal submissions

---

## Known limitations

- **5 GHz scan requires Bluetooth to be stopped first.** Confirmed IDF bug ([IDFGH-15407](https://github.com/espressif/esp-idf/issues/16059)): `esp_wifi_scan_start()` returns `ESP_ERR_TIMEOUT` on 5 GHz when NimBLE is running. `deauth_scan_start()` calls `ble_spam_release_radio()` before scanning to work around it.
- **`esp_wifi_set_band_mode()` must be called after `esp_wifi_start()`.** Calling it before start returns `ESP_ERR_WIFI_NOT_STARTED`. The band-mode call is inside the `WIFI_EVENT_STA_START` handler.
- **C5 deauth reliability is inconsistent across IDF versions.** Works on some builds, has no effect on others. The `ieee80211_raw_frame_sanity_check` symbol is weak in IDF 5.5.5, so a strong override links in; downstream driver behaviour is still version-dependent.
- **No PSRAM.** The DMA-capable internal heap fragments after Wi-Fi init, which is why BLE bring-up does a precheck for a minimum contiguous block.

---

## Status

- C5 firmware: functional (sniffer, handshake capture, BLE analysis, UI, captive portal).
- S3 firmware: planned. The S3 has more headroom and a larger module set but no 5 GHz. The C5 is more constrained but has dual-band support.

---

## Notes

- USB-Serial-JTAG carries both ESP-IDF log output and framed binary packets. Close `idf.py monitor` before running the host script, or the parser will occasionally resync on log bytes.
- LVGL deprecation warnings for `lv_obj_add_flag` / `lv_obj_clear_flag` are cosmetic (LVGL 9.6) and do not affect behaviour.
- The C5 has no USB device controller, so the host script is the only path for credential forwarding.