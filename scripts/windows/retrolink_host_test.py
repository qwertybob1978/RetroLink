#!/usr/bin/env python3
"""Exercise a single RetroLink emulator peer through a virtual COM pair."""

from __future__ import annotations

import argparse
import secrets
import struct
import sys
import time
import zlib
from dataclasses import dataclass

try:
    import serial
except ImportError as error:
    raise SystemExit(
        "pyserial is required; install it with: python -m pip install pyserial"
    ) from error


MAGIC = b"RLNK"
HEADER_SIZE = 30
MAX_PAYLOAD = 1024
MAX_LIST_ENTRIES = 32
LIST_COMPLETE = 0xFFFFFFFF
DOS_PROVIDER_ID = 0x80000001
WINDOWS_PROVIDER_ID = 0x80000001

HELLO = 0x01
HELLO_ACK = 0x02
ROLE_REQUEST = 0x03
ROLE_STATE = 0x04
LIST_REQUEST = 0x10
LIST_PAGE = 0x11
MAKE_DIRECTORY = 0x12
RENAME = 0x13
DELETE = 0x14
OPERATION_RESULT = 0x15
COPY_PREPARE = 0x20
TRANSFER_READY = 0x21
TRANSFER_COMPLETE = 0x22
ERROR = 0x7F

XMODEM_SOH = 0x01
XMODEM_EOT = 0x04
XMODEM_ACK = 0x06
XMODEM_NAK = 0x15
XMODEM_CRC_REQUEST = 0x43
XMODEM_BLOCK_SIZE = 128


class TestFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Frame:
    message_type: int
    session_id: bytes
    request_id: int
    payload: bytes


@dataclass(frozen=True)
class Root:
    provider_id: int
    name: str
    read_only: bool


@dataclass(frozen=True)
class Entry:
    kind: int
    name: str
    data_size: int


def crc16(data: bytes, initial: int = 0xFFFF) -> int:
    crc = initial
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(frame: Frame) -> bytes:
    if len(frame.session_id) != 16 or len(frame.payload) > MAX_PAYLOAD:
        raise TestFailure("Invalid frame field length.")
    header = (
        MAGIC
        + bytes((1, frame.message_type, 0, 0))
        + frame.session_id
        + struct.pack(">IH", frame.request_id, len(frame.payload))
    )
    body = header + frame.payload
    return body + struct.pack(">H", crc16(body))


class FrameDecoder:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.frames: list[Frame] = []

    def feed(self, data: bytes) -> None:
        self.buffer.extend(data)
        while True:
            offset = self.buffer.find(MAGIC)
            if offset < 0:
                if len(self.buffer) > 3:
                    del self.buffer[:-3]
                return
            if offset:
                del self.buffer[:offset]
            if len(self.buffer) < HEADER_SIZE:
                return
            payload_length = struct.unpack_from(">H", self.buffer, 28)[0]
            if (
                self.buffer[4] != 1
                or self.buffer[6] != 0
                or self.buffer[7] != 0
                or payload_length > MAX_PAYLOAD
            ):
                del self.buffer[0]
                continue
            frame_length = HEADER_SIZE + payload_length + 2
            if len(self.buffer) < frame_length:
                return
            body = self.buffer[: frame_length - 2]
            expected_crc = struct.unpack_from(">H", self.buffer, frame_length - 2)[0]
            if crc16(body) != expected_crc:
                del self.buffer[0]
                continue
            self.frames.append(
                Frame(
                    self.buffer[5],
                    bytes(self.buffer[8:24]),
                    struct.unpack_from(">I", self.buffer, 24)[0],
                    bytes(self.buffer[30 : frame_length - 2]),
                )
            )
            del self.buffer[:frame_length]

    def take_raw_byte(self) -> int | None:
        if not self.buffer:
            return None
        value = self.buffer[0]
        del self.buffer[0]
        return value


def read_u16(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def read_u32(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def encode_path(provider_id: int, components: tuple[str, ...]) -> bytes:
    if not provider_id or len(components) > 16:
        raise TestFailure("Invalid provider or path depth.")
    result = bytearray(struct.pack(">IB", provider_id, len(components)))
    for component in components:
        encoded = component.encode("utf-8")
        if not encoded or len(encoded) > 63 or component in (".", ".."):
            raise TestFailure(f"Invalid path component: {component!r}")
        result.append(len(encoded))
        result.extend(encoded)
    return bytes(result)


def decode_hello_ack(payload: bytes) -> tuple[bytes, tuple[Root, ...]]:
    if len(payload) < 37 or payload[0] != 1:
        raise TestFailure("HELLO_ACK is truncated or uses an unsupported version.")
    endpoint_id = payload[8:24]
    root_count = payload[36]
    offset = 37
    roots: list[Root] = []
    for _ in range(root_count):
        if len(payload) - offset < 5:
            raise TestFailure("HELLO_ACK root record is truncated.")
        provider_id = read_u32(payload, offset)
        name_length = payload[offset + 4]
        offset += 5
        if not provider_id or len(payload) - offset < name_length + 1:
            raise TestFailure("HELLO_ACK root record is malformed.")
        name = payload[offset : offset + name_length].decode("utf-8")
        offset += name_length
        read_only = payload[offset]
        offset += 1
        if read_only > 1:
            raise TestFailure("HELLO_ACK has an invalid read-only flag.")
        roots.append(Root(provider_id, name, bool(read_only)))
    if offset != len(payload):
        raise TestFailure("HELLO_ACK contains trailing data.")
    return endpoint_id, tuple(roots)


def make_dos_hello_ack(host_id: bytes, baud_rate: int) -> bytes:
    root_name = b"PYHOST"
    payload = bytearray(37)
    payload[0:6] = bytes((1, 1, 0, 1, 7, 0))
    payload[8:24] = host_id
    struct.pack_into(">I", payload, 24, 0x51)
    struct.pack_into(">I", payload, 28, baud_rate)
    struct.pack_into(">I", payload, 32, 0x01000000)
    payload[36] = 1
    payload.extend(struct.pack(">IB", WINDOWS_PROVIDER_ID, len(root_name)))
    payload.extend(root_name)
    payload.append(0)
    return bytes(payload)


def parse_list_page(payload: bytes) -> tuple[int, int, int, tuple[Entry, ...]]:
    if len(payload) < 11:
        raise TestFailure("LIST_PAGE is truncated.")
    result_code, cursor, next_cursor = struct.unpack_from(">HII", payload, 0)
    count = payload[10]
    if count > MAX_LIST_ENTRIES or (result_code and count):
        raise TestFailure("LIST_PAGE has an invalid entry count.")
    offset = 11
    entries: list[Entry] = []
    for _ in range(count):
        if len(payload) - offset < 4:
            raise TestFailure("LIST_PAGE entry header is truncated.")
        kind = payload[offset]
        name_length = payload[offset + 3]
        offset += 4
        if kind not in (0, 1) or not name_length or len(payload) - offset < name_length + 16:
            raise TestFailure("LIST_PAGE entry is malformed.")
        name = payload[offset : offset + name_length].decode("utf-8")
        offset += name_length
        high_size, data_size = struct.unpack_from(">II", payload, offset)
        if high_size:
            data_size = 0xFFFFFFFF
        offset += 16
        entries.append(Entry(kind, name, data_size))
    if offset != len(payload):
        raise TestFailure("LIST_PAGE contains trailing data.")
    return result_code, cursor, next_cursor, tuple(entries)


class RetroLinkHostTest:
    def __init__(self, port: str, target: str, baud_rate: int, timeout: float) -> None:
        self.port = port
        self.target = target
        self.baud_rate = baud_rate
        self.timeout = timeout
        self.decoder = FrameDecoder()
        self.host_id = secrets.token_bytes(16)
        self.session_id = secrets.token_bytes(16) if target == "mac" else b""
        self.peer_id = b""
        self.role_epoch = 0
        self.roots: tuple[Root, ...] = ()
        self.serial: serial.Serial | None = None
        self.next_request_id = 1

    def run(self) -> None:
        with serial.Serial(
            self.port,
            self.baud_rate,
            timeout=0.05,
            write_timeout=self.timeout,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            xonxoff=False,
            rtscts=False,
            dsrdtr=False,
        ) as connection:
            self.serial = connection
            connection.reset_input_buffer()
            connection.reset_output_buffer()
            if self.target == "mac":
                self.connect_to_mac()
            else:
                self.connect_to_dos()
            print(f"PASS {self.target.upper()} handshake on {self.port} at {self.baud_rate}")
            self.take_control()
            print("PASS role handoff")
            root = self.writable_root()
            print(f"Testing provider {root.provider_id:08X} ({root.name})")
            self.test_filesystem_and_transfer(root)
            self.return_control()
            print("PASS all independent peer tests; temporary test files removed")

    def send_frame(self, message_type: int, request_id: int, payload: bytes) -> None:
        if self.serial is None:
            raise TestFailure("Serial port is not open.")
        data = encode_frame(Frame(message_type, self.session_id, request_id, payload))
        written = self.serial.write(data)
        if written != len(data):
            raise TestFailure(f"Short serial write: sent {written} of {len(data)} bytes.")

    def wait_for_frame(
        self,
        message_type: int,
        request_id: int | None,
        timeout: float | None = None,
    ) -> Frame:
        if self.serial is None:
            raise TestFailure("Serial port is not open.")
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            while self.decoder.frames:
                frame = self.decoder.frames.pop(0)
                if self.session_id and frame.session_id != self.session_id:
                    continue
                if frame.message_type == ERROR and (
                    request_id is None or frame.request_id == request_id
                ):
                    raise TestFailure(
                        f"Peer returned protocol ERROR for request {frame.request_id}."
                    )
                if frame.message_type == message_type and (
                    request_id is None or frame.request_id == request_id
                ):
                    return frame
            data = self.serial.read(4096)
            if data:
                self.decoder.feed(data)
        expected = f"type 0x{message_type:02X}"
        if request_id is not None:
            expected += f", request {request_id}"
        raise TimeoutError(f"Timed out waiting for {expected}.")

    def connect_to_mac(self) -> None:
        hello = bytearray(29)
        hello[0:3] = bytes((1, 1, 1))
        hello[5:21] = self.host_id
        struct.pack_into(">I", hello, 21, 0x51)
        struct.pack_into(">I", hello, 25, self.baud_rate)
        self.send_frame(HELLO, 1, bytes(hello))
        response = self.wait_for_frame(HELLO_ACK, None, 10)
        if response.session_id != self.session_id:
            raise TestFailure("Mac HELLO_ACK used the wrong session ID.")
        self.peer_id, self.roots = decode_hello_ack(response.payload)
        if not self.roots:
            raise TestFailure("Mac did not advertise a mounted volume.")
        self.role_epoch = 0
        self.send_role_state(self.host_id, self.role_epoch)

    def connect_to_dos(self) -> None:
        print("Waiting for DOS RetroLink; press F3 in DOSBox-X to connect.")
        hello = self.wait_for_frame(HELLO, None, 60)
        if len(hello.payload) != 29 or hello.payload[0] != 1:
            raise TestFailure("DOS HELLO is malformed.")
        self.session_id = hello.session_id
        self.peer_id = hello.payload[5:21]
        if len(self.peer_id) != 16:
            raise TestFailure("DOS HELLO has no valid endpoint ID.")
        ack = make_dos_hello_ack(self.host_id, self.baud_rate)
        self.send_frame(HELLO_ACK, 0, ack)
        role = self.wait_for_frame(ROLE_STATE, None, 10)
        self.role_epoch, active_id = self.parse_role_state(role.payload)
        if active_id != self.peer_id:
            raise TestFailure("DOS did not initially claim the active role.")

    def send_role_state(self, active_id: bytes, epoch: int) -> None:
        self.send_frame(
            ROLE_STATE,
            0,
            struct.pack(">I", epoch) + active_id + b"\x00",
        )

    @staticmethod
    def parse_role_state(payload: bytes) -> tuple[int, bytes]:
        if len(payload) != 21:
            raise TestFailure("ROLE_STATE must contain 21 bytes.")
        return read_u32(payload), payload[4:20]

    def take_control(self) -> None:
        if self.target == "mac":
            self.send_role_state(self.host_id, self.role_epoch)
            return
        self.send_frame(
            ROLE_REQUEST,
            0,
            self.host_id + struct.pack(">I", self.role_epoch) + b"\x00",
        )
        response = self.wait_for_frame(ROLE_STATE, None, 10)
        self.role_epoch, active_id = self.parse_role_state(response.payload)
        if active_id != self.host_id:
            raise TestFailure("DOS rejected the host's role request.")

    def return_control(self) -> None:
        self.role_epoch += 1
        self.send_role_state(self.peer_id, self.role_epoch)

    def writable_root(self) -> Root:
        if self.target == "dos":
            return Root(DOS_PROVIDER_ID, "DOS", False)
        for root in self.roots:
            if not root.read_only:
                return root
        raise TestFailure("Mac did not advertise a writable volume.")

    def request_id(self) -> int:
        request_id = self.next_request_id
        self.next_request_id = (self.next_request_id + 1) & 0xFFFFFFFF
        if self.next_request_id == 0:
            self.next_request_id = 1
        return request_id

    def list_directory(self, root: Root, components: tuple[str, ...]) -> tuple[Entry, ...]:
        entries: list[Entry] = []
        cursor = 0
        while True:
            request_id = self.request_id()
            payload = (
                struct.pack(">I", self.role_epoch)
                + self.host_id
                + encode_path(root.provider_id, components)
                + struct.pack(">IH", cursor, MAX_LIST_ENTRIES)
            )
            self.send_frame(LIST_REQUEST, request_id, payload)
            response = self.wait_for_frame(LIST_PAGE, request_id)
            result_code, response_cursor, next_cursor, page_entries = parse_list_page(
                response.payload
            )
            if result_code:
                raise TestFailure(f"LIST_REQUEST failed with result {result_code}.")
            if response_cursor != cursor:
                raise TestFailure("LIST_PAGE returned a different cursor.")
            entries.extend(page_entries)
            if next_cursor == LIST_COMPLETE:
                return tuple(entries)
            if next_cursor == cursor:
                raise TestFailure("LIST_PAGE did not advance its cursor.")
            cursor = next_cursor

    def operation(self, message_type: int, payload: bytes) -> str:
        request_id = self.request_id()
        self.send_frame(message_type, request_id, payload)
        response = self.wait_for_frame(OPERATION_RESULT, request_id)
        if len(response.payload) < 4:
            raise TestFailure("OPERATION_RESULT is truncated.")
        result_code, operation_kind, name_length = struct.unpack_from(">HBB", response.payload)
        if operation_kind != message_type or len(response.payload) != 4 + name_length:
            raise TestFailure("OPERATION_RESULT fields do not match the request.")
        if result_code:
            raise TestFailure(
                f"Filesystem operation 0x{message_type:02X} failed with {result_code}."
            )
        return response.payload[4:].decode("utf-8") if name_length else ""

    def create_directory(self, root: Root, components: tuple[str, ...], name: str) -> None:
        payload = (
            struct.pack(">I", self.role_epoch)
            + self.host_id
            + encode_path(root.provider_id, components)
            + bytes((len(name.encode("utf-8")),))
            + name.encode("utf-8")
        )
        self.operation(MAKE_DIRECTORY, payload)

    def rename(
        self,
        root: Root,
        source_components: tuple[str, ...],
        destination_parent: tuple[str, ...],
        new_name: str,
    ) -> None:
        encoded_name = new_name.encode("utf-8")
        payload = (
            struct.pack(">I", self.role_epoch)
            + self.host_id
            + encode_path(root.provider_id, source_components)
            + encode_path(root.provider_id, destination_parent)
            + bytes((len(encoded_name),))
            + encoded_name
        )
        self.operation(RENAME, payload)

    def delete(self, root: Root, components: tuple[str, ...]) -> None:
        payload = (
            struct.pack(">I", self.role_epoch)
            + self.host_id
            + encode_path(root.provider_id, components)
            + b"\x00"
        )
        self.operation(DELETE, payload)

    def send_copy_prepare(
        self,
        root: Root,
        destination_components: tuple[str, ...],
        name: str,
        data: bytes,
    ) -> int:
        encoded_name = name.encode("utf-8")
        checksum = zlib.crc32(data) & 0xFFFFFFFF
        payload = (
            struct.pack(">I", self.role_epoch)
            + self.host_id
            + self.host_id
            + encode_path(WINDOWS_PROVIDER_ID, ())
            + self.peer_id
            + encode_path(root.provider_id, destination_components)
            + bytes((len(encoded_name),))
            + encoded_name
            + struct.pack(">QIBBB", len(data), checksum, 0, 0, 1)
        )
        request_id = self.request_id()
        self.send_frame(COPY_PREPARE, request_id, payload)
        response = self.wait_for_frame(TRANSFER_READY, request_id, 15)
        if len(response.payload) != 17:
            raise TestFailure("TRANSFER_READY has an invalid length.")
        result_code = read_u16(response.payload)
        stream_length = struct.unpack_from(">Q", response.payload, 2)[0]
        stream_crc = read_u32(response.payload, 10)
        mode = response.payload[14]
        block_size = read_u16(response.payload, 15)
        if (
            result_code
            or stream_length != len(data)
            or stream_crc != checksum
            or mode != 1
            or block_size != XMODEM_BLOCK_SIZE
        ):
            raise TestFailure(
                "Peer rejected the transfer or returned mismatched transfer metadata."
            )
        self.send_xmodem(data)
        complete = self.wait_for_frame(TRANSFER_COMPLETE, request_id, 20)
        if len(complete.payload) != 14:
            raise TestFailure("TRANSFER_COMPLETE has an invalid length.")
        result_code = read_u16(complete.payload)
        stream_length = struct.unpack_from(">Q", complete.payload, 2)[0]
        stream_crc = read_u32(complete.payload, 10)
        if result_code or stream_length != len(data) or stream_crc != checksum:
            raise TestFailure("Peer did not verify the transferred data.")
        return checksum

    def read_raw_byte(self, timeout: float) -> int:
        if self.serial is None:
            raise TestFailure("Serial port is not open.")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            buffered = self.decoder.take_raw_byte()
            if buffered is not None:
                return buffered
            data = self.serial.read(1)
            if data:
                return data[0]
        raise TimeoutError(f"No XMODEM control byte arrived within {timeout:g} seconds.")

    def send_xmodem(self, data: bytes) -> None:
        control = self.read_raw_byte(10)
        if control != XMODEM_CRC_REQUEST:
            raise TestFailure(f"Expected XMODEM 'C', got 0x{control:02X}.")
        for block_index, offset in enumerate(range(0, len(data), XMODEM_BLOCK_SIZE), 1):
            block = data[offset : offset + XMODEM_BLOCK_SIZE].ljust(
                XMODEM_BLOCK_SIZE,
                b"\x1A",
            )
            header = bytes((XMODEM_SOH, block_index & 0xFF, (~block_index) & 0xFF))
            packet_body = header + block
            packet = packet_body + struct.pack(">H", crc16(block, 0))
            for attempt in range(3):
                if self.serial is None:
                    raise TestFailure("Serial port is not open.")
                self.serial.write(packet)
                response = self.read_raw_byte(10)
                if response == XMODEM_ACK:
                    break
                if response != XMODEM_NAK or attempt == 2:
                    raise TestFailure(
                        f"Unexpected XMODEM block response 0x{response:02X}."
                    )
        for attempt in range(3):
            if self.serial is None:
                raise TestFailure("Serial port is not open.")
            self.serial.write(bytes((XMODEM_EOT,)))
            response = self.read_raw_byte(10)
            if response == XMODEM_ACK:
                return
            if response != XMODEM_NAK or attempt == 2:
                raise TestFailure(
                    f"Unexpected XMODEM EOT response 0x{response:02X}."
                )
        raise TestFailure("XMODEM receiver did not acknowledge EOT.")

    def test_filesystem_and_transfer(self, root: Root) -> None:
        token = secrets.token_hex(3).upper()
        initial_name = f"RL{token}"
        renamed_name = f"RT{token}"
        file_name = f"X{token[:5]}.BIN"
        test_directory: str | None = None
        transferred_file: tuple[str, ...] | None = None
        try:
            initial_entries = self.list_directory(root, ())
            if any(entry.name.upper() in (initial_name, renamed_name) for entry in initial_entries):
                raise TestFailure("Generated test name already exists; choose a fresh token.")
            self.create_directory(root, (), initial_name)
            test_directory = initial_name
            self.rename(root, (initial_name,), (), renamed_name)
            test_directory = renamed_name
            root_entries = self.list_directory(root, ())
            if not any(entry.kind == 1 and entry.name == renamed_name for entry in root_entries):
                raise TestFailure("Renamed test directory is absent from LIST_PAGE.")
            print("PASS directory listing, create, and rename")

            data = bytes((index * 37 + 11) & 0xFF for index in range(257))
            transferred_file = (renamed_name, file_name)
            checksum = self.send_copy_prepare(root, (renamed_name,), file_name, data)
            directory_entries = self.list_directory(root, (renamed_name,))
            matching_file = next(
                (entry for entry in directory_entries if entry.name == file_name),
                None,
            )
            if matching_file is None or matching_file.data_size != len(data):
                raise TestFailure("Transferred file is missing or has the wrong size.")
            print(
                f"PASS 257-byte XMODEM-CRC receive and CRC32 {checksum:08X}"
            )
            self.delete(root, transferred_file)
            transferred_file = None
            self.delete(root, (renamed_name,))
            test_directory = None
            if any(
                entry.name in (initial_name, renamed_name)
                for entry in self.list_directory(root, ())
            ):
                raise TestFailure("Temporary test directory remained after cleanup.")
            print("PASS delete and cleanup")
        finally:
            if test_directory is not None:
                self.cleanup_test_files(root, test_directory, transferred_file)

    def cleanup_test_files(
        self,
        root: Root,
        directory: str,
        file_components: tuple[str, ...] | None,
    ) -> None:
        try:
            if file_components is not None:
                entries = self.list_directory(root, (directory,))
                if any(entry.name == file_components[-1] for entry in entries):
                    self.delete(root, file_components)
            root_entries = self.list_directory(root, ())
            if any(entry.kind == 1 and entry.name == directory for entry in root_entries):
                self.delete(root, (directory,))
            print("Removed partial temporary test data.")
        except (TestFailure, TimeoutError, OSError) as error:
            print(f"WARNING: could not remove temporary test data: {error}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Test Mac or DOS RetroLink independently using a host-side serial peer."
    )
    parser.add_argument("--target", choices=("mac", "dos"), required=True)
    parser.add_argument("--port", required=True, help="Host end of the target's COM pair.")
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    if args.baud not in (9600, 19200, 38400, 57600, 115200):
        parser.error("--baud must be 9600, 19200, 38400, 57600, or 115200")
    try:
        RetroLinkHostTest(args.port, args.target, args.baud, args.timeout).run()
    except (TestFailure, TimeoutError, OSError, serial.SerialException) as error:
        print(f"FAIL {args.target.upper()}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
