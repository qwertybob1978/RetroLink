# Build RetroLink for Classic Macintosh

The complete generated C/Rez application is checked in under `macintosh/src`.
It can be compiled without MacStudio IDE. `macintosh/RetroLinkRemote.cmideproj`
is retained as the editable form definition; it is not required for compilation.
For Windows/WSL prerequisites and the current toolchain caveats, see
[Build Environment Setup](setup-environment.md).

Install WSL Ubuntu and the Retro68 prerequisites described by the toolchain
script, then run these commands from this project's root in PowerShell:

```powershell
wsl.exe -d Ubuntu -- bash scripts/wsl/build-retro68.sh
wsl.exe -d Ubuntu -- bash scripts/wsl/build-hello-mac.sh
```

The build helper uses the Retro68 toolchain under `~/retro68-build`. Its
default source directory is also `macintosh/src`. The Retro68 application
target remains named `DesignerForm`, so the build produces `DesignerForm.dsk`
and the corresponding application artifacts, not `RetroLink.dsk`.

Copy the application into your legally obtained System 7 disk using an HFS
tool, emulator, or target-machine workflow. No ROM, System 7 disk, emulator,
MacStudioAgent application, or IDE installation is bundled or required to
compile RetroLink. Serial testing needs a supported modem/printer serial port
or an emulator with serial passthrough.