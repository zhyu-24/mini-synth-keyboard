#!/usr/bin/env python3
"""Cross-platform pyserial uploader for the Mini Synth MSPKG USB importer."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys
import time
from typing import Callable, Iterable

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - environment dependent
    serial = None
    list_ports = None
    SERIAL_IMPORT_ERROR = exc
else:
    SERIAL_IMPORT_ERROR = None

from mspkg_usb_protocol import (
    Command,
    DeviceState,
    ErrorCode,
    Frame,
    FrameDecoder,
    MAX_CHUNK_BYTES,
    crc32,
    encode_frame,
    pack_begin,
    pack_chunk,
    pack_filename,
    pack_init_storage_confirmation,
    sanitize_filename,
    unpack_ack,
    unpack_status,
)

ERROR_NAMES = {int(value): value.name for value in ErrorCode}
STATE_NAMES = {int(value): value.name for value in DeviceState}


class ImportProtocolError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        code: int | None = None,
        state: int | None = None,
        next_offset: int | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.state = state
        self.next_offset = next_offset


class Transport:
    def __init__(self, port: str, baud: int, timeout: float, retries: int) -> None:
        if serial is None:
            raise RuntimeError(f"pyserial is required: {SERIAL_IMPORT_ERROR}")
        self.serial = serial.Serial(port=port, baudrate=baud, timeout=0.05, write_timeout=timeout)
        self.timeout = timeout
        self.retries = retries
        self.decoder = FrameDecoder()
        self.sequence = 1
        time.sleep(0.15)
        self.serial.reset_input_buffer()

    def close(self) -> None:
        self.serial.close()

    def request(self, command: Command, payload: bytes = b"", accept: Iterable[Command] = (Command.ACK,)) -> Frame:
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        accepted = {int(item) for item in accept}
        encoded = encode_frame(command, sequence, payload)
        last_problem = "timeout"
        for attempt in range(self.retries + 1):
            self.serial.write(encoded)
            self.serial.flush()
            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline:
                data = self.serial.read(4096)
                if not data:
                    continue
                for frame in self.decoder.feed(data):
                    if frame.sequence != sequence:
                        continue
                    if frame.command == Command.ERROR:
                        info = unpack_ack(frame.payload)
                        code = info["code"]
                        raise ImportProtocolError(
                            f"device error {ERROR_NAMES.get(code, code)}; "
                            f"state={STATE_NAMES.get(info['state'], info['state'])}; "
                            f"next_offset={info['next_offset']}",
                            code=code,
                            state=info["state"],
                            next_offset=info["next_offset"],
                        )
                    if frame.command in accepted:
                        return frame
                if self.decoder.errors:
                    last_problem = self.decoder.errors[-1].name
            last_problem = f"timeout attempt {attempt + 1}/{self.retries + 1}"
        raise TimeoutError(f"no valid response for {command.name}: {last_problem}")


def get_status(transport: Transport) -> dict[str, int]:
    frame = transport.request(Command.STATUS, accept=(Command.STATUS_REPLY,))
    return unpack_status(frame.payload)


def print_status(transport: Transport) -> None:
    status = get_status(transport)
    code = status["code"]
    print(
        f"state={STATE_NAMES.get(status['state'], status['state'])} "
        f"ffat={'mounted' if status['mounted'] else 'unavailable'} "
        f"result={ERROR_NAMES.get(code, code)} "
        f"offset={status['next_offset']}/{status['total_length']} "
        f"expected_crc=0x{status['expected_crc']:08X} "
        f"running_crc=0x{status['running_crc']:08X}"
    )


def get_files(transport: Transport) -> list[tuple[str, int]]:
    """Return the device's root MSPKG files without printing them."""
    sequence = transport.sequence
    transport.sequence = (transport.sequence + 1) & 0xFFFFFFFF
    encoded = encode_frame(Command.LIST, sequence)
    for attempt in range(transport.retries + 1):
        transport.serial.write(encoded)
        transport.serial.flush()
        entries: list[tuple[str, int]] = []
        deadline = time.monotonic() + transport.timeout
        while time.monotonic() < deadline:
            for frame in transport.decoder.feed(transport.serial.read(4096)):
                if frame.sequence != sequence:
                    continue
                if frame.command == Command.ERROR:
                    info = unpack_ack(frame.payload)
                    raise ImportProtocolError(ERROR_NAMES.get(info["code"], str(info["code"])))
                if frame.command == Command.LIST_ENTRY:
                    if len(frame.payload) < 5:
                        raise ImportProtocolError("malformed LIST_ENTRY")
                    size, name_len = struct.unpack_from("<IB", frame.payload)
                    if len(frame.payload) != 5 + name_len:
                        raise ImportProtocolError("malformed LIST_ENTRY filename")
                    entries.append((frame.payload[5:].decode("ascii", "strict"), size))
                elif frame.command == Command.LIST_DONE:
                    return entries
        if attempt == transport.retries:
            raise TimeoutError("LIST timed out")
    raise AssertionError("unreachable")


def list_files(transport: Transport) -> None:
    entries = get_files(transport)
    for name, size in entries:
        print(f"{size:8d}  {name}")
    print(f"{len(entries)} file(s)")


def upload_file(
    transport: Transport,
    source: Path,
    remote_name: str | None,
    progress: Callable[[int, int], None] | None = None,
) -> None:
    data = source.read_bytes()
    name = sanitize_filename(remote_name or source.name)
    expected_crc = crc32(data)
    begin = unpack_ack(transport.request(Command.BEGIN, pack_begin(name, len(data), expected_crc)).payload)
    offset = begin["next_offset"]
    if offset not in (0, len(data)):
        raise ImportProtocolError(f"device returned impossible BEGIN offset {offset}")

    last_percent = -1
    while offset < len(data):
        chunk = data[offset : offset + MAX_CHUNK_BYTES]
        payload = pack_chunk(offset, chunk)
        try:
            ack = unpack_ack(transport.request(Command.CHUNK, payload).payload)
        except TimeoutError:
            status = unpack_status(transport.request(Command.STATUS, accept=(Command.STATUS_REPLY,)).payload)
            if status["state"] != DeviceState.RECEIVING or status["total_length"] != len(data):
                raise
            offset = status["next_offset"]
            continue
        next_offset = ack["next_offset"]
        if next_offset == offset:
            continue
        if next_offset != offset + len(chunk):
            raise ImportProtocolError(f"offset mismatch: host={offset}, device={next_offset}")
        offset = next_offset
        percent = offset * 100 // len(data)
        if progress is not None:
            progress(offset, len(data))
        elif percent != last_percent:
            print(f"\rupload {percent:3d}%  {offset}/{len(data)} bytes", end="", flush=True)
            last_percent = percent
    if progress is None:
        print()
    finish = unpack_ack(transport.request(Command.FINISH).payload)
    if finish["code"] != ErrorCode.OK or finish["next_offset"] != len(data):
        raise ImportProtocolError("FINISH did not confirm full commit")
    print(f"committed {name}: {len(data)} bytes, CRC-32 0x{expected_crc:08X}")


def delete_file(transport: Transport, remote_name: str) -> None:
    name = sanitize_filename(remote_name)
    ack = unpack_ack(transport.request(Command.DELETE, pack_filename(name)).payload)
    if ack["code"] != ErrorCode.OK:
        raise ImportProtocolError(f"DELETE failed: {ERROR_NAMES.get(ack['code'], ack['code'])}")
    print(f"deleted {name}")


def initialize_storage(transport: Transport, confirmation: str | None) -> None:
    required = "ERASE-MUSIC-LIBRARY"
    if confirmation != required:
        raise ValueError(f"init-storage requires --confirm {required}")
    ack = unpack_ack(
        transport.request(Command.INIT_STORAGE, pack_init_storage_confirmation()).payload
    )
    if ack["code"] != ErrorCode.OK:
        raise ImportProtocolError(
            f"INIT_STORAGE failed: {ERROR_NAMES.get(ack['code'], ack['code'])}"
        )
    print("music storage initialized; all file songs were erased")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Mini Synth USB MSPKG importer")
    parser.add_argument("--port", help="serial port, e.g. COM13 or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200, help="nominal CDC baud (default: 115200)")
    parser.add_argument("--timeout", type=float, default=2.0, help="response timeout seconds")
    parser.add_argument("--retries", type=int, default=3, help="retries after timeout")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("ports", help="list candidate serial ports without opening one")
    sub.add_parser("list", help="list root MSPKG files")
    sub.add_parser("status", help="show device importer status")
    upload = sub.add_parser("upload", help="upload and atomically commit one MSPKG")
    upload.add_argument("file", type=Path)
    upload.add_argument("--name", help="safe ASCII destination filename")
    delete = sub.add_parser("delete", help="delete one root MSPKG file and refresh the catalog")
    delete.add_argument("name", help="safe ASCII device filename")
    init_storage = sub.add_parser(
        "init-storage",
        help="format only the FFat music partition; erases every file song",
    )
    init_storage.add_argument(
        "--confirm",
        help="required literal: ERASE-MUSIC-LIBRARY",
    )
    sub.add_parser("abort", help="abort current upload and remove only its temporary file")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "ports":
        if list_ports is None:
            raise RuntimeError(f"pyserial is required: {SERIAL_IMPORT_ERROR}")
        for port in list_ports.comports():
            print(f"{port.device}\t{port.description}\t{port.hwid}")
        return 0
    if not args.port:
        raise SystemExit("--port is required for list/status/upload/delete/init-storage/abort")
    transport = Transport(args.port, args.baud, args.timeout, args.retries)
    try:
        if args.command == "list":
            list_files(transport)
        elif args.command == "status":
            print_status(transport)
        elif args.command == "upload":
            upload_file(transport, args.file, args.name)
        elif args.command == "delete":
            delete_file(transport, args.name)
        elif args.command == "init-storage":
            initialize_storage(transport, args.confirm)
        elif args.command == "abort":
            info = unpack_ack(transport.request(Command.ABORT).payload)
            print(f"aborted; state={STATE_NAMES.get(info['state'], info['state'])}")
        return 0
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, TimeoutError, ImportProtocolError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
