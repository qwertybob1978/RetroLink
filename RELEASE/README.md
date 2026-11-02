# RetroLink release packages

This directory contains platform-specific release notes. Generate the Windows
package using the publish command below; prebuilt binaries are not included.

| Platform | Status | Package or notes |
|---|---|---|
| Windows | Self-contained x64 build | [Windows release notes](Windows/ReleaseNotes.md) |
| Classic Macintosh | Development build for 68k System 7 | [Macintosh release notes](Macintosh/ReleaseNotes.md) |
| MS-DOS | Development build for 8088/8086 | [DOS release notes](DOS/ReleaseNotes.md) |
| Apple IIe | Work in progress; no release binary | [Apple IIe work in progress](AppleII/ReleaseNotes.md) |
| Commodore 64 | Work in progress; no release binary | [C64 work in progress](C64/ReleaseNotes.md) |

Build the Windows package from the repository root with:

```powershell
.\scripts\windows\publish-retrolink-release.ps1
```

The script publishes a self-contained, compressed, single-file `win-x64`
executable to `RELEASE\Windows\RetroLink.exe`. Apple IIe and C64 research and
implementation work remains in progress and is intentionally retained.
