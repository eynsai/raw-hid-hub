# raw-hid-hub

This C script runs on your computer, acting as a switchboard that passes raw HID messages from one QMK-enabled device to another.

## Building the Program

This program should work on Windows, Linux, and MacOS, although it has not been tested on MacOS.
[HIDAPI](https://github.com/libusb/hidapi) and C11 are required.

### Building on Windows

1. Install [MSYS2](https://.www.msys2.org/).
2. Open a MINGW64 terminal.
3. `pacman -Syu` (twice!)
4. `pacman -S mingw-w64-x86_64-gcc`
5. `pacman -S mingw-w64-x86_64-hidapi`
6. `gcc -static -std=c11 -o raw_hid_hub.exe raw_hid_hub.c -O3 -lhidapi`

### Building on Linux

1. Open a terminal.
2. `sudo apt update`
3. `sudo apt install build-essential`
4. `sudo apt install libhidapi-dev` 
5. `gcc -std=c11 -o raw_hid_hub raw_hid_hub.c -O3 -lhidapi-hidraw`.

### Flags

| Flag                              | Description                                                                                       |
| --------------------------------- | ------------------------------------------------------------------------------------------------- |
| `STATS_INTERVAL_MS`               | How often to print stats when using `-v2`.                                                        |
| `QMK_RAW_HID_USAGE_PAGE`          | HID usage page for raw HID. You probably don't need to change this.                               |
| `QMK_RAW_HID_USAGE`               | HID usage for raw HID. You probably don't need to change this.                                    |
| `RAW_HID_HUB_COMMAND_ID`          | Command ID to identify messages that are intended for the hub. Change this if necessary.          |
| `USE_SLEEP_*`                     | If this is defined, the program sleeps after each iteration over HID devices, reducing CPU usage. |
| `USE_SMART_SLEEP_*`               | If this is defined, the program waits for a bit after the last message report before sleeping.    |
| `SLEEP_MILLISECONDS_*`            | Controls how long to sleep for when `USE_SLEEP_*` is defined.                                     |
| `SMART_SLEEP_WAIT_MILLISECONDS_*` | Controls how long to stop sleeping for when `USE_SMART_SLEEP_*` is defined.                       |
| `SECONDS_PER_ENUMERATION`         | Controls how much time passes between HID device enumerations.                                    |

## Verbosity

The `-v<VERBOSITY LEVEL>` argument can be supplied to control verbosity:

- `-v0`: Silence (default)
- `-v1`: Print initialization and error messages, as well as device information, registration, and unregistration
- `-v2`: Print statistics periodically
- `-v4`: Print all raw HID messages to and from the hub
- `-v8`: Print all raw HID messages between devices
- `-v16`: Print all raw HID messages that the hub is ignoring

These options can be combined by adding the respective numbers together. For example, `-v12` would print raw HID messages to and from the hub, as well as between devices.

## Reports

Use QMK's [Raw HID](https://docs.qmk.fm/features/rawhid) feature to send and receive reports.
If you're using VIA or VIAL, you may need to look into how these frameworks handle Raw HID.

#### Registration Report (device -> hub):
The hub never sends raw HID to any device that isn't "registered".
Devices can register by sending a registration report to the hub.
After a device initially sends a registration report, if the registration was successful, the hub will send a status report to all currently registered devices, including the newly registered device.
If an already-registered device sends another registration report, the hub will send a status report to only the device that sent the registration report. This allows registration reports to double as an "are you there" ping.
```
byte 0:         RAW_HID_HUB_COMMAND_ID (default 0x27)
byte 1:         DEVICE_ID_HUB (default 0xFF)
byte 2:         0x01
byte 3-32:      undefined
```

#### Unregistration Report (device -> hub):
Devices can unregister from the hub to stop being able to send and receive messages.
The hub will not respond to the device that sends this report.
However, after a device unregisters, all remaining registered devices will be sent a status report.
```
byte 0:         RAW_HID_HUB_COMMAND_ID (default 0x27)
byte 1:         DEVICE_ID_HUB (default 0xFF)
byte 2:         0x00
byte 3-32:      undefined
```

#### Status Report (hub -> device):
This report is sent in response to device registrations and unregistrations.
The report serves to tell the device what device ID it has been assigned, as well as how many other devices are registered, and what their IDs are.
Device IDs range from `0x00` to `0xFE`, inclusive. 
The device ID `0xFF` is reserved. In the context of byte `1` of any report, this value can be interpreted as the device ID belonging to the hub itself. 
In any other context, it can be interpreted as the lack of an assigned device ID.
```
byte 0:         RAW_HID_HUB_COMMAND_ID (default 0x27)
byte 1:         DEVICE_ID_HUB (default 0xFF)
byte 2:         device id assigned to the recipient device
bytes 3-32:     device ids of all other registered devices, or 0xFF for padding
```

#### Message Reports (device -> hub -> device):
When the hub recieves a reports of this form, it will check to ensure that the destination device ID ( byte `1`) corresponds to a registered device.
If so, it will replace byte `1` with the origin device ID, and pass the message along to the destination.
No action will be taken if the destination device ID does not correspond to a registered device.
```
byte 0:         RAW_HID_HUB_COMMAND_ID (default 0x27)
byte 1:         destination device id (origin device -> hub) OR origin device id (hub -> destination device)
bytes 2-32:     payload
```

#### Hub Shutdown Report (hub -> device):
The hub will send this report to every registered device upon termination of the program.
```
byte 0:         RAW_HID_HUB_COMMAND_ID (default 0x27)
byte 1:         DEVICE_ID_HUB (default 0xFF)
byte 2:         DEVICE_ID_UNASSIGNED (default 0xFF)
bytes 3-32:     undefined
```
