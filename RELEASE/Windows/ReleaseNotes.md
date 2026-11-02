# RetroLink for Windows

## Development release

- Application version: **0.1**.
- Packaged as a self-contained, compressed, single-file `win-x64` executable.
- Includes the .NET runtime, WPF framework, and application dependencies in
  `RetroLink.exe`; no separate .NET runtime installation is required.
- Provides a LOCAL/REMOTE dual-pane file manager, serial connection controls,
  role handoff, filesystem operations, and file transfer.
- Connect at 9600 baud first; use the same baud rate at both endpoints.

Build or refresh the package with
[`publish-retrolink-release.ps1`](../../scripts/windows/publish-retrolink-release.ps1).
The generated executable is `RELEASE/Windows/RetroLink.exe`. This source-only
copy does not include prebuilt binaries.

This is a development package. Live interoperability coverage is documented in
the platform test notes; it is not a certification of every remote peer.
