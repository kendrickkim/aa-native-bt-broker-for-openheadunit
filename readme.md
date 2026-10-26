# OpenHU-BT-Relay (ESP32)

[한국어](readme-kr.md)

A classic ESP32 sits between [OpenHeadUnit](https://github.com/andreknieriem/open-headunit) and an Android Auto phone. Like FYT BLINK/DUDUAUTO, it opens the Android Auto channel to the phone over classic Bluetooth RFCOMM, and relays those bytes to the head unit over BLE GATT write / indicate.

If that support is not yet merged into [open-headunit](https://github.com/andreknieriem/open-headunit), apply the code from https://github.com/kendrickkim/headunit-revived/tree/with-bt-aa-broker.

The ESP32 does not parse Android Auto (WPP) payloads. Bytes from the head unit go to the phone RFCOMM; bytes from the phone are indicated to the head unit. Handshake and WifiStart/WifiInfo stay on the head unit.

## Hardware

- ESP32 with Bluetooth Classic. ESP32-S3 has no Classic BT, so this firmware will not run on it.
- Assumes 4 MB flash. HFP, BLE, and RFCOMM do not fit the default 1.25 MB app partition, so the 3 MB `app0` from `huge_app_32mbit.csv` is used.
- Wi-Fi is turned off at boot. Projection IP and SSID are not joined by the ESP32; the head unit delivers them to the phone over the RFCOMM relay.

Hardware like the following can be used:

<img src="images/esp32.png" width="300">

## Visible names

| Path | Name |
|---|---|
| Classic Bluetooth | `OpenHU-BT-Relay` |
| BLE advertising | `OpenHU-BT-Relay-Setup` |
| SPP service | `AndroidAuto` |
| Pairing PIN | `0000` |
| HFP role | Hands-Free (HF) |

Already-paired devices may keep an old name. Remove them from the list and scan again for the new name to appear.

## Pair a phone (head unit + board)

Order matters. The head unit must save the board over BLE before the phone can Classic-pair.

1. On the head unit: enable Bluetooth relay (ESP32), **Scan**, select `OpenHU-BT-Relay-Setup`, and save it.
2. On the phone: forget any old `OpenHU-BT-Relay` entry (a leftover bond often blocks a new pair).
3. On the head unit: tap **Pair phone** (PIN **0000**). The board clears its phone bonds and stays discoverable for about 120 seconds.
4. On the phone: search and pair `OpenHU-BT-Relay` with PIN `0000` during that window.
5. If pairing mode ends immediately after step 3, tap **Pair phone** once more, then retry step 4.

## Flashing from release bins only (no toolchain)

You can write the firmware with only the shipped `.bin` files—no source or PlatformIO. Steps below are for Windows.

### What you need

- An **ESP32 board with Bluetooth Classic** (not ESP32-S3)
- USB cable to the PC
- The bin files from the release folder (table below)
- [ESP Flash Download Tool](https://www.espressif.com/en/support/download/other-tools) (official Espressif GUI)

When you first plug the board in, Windows creates a COM port. Check Device Manager → Ports (COM & LPT) for `COMx` (e.g. `COM17`).

### Release files and flash addresses

| File | Flash address | Role |
|---|---|---|
| `bootloader.bin` | `0x1000` | Bootloader |
| `partitions.bin` | `0x8000` | Partition table |
| `boot_app0.bin` | `0xe000` | OTA boot data |
| `firmware.bin` | `0x10000` | Broker application |

All four files are required. Flashing only one of them can leave the board unable to boot.

### Flash with Flash Download Tool

1. Start the tool and choose **ESP32** / **Developer Mode** / **SPIDownload**.
2. Add each file on its own row, with address and checkbox as below.

   | Check | File | Address |
   |---|---|---|
   | ✓ | `bootloader.bin` | `0x1000` |
   | ✓ | `partitions.bin` | `0x8000` |
   | ✓ | `boot_app0.bin` | `0xe000` |
   | ✓ | `firmware.bin` | `0x10000` |

3. Set **SPI SPEED** `40MHz`, **SPI MODE** `DIO`, **FLASH SIZE** `4MB` (for a 4 MB board).
4. Set **COM** to the port from Device Manager, and **BAUD** to `921600` or `460800`.
5. Click **START**. If write fails and the board has a BOOT/IO0 button, hold BOOT, press RESET, release both, then try again.
6. When you see `FINISH` / done, unplug and replug USB or press RESET to reboot.

After a successful flash, the phone/head-unit Bluetooth lists should show `OpenHU-BT-Relay` (classic) and `OpenHU-BT-Relay-Setup` (BLE). Pairing PIN is `0000`. Requires an original ESP32 with Bluetooth Classic (not ESP32-S2/S3/C3/C6).

### Command-line flash (optional)

If [esptool](https://github.com/espressif/esptool) is installed, from the release folder (change `COM17` to your port):

```bash
esptool.py --chip esp32 --port COM17 --baud 921600 write_flash -z ^
  0x1000 bootloader.bin ^
  0x8000 partitions.bin ^
  0xe000 boot_app0.bin ^
  0x10000 firmware.bin
```

On macOS / Linux use `\` instead of `^`, and a port like `/dev/ttyUSB0`.

## Build and flash (developers)

[PlatformIO](https://platformio.org/) is required.

```bash
pio run
pio run -t upload
pio device monitor
```

`upload_port` and `monitor_port` in `platformio.ini` are set to this development PC’s `COM17`. Change them for another board. Monitor baud is 115200.

If boot logs do not show `broker self-check failed`, the frame codec self-check passed.

For a release package, after `pio run` collect `bootloader.bin`, `partitions.bin`, and `firmware.bin` from `.pio/build/esp32dev/`, plus the framework’s `boot_app0.bin`.

## Sequence (BLINK mapping)

1. After boot, once the classic stack is ready, the board stays discoverable for about 180 seconds. The head unit’s bonding command can reopen that window.
2. When the head unit connects over BLE and sends `CMD_HELLO`, it gets `EVT_HELLO_ACK` and, if the stack is ready, `EVT_BT_READY`.
3. HFP is off at boot. It is armed only for a new-phone pairing window or for `CMD_HFP_ON` / main-compat `CMD_AA_ENABLE`. Phone-initiated HFP while suppressed is refused. (`CMD_SET_NETWORK` is optional; WPP is sent by the head unit.)
4. Phone HFP up → `EVT_PHONE_LINK` (≈ JH); Android Auto RFCOMM open → `EVT_RFCOMM_OPEN` (≈ AA). The head unit then sends WPP first. `CMD_HFP_OFF` drops HFP; `CMD_PROJECTION_UP` is status only.
5. Phone → `DATA_RFCOMM_RX` (≈ ZB) indicate; head unit → `DATA_RFCOMM_TX` (≈ AT#ZA) write.
6. The head unit tops up RX credit with `EVT_CREDIT`. Without credit, phone bytes are dropped.
7. If head-unit BLE stays down for 10 seconds, phone RFCOMM is closed and the relay is torn down.

## Serial log

- `rx frame` / `tx frame ... | …`: logical frame summary (type, length, meaning)
- Payload hex is off by default. Add `build_flags = -DBROKER_LOG_DATA_BYTES=1` in `platformio.ini` when you need it.

HFP voice (SCO) is not logged.

## Source layout

| Directory | Role |
|---|---|
| `src/main.cpp` | Startup and `broker_poll` loop |
| `src/protocol` | Wire protocol and frame codec |
| `src/broker` | Queues, command handling, RFCOMM↔BLE relay |
| `src/ble` | Head-unit GATT server (write / indicate) |
| `src/classic` | RFCOMM, Android Auto SDP, HFP HF |
| `src/wpp` | Former on-device WPP (unused at runtime, kept) |

Wire constants live in `src/protocol/broker_protocol.h`. Constraints for later changes are in `agent.md`.
