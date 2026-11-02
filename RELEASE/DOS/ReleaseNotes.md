# RetroLink for MS-DOS

## Development release

- Application version: **0.1**.
- Real-mode 8088/8086 application built with Open Watcom.
- The text interface uses LOCAL and REMOTE panes; Tab switches panes.
- Insert marks or unmarks LOCAL files. F5 sends marked files to REMOTE one at a
  time; with no marks, F5 sends the current file.
- Supports serial role handoff, directory operations, XMODEM-CRC transfer,
  progress reporting, and parent-directory navigation.
- DOSBox-X 8086 self-test, native protocol tests, and isolated host-peer
  protocol tests pass at 9600 baud.

Live LOCAL/REMOTE CRUD and bidirectional transfer over the current COM33/COM34
mapping still require full end-to-end verification.
