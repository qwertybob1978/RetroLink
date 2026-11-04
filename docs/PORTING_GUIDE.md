# RetroLink platform porting guide

This guide describes how to build a RetroLink peer for a new computer or
operating system with a usable serial port. It is intended for both human
developers and code-generation agents. Follow the wire contract and operational
rules below; keep platform-specific serial, filesystem, and UI code behind
separate adapters.

RetroLink is a two-endpoint file manager, not a remote shell. It must not expose
arbitrary host paths, execute received files, or silently overwrite or delete
data.

## 1. Source of truth

Protocol version 1 is described here from the current shared codec and peer
implementations. If an older planning document disagrees, prefer the current
codec and tests, and record any unresolved interoperability mismatch rather
than guessing:

- [Frame codec](../src/ClassicMacIde.Core/RetroLinkProtocol.cs)
- [Session and HELLO codec](../src/ClassicMacIde.Core/RetroLinkSession.cs)
- [Directory and filesystem-operation codecs](../src/ClassicMacIde.Core/RetroLinkDirectoryProtocol.cs)
- [Transfer codec](../src/ClassicMacIde.Core/RetroLinkTransferProtocol.cs)
- [XMODEM-CRC sender](../src/ClassicMacIde.Core/XModemCrcSender.cs)
- [Classic Mac peer](../macintosh/src/user_events.c)
- [DOS peer protocol definitions](../src/RetroLink.Dos/include/retro_protocol.h)
- [Existing RetroLink plan](retrolink-plan.md)

Some version-1 fields and optional messages are not used consistently by every
current peer. Those gaps are called out below. Do not advertise a feature just
because a constant or message type exists.

## 2. Product and UX contract

- Name the application **RetroLink** and display its own version.
- Use **LOCAL** and **REMOTE** for the two endpoints in panes, menus, actions,
  status text, and documentation. These names are relative to the UI owner;
  do not call the other endpoint "DOS", "Windows", or "Mac" in the file-manager
  UI.
- Show which endpoint is **ACTIVE**. Only the active endpoint may initiate
  filesystem and copy operations. A non-active endpoint can request control.
- Present provider roots as browsable volumes. Root display names are labels,
  not paths or protocol identifiers.
- Make connection state, port, baud rate, transfer progress, failures, and
  overwrite decisions explicit. Never report success after a timeout,
  cancellation, CRC mismatch, or failed filesystem commit.
- The LOCAL and REMOTE panes can have different screen and character-set
  limits. Keep UI navigation component-based rather than constructing raw
  remote paths.

## 3. Suggested implementation boundaries

Keep the protocol reusable and independently testable. A small port should
still separate:

1. **Serial transport** — open/close the port, configure it, read arbitrary
   byte chunks, write complete byte sequences, and report disconnects.
2. **Wire codec** — frame encoding/decoding, CRCs, UTF-8 validation, and
   payload bounds. It must not depend on UI or filesystem APIs.
3. **Session and role state** — handshake, endpoint/session IDs, request IDs,
   capabilities, active endpoint, and role epoch.
4. **Filesystem providers** — map provider IDs and component paths to only the
   volumes/directories intentionally shared by the application.
5. **Transfer engine** — prepare, receive/send XMODEM-CRC, validate, commit,
   and cancel without interleaving control frames into the data stream.
6. **UI or command interface** — LOCAL/REMOTE browsing, selection, role
   status, prompts, and actionable error reporting.

Do not share a serial port with the MacStudio Agent debug connection. The
Agent's `MSAG` protocol and RetroLink's `RLNK` file protocol are separate
protocols and should have separate port ownership.

## 4. Serial transport

Use a full-duplex serial connection configured for 8 data bits, no parity, one
stop bit, and no hardware or software flow control. Use **9600 baud** as the
interoperability baseline. Current DOS and Mac testing uses 9600; the DOS UI
also supports 19200, 38400, and 57600. Change baud only when both applications
are configured to the same rate and reconnect.

Serial reads do not preserve frame boundaries. A read may contain a partial
frame, several frames, or noise followed by a frame. Feed every received byte
to a streaming decoder and retain incomplete data. Do not treat one read as
one protocol message.

Keep the file-transfer port open for the duration of a transfer. XMODEM owns
the link while its data stream is active; do not inject RetroLink frames or
unrelated diagnostic bytes during that period.

## 5. Frame format

Every control message uses this frame. Integers are unsigned and big-endian.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `RLNK` |
| 4 | 1 | Protocol version: `1` |
| 5 | 1 | Message type |
| 6 | 2 | Reserved; both bytes must be zero |
| 8 | 16 | Session ID |
| 24 | 4 | Request ID |
| 28 | 2 | Payload length |
| 30 | 0–1024 | Payload |
| end - 2 | 2 | CRC-16/CCITT-FALSE of header and payload |

The fixed header is 30 bytes, maximum payload is 1024 bytes, and maximum
complete frame is 1056 bytes. CRC-16/CCITT-FALSE uses polynomial `0x1021`,
initial value `0xFFFF`, no reflection, no final XOR, and is transmitted
big-endian. The CRC covers every frame byte except the CRC itself.

The decoder must reject an unsupported version, nonzero reserved bytes,
oversized payload, or incorrect CRC, then resynchronize on the next `RLNK`
magic. Do not allocate based on an unvalidated length.

Use a fresh nonzero request ID for each ordinary request and echo it in the
matching response. HELLO and the current unsolicited ROLE_STATE use request
ID zero. Do not confuse the frame's session ID with either endpoint ID.

Useful CRC check vectors for ASCII `123456789`:

| Check | Expected |
|---|---:|
| Frame CRC-16/CCITT-FALSE | `0x29B1` |
| XMODEM CRC-16 | `0x31C3` |
| CRC-32/ISO-HDLC | `0xCBF43926` |

## 6. Session and capabilities

### HELLO (`0x01`)

The session initiator sends a 29-byte payload:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Minimum protocol version |
| 1 | 1 | Maximum protocol version |
| 2 | 3 | Application major, minor, patch |
| 5 | 16 | Initiator endpoint ID |
| 21 | 4 | Initiator capability flags |
| 25 | 4 | Requested baud rate in baud, e.g. `9600` |

Generate a new 16-byte session ID for each connection attempt and put it in
the frame header. Keep the endpoint ID stable for the lifetime of that
endpoint instance and unique from the other endpoint in the session.

### HELLO_ACK (`0x02`)

The receiver replies using the same session ID. The fixed payload is 37 bytes,
followed by zero to eight root descriptors:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Selected protocol version; currently `1` |
| 1 | 3 | Application major, minor, patch |
| 4 | 2 | System major and minor |
| 6 | 2 | Machine type |
| 8 | 16 | Receiver endpoint ID |
| 24 | 4 | Receiver capability flags |
| 28 | 4 | Supported-baud flags |
| 32 | 4 | Maximum-transfer-size field |
| 36 | 1 | Root count, maximum `8` |

Each root descriptor is `providerId:u32`, `nameLength:u8`, UTF-8 display name
(maximum 64 bytes), and `flags:u8`. Flag bit 0 means read-only; all other bits
are reserved and must be zero. Provider ID zero is invalid.

Capability bits:

| Bit | Meaning |
|---:|---|
| `0x00000001` | LIST |
| `0x00000002` | MKDIR |
| `0x00000004` | RENAME |
| `0x00000008` | DELETE |
| `0x00000010` | XMODEM-CRC |
| `0x00000020` | MacBinary II |
| `0x00000040` | Role handover |
| `0x00000080` | Host-initiated REMOTE-to-LOCAL copy request |

Unknown capability bits are ignored. Advertise only features that the peer
can actually perform. The current Mac peer's HELLO_ACK advertises `0xC1`,
including host-initiated copy requests. Capability bits do not enumerate every
operation implemented by all existing builds; validate the requested
operation and return a failure if it is unsupported.

The supported-baud flags use bit positions in this order:
bit 0 = 300, 1 = 600, 2 = 1200, 3 = 1800, 4 = 2400, 5 = 3600, 6 = 4800,
7 = 7200, 8 = 9600, 9 = 14400, 10 = 19200, 11 = 28800, 12 = 38400,
13 = 57600 baud. The current Mac peer advertises `0x00000100` (9600 baud).

**Field compatibility caution:** The shared .NET codec interprets HELLO_ACK
offset 28 as supported-baud flags and offset 32 as maximum transfer size. The
DOS structure names these words `baud_rate` and `free_bytes`; the Mac currently
sends zero at offset 32. Existing peers do not consistently enforce these
values. Treat them as advisory until a target-specific interoperability test
proves otherwise; do not interpret zero as a reason to reject the session or
silently assume a disk-space value.

The MacBinary II capability bit exists, but the current shared transfer codec
accepts only transfer format 0. Do not claim MacBinary II/resource-fork
transfer support unless the complete sender/receiver flow is implemented and
tested.

## 7. Message types

| Type | Name | Current use |
|---:|---|---|
| `0x01` | HELLO | Start a session |
| `0x02` | HELLO_ACK | Accept a session and describe the endpoint |
| `0x03` | ROLE_REQUEST | Request the active role |
| `0x04` | ROLE_STATE | Publish active endpoint and role epoch |
| `0x05` | HEARTBEAT | Defined in the older plan; not required by current peers |
| `0x10` | LIST_REQUEST | Request a directory page |
| `0x11` | LIST_PAGE | Return a directory page |
| `0x12` | MKDIR | Create a directory |
| `0x13` | RENAME | Rename or move within one provider |
| `0x14` | DELETE | Delete one file or empty directory |
| `0x15` | OP_RESULT | Return filesystem-operation status |
| `0x20` | COPY_PREPARE | Prepare a file copy |
| `0x21` | TRANSFER_READY | Receiver accepts or rejects preparation |
| `0x22` | TRANSFER_COMPLETE | Receiver reports final transfer result |
| `0x23` | TRANSFER_CANCEL | Type is defined; no common payload contract is implemented |
| `0x24` | COPY_REQUEST | Active host asks a peer to prepare a remote-to-host copy |
| `0x25` | COPY_REQUEST_RESULT | Peer accepts or rejects a COPY_REQUEST |
| `0x7F` | ERROR | Report a protocol/request error |

Current code does not require heartbeat messages or define a common payload
for `TRANSFER_CANCEL`. Do not make a new peer depend on either. XMODEM's
in-band cancellation bytes are separate from the `TRANSFER_CANCEL` frame.

## 8. Active-role handover

The active endpoint owns the UI actions and may issue filesystem or copy
requests. Every such request carries both the active endpoint ID and a role
epoch, so a receiver can reject stale requests.

`ROLE_REQUEST` (`0x03`) has a 21-byte payload:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 16 | Requester's endpoint ID |
| 16 | 4 | Expected role epoch |
| 20 | 1 | Reason: 0 key, 1 command, 2 explicit takeover |

`ROLE_STATE` (`0x04`) has a 21-byte payload:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Current role epoch |
| 4 | 16 | Active endpoint ID |
| 20 | 1 | Pending-operation flag; current peers send zero |

On a valid request, the current owner verifies the requester identity, epoch,
and reason, and refuses/defer handover while a file operation is active.
Otherwise increment the epoch, assign the requester as active, and publish
ROLE_STATE. A receiver must not accept an operation whose endpoint or epoch
does not match its current state.

After HELLO, establish and publish one authoritative role state before
allowing operations. The existing Windows UI starts with its local endpoint
active; do not assume that startup rule applies to every new pair. The old plan
describes simultaneous-request arbitration, but current peers do not implement
that 50 ms tie-break. Serialize role requests and treat the latest valid
ROLE_STATE as authoritative.

## 9. Paths, providers, and names

A `PathRef` is:

```text
providerId:u32 | componentCount:u8 | (nameLength:u8 | UTF-8 name)[componentCount]
```

The root is a PathRef with zero components. The shared codec limits paths to
16 components and 512 encoded bytes, and components to 255 UTF-8 bytes.
Provider ID zero, empty components, `.` and `..`, NUL, backslash, and colon
are invalid. Slash is disallowed by default; only allow it in paths if the
provider and every operation using that path can preserve it safely.

Provider IDs identify roots within an endpoint. Interpret a provider together
with its endpoint ID; do not assume IDs are global or are operating-system
volume numbers. The existing DOS and Windows shared-root code uses
`0x80000001`. Root display names are presentation only. Never accept a raw
absolute path from the wire or allow a component to escape its selected root.

Wire-level UTF-8 limits do not override the target filesystem:

- The DOS provider uses printable ASCII and DOS 8.3 filename rules; its parser
  has a 63-byte component buffer.
- Classic HFS names are limited to 31 MacRoman characters. Convert them to
  and from UTF-8 at the protocol boundary, and reject unrepresentable names
  instead of truncating them.
- Other targets must enforce their own path/name limits and reject names that
  cannot be represented without loss.

Use a unique provider ID for each separately browsable root. Mark roots
read-only when they cannot safely accept changes. Never let a peer choose an
arbitrary local path by supplying a provider ID or name.

## 10. Directory and filesystem operations

All filesystem request payloads begin with `roleEpoch:u32` and
`activeEndpointId[16]`. Integers are big-endian. Check both fields before
performing any operation.

### LIST_REQUEST (`0x10`)

```text
roleEpoch:u32 | activeEndpointId[16] | PathRef |
cursor:u32 | entryLimit:u16
```

`entryLimit` is 1–32. Cursor zero requests the first page; later requests use
the previous page's `nextCursor`. `0xFFFFFFFF` means there are no more pages.

### LIST_PAGE (`0x11`)

```text
resultCode:u16 | cursor:u32 | nextCursor:u32 | entryCount:u8 | entries[]
```

An error page has no entries. Each entry is:

```text
kind:u8 | attributes:u16 | nameLength:u8 | UTF-8 name |
dataForkSize:u64 | resourceForkSize:u32 | modifiedTime:u32
```

Kind 0 is a file; kind 1 is a directory. Attribute bits are provider-defined.
The shared codec treats `modifiedTime` as an opaque 32-bit value; the Mac peer
currently sends seconds since 1970 and the DOS peer sends zero. Do not compare
timestamps from different providers unless their epoch and resolution are
known.

### MKDIR (`0x12`)

```text
roleEpoch:u32 | activeEndpointId[16] | parentPath:PathRef |
nameLength:u8 | UTF-8 name
```

### RENAME (`0x13`)

```text
roleEpoch:u32 | activeEndpointId[16] | sourcePath:PathRef |
destinationParentPath:PathRef | newNameLength:u8 | UTF-8 new name
```

The source and destination parent must use the same provider/volume.

### DELETE (`0x14`)

```text
roleEpoch:u32 | activeEndpointId[16] | path:PathRef | flags:u8
```

Flags must be zero in version 1. Delete one file or an empty directory only;
do not implement recursive deletion.

### OP_RESULT (`0x15`)

```text
resultCode:u16 | operationType:u8 | canonicalNameLength:u8 |
UTF-8 canonical name
```

The operation type is MKDIR, RENAME, or DELETE. Result code zero means success;
nonzero means failure. Error-code meanings beyond success/failure are
provider-specific in current peers. Include the canonical name when the
filesystem changed the requested name. Refresh the affected directory after a
successful mutation.

## 11. File transfers

### COPY_PREPARE (`0x20`)

The fixed fields and paths are serialized in this order:

```text
roleEpoch:u32
activeEndpointId[16]
sourceEndpointId[16]
sourcePath:PathRef
destinationEndpointId[16]
destinationPath:PathRef
destinationNameLength:u8
destinationName:UTF-8
streamLength:u64
streamCrc32:u32
transferFormat:u8
overwrite:u8
transferMode:u8
```

The payload must fit in the 1024-byte frame limit. Endpoint identities are
required because provider IDs are endpoint-scoped. Confirm the destination,
name, available space, read-only status, and overwrite policy before accepting
the copy.

Current shared-codec version 1 values are:

- `transferFormat = 0`: raw byte stream.
- `transferMode = 1`: 128-byte XMODEM-CRC.
- `streamLength` must not exceed `2,147,483,647` in the shared transfer codec.
- `streamCrc32` is CRC-32/ISO-HDLC over the exact unpadded stream.

The current transfer flow handles a file's byte stream; the directory
protocol's resource-fork size field does not mean resource-fork contents are
also transferred. Do not claim complete Classic Mac metadata/fork preservation
without an implemented, tested data format for it.

The active target may initiate `COPY_PREPARE` for either transfer direction.
The active PC may also request a download with `COPY_REQUEST`; the target then
initiates `COPY_PREPARE` using the request's ID. A target-to-PC copy is staged
in a unique temporary file under the selected LOCAL root, checked against the
declared length and CRC-32, and committed only after verification. PC-to-target
copies identify a regular source file under the same root; the PC sends it
using the shared XMODEM-CRC sender. All paths reject stale roles, unsupported
providers or formats, and LOCAL paths that cross reparse points.

### COPY_REQUEST (`0x24`) and COPY_REQUEST_RESULT (`0x25`)

`COPY_REQUEST` asks the target to send one file from a REMOTE provider to the
selected LOCAL folder. The host must own the active role. Its payload is:

```text
roleEpoch:u32
activeEndpointId[16]
sourceEndpointId[16]
sourcePath:PathRef
destinationEndpointId[16]
destinationPath:PathRef
destinationNameLength:u8
destinationName:UTF-8
overwrite:u8
```

The target verifies the role epoch, active endpoint, source and destination
endpoint IDs, provider IDs, safe paths, and destination name before accepting.
Advertise capability `0x00000080` only when `COPY_REQUEST` is implemented.
The RT1170 target, Windows app, and Classic Mac peer support this extension;
the DOS peer does not currently implement it and ignores the unknown message
type.
`COPY_REQUEST_RESULT` contains exactly `resultCode:u16` and uses the same
request ID. Result 0 means the request was accepted; the target then sends
`COPY_PREPARE` with that same ID after it has read and checksummed the source.
Nonzero results reject the request: 1=busy, 2=invalid request/source, 3=stale
or unauthorized role, and 4=source read or preparation failure. The host
continues waiting after result 0 and times out if no matching `COPY_PREPARE`
arrives. The host receiver still validates the returned endpoints, paths, name,
length, CRC-32, and overwrite policy before accepting the transfer.

### TRANSFER_READY (`0x21`)

Exactly 17 bytes:

```text
resultCode:u16 | streamLength:u64 | streamCrc32:u32 |
transferMode:u8 | maximumBlockSize:u16
```

On success, the receiver echoes the accepted length and CRC, and reports mode 1
with maximum block size 128. A nonzero result rejects the preparation.

### XMODEM-CRC data phase

After a successful READY, transfer bytes using classic 128-byte XMODEM-CRC.
The receiver requests CRC mode with ASCII `C` (`0x43`). Each packet is 133
bytes:

```text
SOH (0x01) | block number:u8 | one's complement:u8 |
128 data bytes | XMODEM CRC-16:u16 big-endian
```

XMODEM CRC-16 uses polynomial `0x1021`, initial value zero, no reflection, and
no final XOR. Begin block numbering at 1 and wrap the one-byte number normally.
Pad only the final wire block with `0x1A` (SUB); write exactly `streamLength`
bytes to the destination and calculate CRC32 over those exact bytes.

The receiver ACKs each accepted block (`0x06`) and requests retransmission for
invalid blocks (`0x15`). The sender finishes with EOT (`0x04`) and waits for
ACK. Bound retries and response waits. A pair of CAN bytes (`0x18, 0x18`)
signals sender cancellation. Handle duplicate retransmissions idempotently and
do not write the same block twice.

Control-frame CRC and XMODEM block CRC are different algorithms: control
frames use initial value `0xFFFF`; XMODEM uses initial value zero.

### TRANSFER_COMPLETE (`0x22`) and commit

Exactly 14 bytes:

```text
resultCode:u16 | streamLength:u64 | streamCrc32:u32
```

The receiver should write to a temporary file in the destination directory,
verify actual byte count and CRC32, flush/close it, then atomically rename or
otherwise commit it. Remove the temporary file on failure or cancellation.
Only send success after the destination is committed. The initiator must
compare the completion length and CRC with the preparation values.

The current XMODEM sender rejects a zero-length stream. Although the control
payload can encode length zero, empty-file transfer is not an interoperable
version-1 feature; do not claim it works without agreeing on and testing a
complete empty-stream flow.

The `TRANSFER_CANCEL` message type has no common payload contract in the
current codecs. Use the XMODEM cancellation sequence during the data phase and
close/reset the transfer state cleanly; do not invent a wire payload and call
it version-1 compatible.

## 12. Errors and failure handling

`ERROR` (`0x7F`) uses:

```text
errorCode:u16 | rejectedMessageType:u8 | textLength:u8 | UTF-8 text
```

Text is at most 128 bytes. Reject malformed or unauthorized requests
explicitly. Distinguish protocol errors from ordinary filesystem failures:
use OP_RESULT for MKDIR/RENAME/DELETE and transfer result messages for file
copies. If a request cannot be performed, return a failure or a clear UI error;
never silently substitute a different path, filename, baud rate, or transfer
mode.

Also handle lost serial connection, stale role epochs, malformed UTF-8,
unknown providers, unsupported capabilities, disk-full/write errors,
overwrite refusal, failed flush/rename, transfer timeout, CRC mismatch, and
user cancellation. On disconnect, discard incomplete temporary output and
require a new handshake before accepting more requests.

## 13. Porting and conformance checklist

### Before coding

- Identify the target's serial API, supported baud rates, data bits, and flow
  control behavior.
- Identify safe roots/providers and target path/name encoding limits.
- Decide how the target represents files, directories, read-only media, and
  timestamps.
- Decide whether the target is initially a session initiator, responder, or
  both. Do not hard-code the other endpoint as a specific operating system.
- Separate the RetroLink serial link from any debug, console, or agent link.

### Implement in this order

1. Unit-test big-endian reads/writes, strict UTF-8, frame CRC, and stream
   resynchronization.
2. Implement HELLO/HELLO_ACK and verify session and endpoint IDs.
3. Implement role state and stale-epoch rejection before filesystem writes.
4. Implement provider roots and paged LIST in both directions.
5. Add MKDIR, RENAME, and non-recursive DELETE, with explicit result handling.
6. Add COPY_PREPARE validation, XMODEM-CRC send/receive, CRC/length checking,
   temporary-file cleanup, and commit.
7. Add UI labels, status, progress, confirmation prompts, and reconnect
   behavior.

### Required test cases

- Decode frames split at every possible byte boundary and multiple frames in
  one read.
- Reject bad magic/version/reserved bytes, oversized length, invalid CRC,
  truncated payloads, and malformed UTF-8; recover to the next valid frame.
- Verify the three CRC check vectors above.
- Test handshake success, wrong session IDs, unsupported version, and
  unexpected root descriptors.
- Browse an empty root, nested paths, more than one list page, and a
  provider-specific name-limit failure.
- Test role handover, stale epoch, wrong requester ID, and a handover request
  during an active transfer.
- Test create/rename/delete success and failures, including read-only root,
  overwrite, non-empty directory, and path traversal attempts.
- Transfer data in both directions at sizes 1, 127, 128, 129, and 257 bytes;
  compare exact bytes and CRC32. Also test duplicate blocks, bad block CRC,
  retries, disconnect, cancellation, disk-full, and commit failure.
- Test names that the target cannot represent and verify they are rejected,
  not truncated or silently renamed.
- Verify every visible panel/action says LOCAL or REMOTE, independent of the
  remote operating system.

The repository's protocol tests are in
[tests/ClassicMacIde.Core.Tests](../tests/ClassicMacIde.Core.Tests/), and
the DOS native tests are under
[src/RetroLink.Dos/tests](../src/RetroLink.Dos/tests/). The Python host
simulator at
[scripts/windows/retrolink_host_test.py](../scripts/windows/retrolink_host_test.py)
currently targets the Mac and DOS peers; extend it or provide an equivalent
simulator for a new target. Finish with a live test over a real or virtual
null-modem pair. Emulator-only tests do not establish compatibility with
physical hardware.

## 14. Release and maintenance

Keep the implementation's protocol version at 1 only while it interoperates
with the current frame and payload rules. A wire-format change needs an
explicit version/capability plan and cross-peer tests; do not repurpose a
reserved field or message payload silently.

Document target-specific build prerequisites, serial-port setup, supported
filesystem roots, known name/size limits, and exact test results in the
platform's README. Keep build artifacts out of source control and include
release notes with the application version.
