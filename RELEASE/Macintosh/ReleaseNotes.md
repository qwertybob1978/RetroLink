# RetroLink for Classic Macintosh

## Development release

- RetroLink application version: **0.1**.
- 68k application intended for Classic Mac OS and System 7-era hardware.
- Uses the built-in Modem (Serial A) or Printer (Serial B) port and selectable
  baud rates.
- The serial button toggles between **Close** and **Open**; Open uses the
  currently selected port and baud rate.
- The file panes are labeled LOCAL and REMOTE. The layout is sized for the
  512×384 Color Classic display.
- Supports volume and directory browsing, filesystem operations, role
  arbitration, progress reporting, and XMODEM-CRC transfers.

The current implementation is a development build. Validate port wiring and
baud-rate parity before connecting to another peer.

## Build and Install

Follow the [standalone Macintosh build guide](../../docs/macintosh-build.md).
Copy the resulting application to a legally obtained System 7 volume for an
emulator, physical Macintosh, or BlueSCSI. This source project does not bundle
MacStudioAgent, a system disk, or the former combined-image helper.
