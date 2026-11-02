# RetroLink for DOS

RetroLink DOS is a separate, real-mode DOS peer intended for an 8088/8086
machine profile in DOSBox-X. It uses COM1 for the MacStudio Agent debug
protocol and COM2 for the RetroLink file-manager protocol.

## Serial wiring

The default DOSBox-X configuration maps:

| DOSBox-X port | Windows port | Peer |
|---|---|---|
| COM1 | COM1 | DOS Agent debug link; the IDE uses COM2 |
| COM2 | COM33 | RetroLink file link; Basilisk II uses COM34 |

Use three independent null-modem pairs: COM1/COM2 for DOS Agent debugging,
COM31/COM32 for Mac Agent debugging, and COM33/COM34 for RetroLink file
operations. Basilisk II Serial A uses COM31 for the Mac Agent; Serial B uses
COM34 for RetroLink. Do not use one pair for both protocols.
`dosbox-x.conf` configures direct serial access; install a virtual
null-modem driver such as [com0com](https://com0com.sourceforge.net/) or
[VSPE](https://eterlogic.com/Products.VSPE_Download.html) and confirm that the
configured Windows ports are present before starting either emulator.

## Build and run

Install Open Watcom and set `WATCOM` to its installation directory, or make
`wcl.exe` available on `PATH`. Build from PowerShell:

```powershell
.\scripts\build.ps1
.\scripts\test-native.ps1
.\scripts\test-dosbox.ps1 -DosBoxPath 'C:\Program Files\DOSBox-X\dosbox-x.exe'
.\scripts\run-dosbox.ps1 -DosBoxPath 'C:\Program Files\DOSBox-X\dosbox-x.exe'
```

The output executable is `build\RETROLNK.EXE`, using an 8.3-compatible DOS
filename so it can be launched on classic DOS filesystems without long-filename
support.

The DOS program defaults to 9600 baud. A baud rate can be supplied on the DOS
command line; supported values are 9600, 19200, 38400, and 57600. Both ends of
each active serial link must use the same rate. The transfer link should first
be tested at 9600 baud.

The DOS target is compiled with Open Watcom's 8088/8086 code-generation option
and the 16-bit small memory model. DOSBox-X's lowest CPU profile is 8086; it
executes the same instruction subset generated for the 8088 target. The program
has no 186/386 instructions, protected-mode runtime, or Windows dependency. The
text UI uses the DOS console and BIOS UART registers.
Names shown by the DOS share and transferred to/from DOS must be printable
ASCII within the 8.3 filesystem limits.

Open a directory with Enter. Each pane includes a `..` parent entry while
inside a directory; Backspace also moves up in the active pane. Moving up from
a REMOTE volume root returns to the REMOTE volume list.

Use Tab to switch between the LOCAL and REMOTE panes. In the LOCAL pane, Insert
marks or unmarks the highlighted file for transfer; marked files display `*`.
Press F5 to send all marked files to the current REMOTE directory in list
order, one at a time. If no files are marked, F5 sends the highlighted LOCAL
file as before. Only regular files can be marked. Changing the LOCAL directory
clears its marks.

## Scope and compatibility

The RetroLink wire framing, paged list requests, role arbitration, and
XMODEM-CRC file stream match `ClassicMacIde.Core`. The DOS peer exposes its
mounted DOS directory as provider `0x80000001`, the existing RetroLink
host-share provider ID. File transfer works in both directions. The Mac sender
restricts DOS destination names to printable ASCII 8.3 names and the receiver
verifies the transferred length and CRC.

DOS-initiated create, rename, and delete operations have been verified against
the Mac peer, including rejecting deletion of a nonempty directory. Mac-UI
initiated CRUD against the DOS provider remains unverified.

The COM1 debug monitor speaks the existing MSAG handshake and streams DOS
status records to the IDE. Connect the IDE to Windows COM2 for the DOS debug
link. It is intentionally a diagnostic monitor, not a DOS application
uploader/launcher; unsupported Agent commands receive an explicit protocol
error. The IDE can use Windows COM32 for Mac Agent debugging while the Mac
RetroLink app uses Serial B on COM34.

## Verification status

The native protocol tests are independent of DOSBox and can run with MinGW GCC.
The DOSBox-X 8086 self-test has also passed. Live DOSBox-X/Basilisk II
interoperability was previously verified at 9600 baud using DOSBox-X COM2
through Windows COM32 and Basilisk II Serial A on Windows COM33. The current
three-pair configuration instead uses DOSBox-X COM2 through Windows COM33 and
Basilisk II Serial B on Windows COM34. The new mapping has completed a live
handshake at 9600 baud, and the DOS peer listed the Mac volumes. A
Mac-to-DOS transfer of `ReadMe.1st` was previously verified on the older
mapping; the full CRUD and bidirectional transfer suite has not yet been
repeated over COM33/COM34.

## Independent host-peer verification

Before testing DOS-to-Mac interoperability, each emulator peer can be tested
separately against the Python host simulator. The simulator checks handshake,
role handoff, directory listing, create/rename/delete, and a 257-byte
host-to-peer XMODEM-CRC transfer with length and CRC verification. It uses
unique temporary names and removes its test data.

Install `pyserial` if needed:

```powershell
python -m pip install pyserial
```

With RetroLink open in Basilisk II on Serial B/COM34, test the Mac peer from
the opposite end of the file-link pair:

```powershell
python .\scripts\windows\retrolink_host_test.py --target mac --port COM33
```

For DOS, start the simulator first on the host end of COM33/COM34, then press
F3 in DOSBox-X to connect:

```powershell
python .\scripts\windows\retrolink_host_test.py --target dos --port COM34
```

Run one emulator at a time; the Python process must own the unused endpoint of
the pair. Both the Mac and DOS peers have passed this suite at 9600 baud,
including handshake, role handoff, listing, create/rename/delete, and a
257-byte XMODEM-CRC receive with matching CRC32 `2E35B052`. These isolated
tests do not replace live DOS-to-Mac CRUD and bidirectional transfer tests.
