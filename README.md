# raw-hid-hub

This C script runs on your computer, acting as a switchboard that passes raw HID messages from one QMK-enabled device to another.

## Building the software

This program should work on Windows, Linux, and MacOS.
[HIDAPI](https://github.com/libusb/hidapi) is required for compilation.

To compile with Windows Powershell, download the latest compiled release of HIDAPI binaries and place the files in `raw-hid-hub/hidapi`. Then run `cl raw_hid_hub.c -O2 /I"." /link "./hidapi.lib"`.

To compile on Linux, install HIDAPI first. Then run `gcc -o raw-hid-hub raw-hid-hub.c -lhidapi-hidraw`.

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

#### Registration Report (device -> hub):
The hub never sends raw HID to any device that hasn't "registered".
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
