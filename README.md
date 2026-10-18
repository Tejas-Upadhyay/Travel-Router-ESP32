# ESP32 Travel Router

ESP-IDF firmware providing a private Wi-Fi network and IPv4 NAT to hotel Wi-Fi.
Based on Espressif's supported AP + station architecture:
https://github.com/espressif/esp-idf/tree/v5.5.1/examples/wifi/softap_sta

Detected board: ESP32-S3 revision v0.2, 16 MB flash, 8 MB embedded PSRAM.
The firmware uses internal RAM and does not require PSRAM configuration.

The project-local ESP-IDF installation can be used with `bash tools.sh build`
and `bash tools.sh -p /dev/cu.usbmodem5C931117371 -b 115200 flash`.

## Build and flash

Use **ESP-IDF v5.5.1** (not an Arduino sketch). Install it using Espressif's setup
guide: https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/get-started/

For a different board, identify the actual chip before choosing the target. The
USB serial adapter alone does not identify the ESP32 model. With the ESP-IDF
environment activated:

```sh
python -m esptool --port /dev/cu.usbmodem5C931117371 chip-id
```

Then, from this folder, use the detected chip target. This board uses `esp32s3`:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem5C931117371 flash monitor
```

Flashing replaces existing firmware. The detected Mac port may change after
unplugging. Use the board's USB-to-serial/COM connector for this port.

## Use

1. Join `Travel-CE4F25` with password `travel123`. The personal password is
   configured in `main/router.c` and also printed in the USB serial monitor.
2. Join that Wi-Fi from your phone or laptop. Keep the connection even if the
   device reports no internet yet.
3. Open **http://192.168.77.1**. Scan for hotel networks or type the exact name,
   enter its password (blank for open networks), and save.
4. The router restarts to apply settings. Rejoin the personal Wi-Fi. If DNS does
   not work, disconnect and reconnect so your device obtains the hotel's DNS.
5. If needed, visit **http://neverssl.com/** and complete the hotel's login.
6. Connect up to four devices to the same personal Wi-Fi. The router remembers
   the most recently configured hotel and reconnects after power loss.

For the next hotel, repeat step 3. Personal Wi-Fi credentials stay the same.
All devices on the personal Wi-Fi can manage the router, so share its password
only with trusted people. Settings are stored in unencrypted NVS; the personal
password is printed over USB at startup. There is no remote management or CORS.

## Practical limits

- This version targets 2.4 GHz Wi-Fi, open or WPA personal networks; enterprise
  Wi-Fi and 5 GHz-only hotels are not supported.
- One radio shares airtime between hotel Wi-Fi and your devices. Expect modest
  performance; this is a small travel router, not a high-throughput replacement.
- NAT commonly lets devices share one hotel connection, but captive-portal
  policies vary. This firmware cannot guarantee a single login for every hotel,
  bypass payment, or automatically accept a portal's terms.
- Connection status means the hotel supplied an IP address, not verified internet.
- Hotel DNS is advertised directly through DHCP. Rejoin personal Wi-Fi after
  switching hotels if a client keeps its old DNS lease.
- A hotel using the same `192.168.77.0/24` subnet conflicts with this version;
  change the three AP address assignments in `main/router.c` and the setup address
  in its Host check before building for that network.
- No VPN, IPv6 forwarding, automatic portal popup, multi-hotel profile list,
  or over-the-air updates in this first version.

## Verification

Built successfully with ESP-IDF v5.5.1 for ESP32-S3 on October 17, 2026, with no
compiler warnings. IPv4 forwarding and NAPT are enabled. The 852,384-byte app
fits the factory partition with 19% remaining. Flashed to the connected board at
115200 baud; bootloader, partition table, and application hashes were verified.
The browser JavaScript also passed `node --check`.
On-board boot verification passed: AP + station mode started, the DHCP server
started at `192.168.77.1`, the HTTP server registered its routes, and the app
remained running during a 15-second serial capture without a reset or panic.
Personal Wi-Fi is `Travel-CE4F25`, with the requested simple password `travel123`.

The following network flows still need checking before relying on it:

1. Boot with no saved hotel: personal Wi-Fi and setup page remain available.
2. Scan, save valid credentials, rejoin after restart, and browse from two devices.
3. Save an incorrect password: setup remains reachable for correction.
4. Power-cycle: personal password and hotel credentials survive.
5. Disconnect and restore the hotel AP: upstream reconnects within subsequent
   15-second retry intervals; personal setup remains reachable.
6. On a hotel captive portal, complete login and test browsing on both devices.
7. Attempt setup access using the router's hotel-side address: requests are denied.
