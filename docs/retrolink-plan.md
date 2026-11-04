# RetroLink: Classic Mac and PC File Manager

**Status:** Approved specification. Directory browsing, control handoff, and
Windows-to-Mac XMODEM-CRC transfer are implemented and have been verified over
COM32/COM33. Transfer progress reporting is built on both peers but still needs
a fresh-emulator run; Mac-to-Windows transfer and filesystem mutations remain
planned.

**Date:** 2026-11-01

**Target link:** Windows COM32 ↔ emulator COM33, independent of the existing
MacStudio Agent link on COM30/COM31.

## 1. Purpose

Build a separate Windows WPF application named **RetroLink** and a Classic
Macintosh peer application built as a normal MacStudio `.cmideproj`. Together
they provide a two-computer file manager for a Windows PC and a System 7
Macintosh connected by a serial cable, a virtual null-modem pair, or an
emulator serial bridge.

The interface follows the familiar two-panel file-manager model: each panel
shows a directory and its files; a selection in one panel can be copied to
the other. The toolbar and keyboard shortcuts operate on the focused panel.
Either computer may own the active interface. Input on the non-active
computer requests control, making the peer active and the current owner
remote. In particular, the Mac application can own the interface and request
file operations against the Windows share.

RetroLink is a file manager, not a remote shell. It must not expose arbitrary
host paths, execute received files, or silently delete or overwrite data.
For guidance on implementing the protocol and product conventions on another
serial-equipped platform, see the [RetroLink platform porting guide](PORTING_GUIDE.md).

## 2. User-visible requirements

### Windows application

- Ship and run as a separate WPF process from ClassicMacStudio:
  `src/RetroLink/RetroLink.csproj`, launched with
  `dotnet run --project src/RetroLink/RetroLink.csproj`.
- Show a dual-pane file manager with a toolbar, path fields, active-panel
  highlight, selection, progress, and connection status.
- Let the user select a local shared root, a COM port, baud rate, and serial
  settings. The initial profile is 9600 baud, 8 data bits, no parity, one stop
  bit, and no flow control. Support the Mac Serial Manager rates through
  57,600 baud where the selected adapter supports them.
- Refresh the COM-port list, report port-open failures, and release the port
  on disconnect or application close.
- Make the active panel's **Copy** command copy to the opposite panel.

### Macintosh application

- Be a regular MacStudio project under
  `macintosh/`, built with the existing Retro68
  workflow and runnable in Basilisk II with Serial A/Modem connected to COM33.
- Provide the same two-panel workflow on supported monochrome and color
  profiles. Design for the Mac SE's 512 × 342 desktop and the Color Classic's
  512 × 384 desktop; the smaller screen is the layout constraint.
- Show which endpoint owns the active interface and allow the Mac user to
  request control. In remote mode, the application displays status and
  progress but does not issue file operations until control is granted.
- Use the Mac File Manager and a selected HFS volume/folder for Mac-side
  operations. Use the MacStudio serial component/runtime rather than opening
  the same serial drivers a second time.

### Link and roles

- Support both host-active and Mac-active sessions over the same connection.
- The active endpoint may list, create, rename, delete, and copy files on
  either endpoint, subject to each endpoint's authorization policy.
- A takeover request is explicit and visible. It waits while an indivisible
  file operation or transfer is in progress, then transfers ownership.
- A disconnected session disables all remote actions; it never falls back to
  an unverified remote result or reports a false success.

## 3. Interaction model

### Two-panel layout

Each panel contains a provider/volume label, current path, and a scrollable
list with name, kind, size, and modified time. Directories sort before files
by default. The `..` row navigates to the parent; Enter/double-click opens a
directory. A visible focus border identifies the panel whose selection
commands will use.

The top toolbar exposes **Connect**, **Disconnect**, **Take Control**,
**Refresh**, **Copy**, **Rename/Move**, **New Folder**, **Delete**, and
**Swap Panels**. A bottom function-key strip preserves familiar commander
muscle memory:

| Key | Command |
|---|---|
| Tab | Switch panels |
| Enter | Open directory or volume |
| Backspace | Parent directory |
| F5 | Copy selection to the other panel |
| F6 | Rename, or move when the destination is on the same endpoint |
| F7 | Create directory |
| F8 | Delete selection after confirmation |
| F9 | Menu |
| F10 | Disconnect/close session |

The WPF application may show more rows and metadata than the Mac, but both
endpoints use the same commands and panel semantics. On the Mac, use a compact
menu bar, short buttons, and list boxes; do not assume a mouse or color display.
The active endpoint labels panel contents from its own perspective, while the
peer can mirror the current directory and operation status.

### File operations

- `Copy` copies one or more selected files to the current directory in the
  opposite panel. A destination-name dialog allows an explicit rename.
- `Rename/Move` renames on one endpoint or moves within the same volume. A
  cross-endpoint move is copy-then-delete and deletes the source only after
  the destination has been verified.
- `New Folder` creates a single directory in the focused panel.
- `Delete` starts as non-recursive. Non-empty directories require a separate,
  explicit recursive-delete confirmation if recursive deletion is enabled
  later.
- Overwrite, delete, and host-side mutation requests originating from the Mac
  require a clear confirmation on the Windows host. The confirmation states
  the path and action.

## 4. Architecture

```text
Windows
  RetroLink WPF process
    Two-panel UI and active-role display
    Windows share-root picker and confined file provider
    Session / role coordinator / operation queue
    RLNK framing, request/response, transfer coordinator
    XMODEM-CRC sender/receiver
    System.IO.Ports -> COM32
                              |
                      COM32 <-> COM33
                              |
Macintosh
  RetroLinkRemote MacStudio project
    Toolbox two-panel UI and active-role display
    HFS volume provider using the File Manager
    Session / role client / operation dispatcher
    RLNK framing and bounded parser in C
    XMODEM-CRC sender/receiver using the MacStudio serial runtime
    Serial A / Modem
```

### Host components

- `RetroLink` is a standalone WPF app and does not add a RetroLink window or
  connection dependency to the main IDE.
- A testable host protocol/session layer owns one serial stream, performs
  framing and role arbitration, serializes requests, and prevents overlapping
  control and XMODEM traffic.
- A rooted Windows filesystem provider implements list/create/rename/delete
  and streaming reads/writes. The provider receives only a root selected by
  the user for the current session.
- The UI binds panels to endpoint-neutral directory models. The controller
  routes an action to the local provider or the remote peer based on each
  panel's endpoint identity; “active” identifies the controlling UI, not the
  machine whose files are local.

### Mac components

- `RetroLinkRemote.cmideproj` is the authoritative visual project. Its form
  provides controls and event bindings; C handlers implement the protocol and
  filesystem operations.
- The Mac app is a foreground application, not a DOS-style TSR. It owns the
  selected serial port for the session and services serial input through the
  generated MacStudio component/event loop.
- The filesystem provider uses volume references and directory IDs, not
  concatenated absolute paths. It maps protocol operations to safe, bounded
  File Manager calls. HFS names travel as UTF-8 on the wire and are converted
  to/from MacRoman; `/` remains a literal Mac filename character while `:`
  remains prohibited as the HFS path separator.
- The UI remains usable at 1-bit depth. Color is optional decoration and is
  never required to distinguish active focus, selected entries, or errors.

## 5. Session ownership and takeover

The Windows side is the **session coordinator**, not the permanent UI owner.
On successful HELLO it assigns a session ID and grants the initial active
token to the Windows endpoint. Only the current token holder may initiate
mutations, directory navigation mirrored to the peer, or transfers.

1. Any local key/button action on the remote endpoint sends
   `ROLE_REQUEST`; the WPF app makes the same request for its own input.
2. The coordinator increments a 32-bit role epoch and sends `ROLE_STATE` with
   the new owner endpoint ID. The other endpoint becomes REMOTE.
3. If both endpoints request within the same arbitration turn, the coordinator
   grants the token to the lexicographically lower 16-byte endpoint ID and
   reports that result to both. A later request can take control again.
4. An in-progress filesystem operation finishes or is cancelled before the
   grant. An XMODEM transfer is indivisible: takeover remains pending until
   completion, cancellation, or link timeout.
5. On link loss, both sides display disconnected state, cancel remote
   operations, and discard the active token. A new HELLO creates a fresh
   session and starts with Windows active.

The role is a UI/operation lease, not a security credential. It prevents
accidental simultaneous control but does not authenticate a peer on an
untrusted network. RetroLink v1 supports only direct serial or a trusted
virtual null-modem pair.

## 6. Version 1 serial protocol

### Transport

- Full-duplex serial, initially 9600 baud, 8-N-1, no flow control. Start
  emulator integration at 9600, then verify 57,600. A baud change is applied
  only after both endpoints acknowledge the target rate and a bounded
  reconnect interval is agreed.
- Each COM port has one owner. RetroLink uses COM32/COM33 and must never open
  or reconfigure the existing Agent ports COM30/COM31.
- Control frames are bounded and CRC-protected. The parser resynchronizes
  after noise one byte at a time and rejects bad version, reserved bits,
  excessive lengths, and invalid CRC before dispatch.

### Frame

All multi-byte values are big-endian. The fixed header is 30 bytes:

| Field | Bytes | Definition |
|---|---:|---|
| Magic | 4 | ASCII `RLNK` |
| Version | 1 | `1` |
| Type | 1 | Message type below |
| Flags | 2 | Reserved; must be zero |
| Session ID | 16 | Random value from the coordinator's HELLO |
| Request ID | 4 | Monotonically increasing per session; zero for unsolicited events |
| Payload length | 2 | `0..1024` |
| Payload | 0..1024 | Message-specific data |
| CRC | 2 | CRC-16/CCITT-FALSE over header and payload; high byte first |

Maximum encoded frame size is 1056 bytes. CRC-16/CCITT-FALSE uses polynomial
`0x1021`, initial value `0xFFFF`, no reflection, and xor-out `0x0000`. Unknown
message types are ignored or answered with a bounded `ERROR`; they never
change session or role state. Except for HELLO, stale session IDs are ignored.

### Message types

| Type | Name | Direction | Purpose |
|---:|---|---|---|
| `0x01` | `HELLO` | Host → Mac | Begin a version-negotiated session |
| `0x02` | `HELLO_ACK` | Mac → Host | Report selected version and capabilities |
| `0x03` | `ROLE_REQUEST` | Either → coordinator | Request the active token |
| `0x04` | `ROLE_STATE` | Coordinator → Either | Publish owner endpoint and role epoch |
| `0x05` | `HEARTBEAT` | Either | Detect stale/lost serial sessions |
| `0x10` | `LIST_REQUEST` | Active → provider | Enumerate one directory page |
| `0x11` | `LIST_PAGE` | Provider → active | Return bounded entries and continuation cursor |
| `0x12` | `MKDIR` | Active → provider | Create one directory |
| `0x13` | `RENAME` | Active → provider | Rename/move within one endpoint/volume |
| `0x14` | `DELETE` | Active → provider | Delete one file or empty directory |
| `0x15` | `OP_RESULT` | Provider → active | Return operation status and canonical name |
| `0x20` | `COPY_PREPARE` | Active → endpoints | Describe source, destination, length, and mode |
| `0x21` | `TRANSFER_READY` | Receiver → sender | Accept/reject and prepare a temporary file |
| `0x22` | `TRANSFER_COMPLETE` | Receiver → active | Report exact length, CRC, and commit status |
| `0x23` | `TRANSFER_CANCEL` | Either | Cancel a transfer at a safe boundary |
| `0x24` | `COPY_REQUEST` | Active host → target | Request a file from REMOTE to LOCAL |
| `0x25` | `COPY_REQUEST_RESULT` | Target → active host | Accept or reject a copy request |
| `0x7F` | `ERROR` | Either | Bounded error code, rejected request, and reason |

### Session and role payloads

- `HELLO` is host-generated and has a 29-byte payload: minimum version (u8),
  maximum version (u8), host application version major/minor/patch (three
  u8s), host endpoint ID (16 bytes), host capabilities (u32), and requested
  baud rate (u32). The host creates a new random session ID for every
  connection attempt.
- `HELLO_ACK` contains selected version (u8), Mac application
  major/minor/patch (three u8s), System major/minor (two u8s), machine type
  (u16), Mac endpoint ID (16 bytes), capability flags (u32), supported-baud
  flags (u32), maximum transfer size (u32), and root count (u8). It is
  followed by at most eight root descriptors: provider ID (u32), name length
  (u8), UTF-8 display name, and flags (u8). The selected version and session
  ID must match the HELLO request. Root display names are at most 64 UTF-8
  bytes; root flag bit 0 means read-only and all other bits are reserved.
- Endpoint IDs are generated once per process start from a random 16-byte
  value. Provider IDs are unique for one session and are not OS volume
  numbers. Root names are display-only; requests use provider IDs. Provider
  ID `0x80000001` is reserved for the Windows share root selected in RetroLink;
  Mac-assigned provider IDs use the remaining session IDs and must not collide.
- Capability bits are `0x00000001=LIST`, `0x00000002=MKDIR`,
  `0x00000004=RENAME`, `0x00000008=DELETE`, `0x00000010=XMODEM-CRC`,
  `0x00000020=MacBinary-II`, `0x00000040=role handover`, and
  `0x00000080=host-initiated REMOTE-to-LOCAL copy`. Unknown capabilities are
  ignored. The RT1170 target, Windows app, and Classic Mac peer implement the
  copy-request extension; the DOS peer does not. Supported-baud flag bits,
  low to high, represent 300, 600, 1200, 1800, 2400, 3600, 4800, 7200, 9600,
  14400, 19200, 28800, 38400, and 57600 baud.
- `ROLE_REQUEST` contains endpoint ID (16 bytes), expected role epoch (u32),
  and reason (u8: key, command, or explicit takeover). The Mac sends a request
  to the Windows coordinator; Windows UI input is applied locally by that
  coordinator.
- `ROLE_STATE` contains role epoch (u32), active endpoint ID (16 bytes), and
  pending-operation flag (u8). Epoch increments on every grant. All
  filesystem and copy requests include the epoch and active endpoint ID so
  the receiver can reject a stale or unauthorized request.
- Requests arriving during the same 50 ms arbitration window for one epoch
  are considered simultaneous; the lexicographically lower endpoint ID wins.
  A request arriving after a grant is a new takeover. This gives both UIs the
  same deterministic owner without relying on synchronized clocks.
- `HEARTBEAT` contains sender endpoint ID (16 bytes), role epoch (u32), and
  last completed request ID (u32). Missing three heartbeats marks the session
  disconnected; heartbeat interval is 1 second and timeout is 3 seconds.
  Heartbeats are suspended while XMODEM owns the stream; the transfer's
  retry/timeout rules determine connection failure during that interval.

### Path and listing payloads

- A `PathRef` is a 4-byte session-scoped provider/volume ID, a 1-byte
  component count (maximum 16), then length-prefixed UTF-8 path components.
  The root has zero components. Empty names, `.`/`..`, slash, backslash, and
  colon are invalid components; navigation is expressed by issuing a new
  `LIST_REQUEST` for the parent `PathRef`, never by trusting a raw path.
- Each component is at most 255 UTF-8 bytes and the complete `PathRef` is at
  most 512 bytes. A receiver validates normalization, component count, and
  bounds before passing any name to its filesystem provider.
- `LIST_REQUEST` contains `PathRef`, a 4-byte continuation offset, and a
  2-byte requested entry limit (1..32). `LIST_PAGE` returns the same offset,
  the next offset (or `0xFFFFFFFF` when complete), an entry count, and as many
  entries as fit in the frame. Each entry contains kind, attribute flags,
  name length/name, 8-byte data-fork size, 4-byte resource-fork size, and a
  4-byte modification time in seconds since 2000-01-01 UTC. The provider may
  return fewer entries than requested to respect the frame limit. With the
  maximum UTF-8 name length, no more than three entries fit in one page.
- Each `LIST_REQUEST` begins with role epoch (u32) and active endpoint ID
  (16 bytes), followed by `PathRef`, cursor (u32), and requested entry limit
  (u16). `LIST_PAGE` begins with result code (u16), cursor (u32), next cursor
  (u32), and entry count (u8). Each entry encodes kind (u8: file/directory),
  attributes (u16), name length (u8), UTF-8 name, data-fork size (u64),
  resource-fork size (u32), and modified time (u32). A failed list returns a
  nonzero result and no entries. The Mac uses provider ID `0x80000001` to
  request the currently selected Windows share; requests fail explicitly when
  no share root has been selected.
- A listing entry's Mac name is represented losslessly as UTF-8 on the wire.
  Since classic HFS names are limited to 31 MacRoman bytes, the Mac endpoint
  rejects an unrepresentable or overlength name for create/rename/copy rather
  than silently truncating it. The UI visibly marks host names that cannot be
  created on HFS.
- `MKDIR` contains role epoch (u32), active endpoint ID (16 bytes), parent
  `PathRef`, and one length-prefixed name.
- `RENAME` contains role epoch, active endpoint ID, source `PathRef`,
  destination-parent `PathRef`, and one length-prefixed new name. Both paths
  must resolve to the same endpoint and volume.
- `DELETE` contains role epoch, active endpoint ID, object `PathRef`, and a
  flags byte; v1 requires flags zero and refuses non-empty directories.
- `OP_RESULT` contains result code (u16), operation kind (u8), and optional
  canonical name (u8 length followed by UTF-8 bytes). Result codes are
  `0=success`, `1=not found`, `2=invalid path/name`, `3=permission denied`,
  `4=already exists`, `5=directory not empty`, `6=out of space`,
  `7=unsupported operation`, and `8=I/O error`. The request ID in the frame
  header identifies the operation.

### Copy and error payloads

- `COPY_REQUEST` contains role epoch (u32), active endpoint ID (16 bytes),
  source endpoint ID and `PathRef`, destination endpoint ID and parent
  `PathRef`, destination name (u8 length + UTF-8), and overwrite flag (u8).
  It requests a target-to-host transfer while the host owns control.
- `COPY_REQUEST_RESULT` is exactly one result code (u16), returned with the
  request ID. Result 0 means accepted; the target follows with `COPY_PREPARE`
  using that same request ID after reading and checksumming the source. Errors
  are 1=busy, 2=invalid request/source, 3=stale or unauthorized role, and
  4=source read or preparation failure.
- `COPY_PREPARE` begins with role epoch (u32) and active endpoint ID (16
  bytes), then source endpoint ID (16 bytes), source `PathRef`, destination
  endpoint ID (16 bytes), destination-parent `PathRef`, destination name
  (u8 length + UTF-8), stream length (u64), stream CRC-32/ISO-HDLC (u32),
  transfer format (u8: raw data or MacBinary II), overwrite flag (u8), and
  transfer mode (u8: `1` = XMODEM-CRC-128). A valid request ID is reserved
  until `TRANSFER_COMPLETE` or cancellation.
- `TRANSFER_READY` contains result code (u16), expected stream length (u64),
  expected CRC-32 (u32), transfer mode (u8), and receiver maximum block size
  (u16). It is sent only after the receiver has created its unique temporary
  file. A nonzero result rejects the transfer and leaves the destination
  unchanged.
- For a zero-length raw file, the sender skips the XMODEM byte stream. The
  receiver verifies length zero and CRC-32 of the empty stream, then commits
  the empty file. Files larger than 2,147,483,647 bytes are rejected in v1
  because the target File Manager APIs use signed 32-bit lengths.
- `TRANSFER_COMPLETE` contains result code (u16), committed length (u64),
  computed CRC-32 (u32), and canonical destination name (u8 length + UTF-8).
  Success is reported only after the destination is closed and atomically
  installed; the request ID must match the active copy.
- `TRANSFER_CANCEL` contains a reason code (u8). During XMODEM the sender also
  sends CAN-CAN; the receiver closes and removes only the temporary file. The
  receiver returns `TRANSFER_COMPLETE` with a cancelled result when control
  framing resumes.
- `ERROR` contains code (u16), rejected message type (u8), text length (u8),
  and at most 128 printable UTF-8 bytes. Protocol errors are distinct from
  filesystem result codes and never imply successful completion.

### Transfer payloads and XMODEM

- `COPY_PREPARE` contains source endpoint ID and `PathRef`, destination
  endpoint ID and parent `PathRef`, destination name, expected 64-bit byte
  length, CRC-32/ISO-HDLC of the transfer stream, and transfer-mode byte
  (`1` = XMODEM-CRC, 128-byte blocks).
- The receiver validates its destination, free space, filename, and overwrite
  policy before responding `TRANSFER_READY`. It writes into a temporary file
  in the destination directory and then sends `C` to start XMODEM-CRC. The
  sender uses the existing host `XModemCrcSender` or a matching Macintosh
  sender/receiver. Control frames are paused until EOT/ACK or cancellation.
- XMODEM block numbers wrap as specified; both sides handle duplicate blocks,
  NAK/retry limits, CAN-CAN cancellation, timeouts, and disconnects. The
  receiver checks exact byte length and CRC-32, closes/flushed data, then
  renames the temporary file into place. It must not replace the destination
  before all checks pass.
- Windows-native files transfer as data streams. A Macintosh file transferred
  to Windows uses MacBinary II when either fork or Finder metadata is present;
  that preserves both forks and type/creator. A Windows file transferred to
  HFS is a data-fork-only file unless the source is a valid MacBinary II
  package, which is unpacked. V1 reports unsupported MacRoman names or
  malformed MacBinary II explicitly.
- XMODEM-CRC is the interoperable v1 mode because the host already has a
  tested XMODEM-CRC sender and the MacStudio serial runtime contains matching
  protocol code. ZMODEM is a later mode after a host ZMODEM sender/receiver is
  implemented and interoperability tests exist; it is not advertised as
  available in v1.

## 7. Filesystem safety and failure behavior

- The Windows user selects one share root for the session. All remote
  operations are resolved beneath it. Reject `..`, rooted paths, alternate
  separators, device paths, and reparse points/symlinks that could escape the
  selected root.
- The Mac user explicitly selects a mounted volume/folder. The remote app
  exposes only that selected scope and uses volume reference + directory ID
  lookups rather than string-prefix checks.
- Destructive operations and overwrites require confirmation. Host requests
  from the Mac are denied by default until the user enables them for the
  session; per-operation confirmation remains available.
- Receive into a unique temporary file. On cancel, CRC/length failure,
  timeout, or disconnect, delete only that known temporary file and leave the
  previous destination unchanged.
- Never execute a transferred file. Never implement a remote command or shell
  message. Surface errors in both UIs with operation ID and endpoint.
- Frame lengths, file names, path depth, listing count, transfer size, and
  timeouts are bounded. The host rejects paths outside its root before any
  filesystem call.

## 8. Implementation increments

1. **Specification and UX:** this document; confirm frame layout, active-role
   behavior, and MacBinary II policy before coding the wire implementation.
2. **Host protocol core:** implement frame encoding/decoding, request IDs,
   session state, and role arbitration with fake-stream tests.
3. **Standalone WPF shell:** create `src/RetroLink`; implement two local
   panels, toolbar/keyboard commands, share-root selection, COM settings, and
   honest disconnected/connected/error states.
4. **MacStudio remote shell:** add the `.cmideproj`, two-pane monochrome-safe
   UI, serial component, HELLO/HELLO_ACK, and ROLE_REQUEST/ROLE_STATE
   handling; build the Mac app with Retro68.
5. **Bidirectional directory browsing:** enumerate mounted HFS volumes,
   browse Mac catalogs and the Windows share with bounded paged listings, and
   verify parent/child navigation over COM32/COM33.
6. **Filesystem mutations:** add safe create/rename/delete on both providers;
   test host operations with fake serial peers before emulator use.
7. **Bidirectional copy:** implement XMODEM-CRC in both directions, MacBinary
   II import/export, temporary-file commit, cancel, and progress. Run the
   standalone WPF app on COM32 and a separate Basilisk II guest on COM33.
8. **Hardening and usability:** exercise collisions, malformed frames,
   stale sessions, timeouts, disconnect recovery, overwrite confirmation,
   unsupported HFS names, and 9600/57,600 baud. Document Windows and emulator
   launch instructions. Consider ZMODEM only after v1 passes.

Each increment is committed locally with only its own files staged. Existing
uncommitted IDE/Agent work must not be included in a RetroLink commit.

## 9. Acceptance tests

### Automated

- Frame codec round trips, arbitrary stream chunking, noise resynchronization,
  bad CRC/version/flags, oversized payload, stale session, and unknown type.
- Role transitions, simultaneous claim tie-break, request while busy, release
  on disconnect, and fresh-session reset.
- Root confinement for `..`, absolute/device paths, symlink/reparse traversal,
  invalid HFS names, and duplicate/overwrite policy.
- Transfer of 0, 1, 127, 128, 129, multi-block, and block-number-wrap payloads;
  retry, duplicate block, cancellation, mismatch, receiver disk error, and
  temporary-file cleanup.
- Provider tests for list ordering, paging, create, rename, delete, and
  preservation of an existing destination on failed copy.

### COM32/COM33 emulator

1. Use COM32 for RetroLink and COM33 for Basilisk II Serial A/Modem. Do not
   use or alter COM30/COM31, which remain the separate Agent/debug link.
2. Start at 9600 baud, 8-N-1, no flow control. Verify HELLO and show target,
   volume, and current owner in both applications.
3. Type/click in the Mac UI and verify it becomes ACTIVE; request control in
   WPF and verify the Mac becomes REMOTE. Repeat with nearly simultaneous
   requests and confirm one owner is selected.
4. Browse host root from the WPF UI and from the Mac UI. Create, rename, and
   delete a disposable folder/file on each endpoint with confirmation.
5. Copy test files in both directions, verify SHA-256, byte length, MacBinary
   forks/metadata where applicable, progress, cancellation, and recovery.
6. Repeat representative browsing and transfer at 57,600 baud. Confirm the
   unrelated Agent still works on COM30/COM31.

### Physical hardware

After emulator acceptance, repeat at a conservative baud on a real Mac with
the documented compatible serial adapter/cable. Verify the Mac model, System
version, port mapping, and baud independently. Basilisk II success does not
establish real-hardware compatibility.

## 10. Reference material

- [Midnight Commander](https://midnight-commander.org/) describes a visual
  dual-pane manager with copy, move, and delete operations. Its project and
  shortcut documentation informed the panel and key-command model.
- [Norton Commander 5.0 for DOS](https://archive.org/details/norton-commander-5.0-for-dos_202503)
  and the [Norton Commander 5.0 UI Museum](https://ilyabirman.net/meanwhile/all/ui-museum-norton-commander-5-0/)
  document the familiar function-key workflow; the annotated UI describes
  F5 copy and F6 rename/move.
- The [FastLynx manual is indexed by the Internet Archive](https://archive.org/details/manualzilla-id-5700429).
  Its item page was not reachable during this research pass, so ACTIVE/REMOTE
  takeover behavior in this specification follows the user's stated
  requirement rather than an unverified manual detail.
- Historical design references included the MacStudio serial agent and its
  `serial-port` and `protocol-transfer` components. Those IDE-only sources
  are not dependencies of this standalone project. The shared transfer
  implementation is included as
  [XModemCrcSender.cs](../src/ClassicMacIde.Core/XModemCrcSender.cs).
