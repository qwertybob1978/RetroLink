# RetroLink Build Environment Setup

This guide installs the toolchains needed to build RetroLink's DOS and
Classic Macintosh applications on Windows 10/11. Build tools and generated
files are kept in `.local-tools` and `build`, which are ignored by Git.

## MS-DOS

RetroLink's 16-bit DOS executable is built with Open Watcom C/C++ V2.

1. Download the current 64-bit Windows C/C++ installer from the
   [Open Watcom V2 releases](https://github.com/open-watcom/open-watcom-v2/releases)
   into this project's `.local-tools\downloads` directory. The asset is named
   `open-watcom-2_0-c-win-x64.exe`; PowerShell can download it with:

   ```powershell
   New-Item -ItemType Directory -Force '.local-tools\downloads' | Out-Null
   curl.exe -L --fail `
     -o '.local-tools\downloads\open-watcom-2_0-c-win-x64.exe' `
     'https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/open-watcom-2_0-c-win-x64.exe'
   ```

2. Install it to a writable directory. An unattended installation from
   PowerShell installs the full compiler to this project's ignored tools
   directory:

   ```powershell
   $watcom = Join-Path (Get-Location) '.local-tools\watcom'
   & '.local-tools\downloads\open-watcom-2_0-c-win-x64.exe' `
       '-dFullInstall=1' "-dDstDir=$watcom" '-s' '-ns'
   ```

   The installer options are documented in
   [Open Watcom's setup reference](https://github.com/open-watcom/open-watcom-v2/blob/master/bld/setupgui/setup.txt).
   `-ns` leaves system environment settings unchanged.
3. Build the DOS executable:

   ```powershell
   $env:WATCOM = (Join-Path (Get-Location) '.local-tools\watcom')
   .\src\RetroLink.Dos\scripts\build.ps1
   ```

   The result is `src\RetroLink.Dos\build\RETROLNK.EXE`. The script adds
   Open Watcom's compiler and support directories to the current PowerShell
   process. Re-run the `$env:WATCOM` assignment in a new shell before building.

## Classic Macintosh

The Macintosh application is compiled with the Retro68 68K cross-toolchain in
Ubuntu on WSL 2.

1. Install WSL 2 and an Ubuntu distribution if they are not already installed.
2. In an Ubuntu WSL terminal, install Retro68's build prerequisites:

   ```bash
   sudo apt-get update
   sudo apt-get install -y \
     build-essential cmake libgmp-dev libmpfr-dev libmpc-dev \
     libboost-all-dev bison flex texinfo ruby git
   ```

3. From PowerShell, at the root of this project, build the Retro68 toolchain:

   ```powershell
   wsl.exe -d Ubuntu -- bash scripts/wsl/build-retro68.sh
   ```

   The script clones Retro68 and its submodules into `~/retro68-src` and builds
   its toolchain in `~/retro68-build`. This project currently needs only the
   68K compiler and C runtime; its helper skips the unneeded PowerPC target,
   target C++ standard library, and C++ sample applications.
4. Build the Macintosh application and disk image:

   ```powershell
   wsl.exe -d Ubuntu -- bash scripts/wsl/build-hello-mac.sh
   ```

   The app target is named `DesignerForm`; the resulting disk image is
   `.local-tools/retrolink-build/DesignerForm.dsk`.

The toolchain build can take a while. An emulator, ROM, and System 7 disk are
not required to compile the application. Only use system disks and ROMs you
are legally entitled to use.

### Retro68 build caveats

- Allow at least 20 GB of free space for the Retro68 checkout and temporary
  compiler build files. The toolchain build is CPU-intensive and can take
  tens of minutes.
- The current Retro68 GCC 16.1 source can fail while building target
  `libstdc++` for Newlib, with errors such as `std::acoshf` not declared.
  `scripts/wsl/build-retro68.sh` keeps the C++ compiler frontend needed by
  Retro68's CMake checks, but skips the target C++ standard library, the
  unused PowerPC target, and unrelated C++ sample applications. This is
  sufficient for RetroLink's C/Rez application; it does not provide a usable
  target C++ standard library or PowerPC compiler. Projects that need these
  require a compatible Retro68/GCC release or a fix for that upstream
  target-library build failure.
- If the build is interrupted, rerun the toolchain script from the project
  root. It resumes the existing checkout and build directory.
- When building from a Windows-mounted drive, WSL Make may report a small
  clock-skew warning. The build can still complete successfully; if incremental
  builds behave inconsistently, run `build-hello-mac.sh` from the project root
  in Ubuntu and pass a build directory on the Linux filesystem, for example
  `bash scripts/wsl/build-hello-mac.sh "" "$HOME/retrolink-build"`.
- Retro68 uses its open-source Multiversal Interfaces by default. Some APIs
  from Apple's Universal Interfaces are not implemented there. If compilation
  reports a missing Macintosh API, a separately and legally obtained copy of
  Apple's Universal Interfaces 3.x can be placed under
  `~/retro68-src/InterfacesAndLibraries`; see the Retro68 build output for the
  expected directory layout. No Apple interface files are bundled here.

## Optional DOS tests and emulation

The native DOS protocol tests additionally require MinGW GCC. DOSBox-X is
needed only to run the DOS executable in an emulator. These are not required
to build `RETROLNK.EXE`; the DOS project's
[README](../src/RetroLink.Dos/README.md) describes the optional tests, emulator,
and serial-port setup.
