# dfumac - Apple Silicon DFU Tool

This tool puts connected Apple Silicon devices into DFU (Device Firmware Update) mode using another Apple Silicon device running macOS and a standard USB-C cable.

## About

This project is a simplified fork of [macvdmtool](https://github.com/AsahiLinux/macvdmtool) by marcan and The Asahi Linux Contributors. While the original macvdmtool provides multiple VDM (Vendor Defined Message) commands including serial console access and device rebooting, this version focuses specifically on DFU mode functionality.

**Original Project:** [macvdmtool](https://github.com/AsahiLinux/macvdmtool) by marcan (@AsahiLinux)  
**Based on:** Portions of [ThunderboltPatcher](https://github.com/osy/ThunderboltPatcher) by osy86

## Copyright

* Copyright (C) 2019 osy86. All rights reserved.
* Copyright (C) 2021 The Asahi Linux Contributors
* This simplified version maintained separately.

Thanks to t8012.dev and mrarm for assistance with the VDM and Ace2 host interface commands.

## Requirements

* macOS running on Apple Silicon (M1, M1 Pro, M1 Max, M2, etc.)
* Two Apple Silicon devices
* A USB 3.0 compatible (SuperSpeed) USB-C cable (USB 2.0-only cables will not work)
* Xcode Command Line Tools installed
* Root/sudo access

## Building

1. **Install Xcode Command Line Tools:**
   ```bash
   xcode-select --install
   ```

2. **Clone or download this repository:**
   ```bash
   git clone <https://github.com/hacken01/dfumac.git>
   cd dfumac
   ```

3. **Build the program:**
   ```bash
   make
   ```

   This will create the `dfumac` executable.

4. **Clean build artifacts (optional):**
   ```bash
   make clean
   ```

## Usage

### Connecting Devices

Connect the two Apple Silicon devices via their DFU ports:
- **MacBook Air and 13" MacBook Pro:** Use the rear port
- **14" and 16" MacBook Pro:** Use the port next to the MagSafe connector
- **Mac Mini:** Use the port nearest to the power plug

**Important:** You must use a USB 3.0 compatible (SuperSpeed) USB-C cable. USB 2.0-only cables, including most charging cables, will not work as they lack the required pins. Thunderbolt cables also work.

### Running the Tool

Run the program as root:

```bash
sudo ./dfumac
```

The tool will:
1. Automatically detect connected HPM (High Power Management) devices
2. Attempt to unlock and enter DBMa mode on all available ports
3. Put each connected device into DFU mode

The tool processes all 5 available ports on each detected HPM device. If a device is already in DBMa mode, it will skip the unlock step and proceed directly to DFU mode.

### Example Output

```
Apple Silicon DFU Tool
This tool puts connected Apple Silicon devices into DFU mode.

Mac type: MacBookPro18,1
Looking for HPM devices...
Found: IOACPIPlane:/_SB/PCI0@0/XHC0@14
Device status: 0x0d
Found HPM device

=== Port 0 ===
Connection: Sink
Status: DBMa
Rebooting target into DFU mode... OK

=== Port 1 ===
...
```

## How It Works

The tool uses Apple's HPM (High Power Management) interface to communicate with connected devices over USB-C. It:

1. **Unlocks the device** using a key derived from the Mac's platform identifier
2. **Enters DBMa mode** (Debug Bridge Management mode) which provides low-level access
3. **Sends a VDM command** to put the target device into DFU mode

The DFU mode allows for firmware updates and low-level device manipulation, commonly used for bootloader installation (e.g., Asahi Linux) and development work.

## Troubleshooting

### "Did not get a reply to VDM"
- Ensure both devices are powered on
- Verify you're using a USB 3.0 compatible cable
- Try disconnecting and reconnecting the cable
- Make sure both devices are Apple Silicon (not Intel-based Macs)

### "Failed to enter DBMa mode"
- Try running the tool again
- Disconnect and reconnect the cable
- Ensure the target device is fully booted

### "No suitable devices found"
- Check that the cable is properly connected to the DFU port
- Verify the cable is USB 3.0 compatible
- Ensure both devices are Apple Silicon

### Segmentation Fault
- Ensure you've built the latest version
- Check that both devices are properly connected
- Try running with `sudo` if not already

## Limitations

This tool only puts devices into DFU mode. For other functionality like:
- Serial console access
- Device rebooting
- Other VDM commands

Please refer to the original [macvdmtool](https://github.com/AsahiLinux/macvdmtool) project.

## License

This project is licensed under the Apache License 2.0, same as the original macvdmtool.

## Disclaimer

This tool manipulates low-level device interfaces. Use at your own risk. The authors are not responsible for any damage to your hardware or software.

## Contributing

Pull requests and issues are welcome. When contributing, please ensure:
- Code compiles without warnings
- Memory management follows IOKit lifecycle rules
- Error handling is robust

## Acknowledgments

- **marcan** and **The Asahi Linux Contributors** for the original macvdmtool
- **osy86** for the ThunderboltPatcher foundation
- **t8012.dev** and **mrarm** for VDM and Ace2 interface assistance
