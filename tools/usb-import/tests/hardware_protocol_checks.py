#!/usr/bin/env python3
"""Non-destructive hardware checks for the USB MSPKG importer."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys

TOOL_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIR))

from mspkg_usb_protocol import (  # noqa: E402
    Command,
    DeviceState,
    ErrorCode,
    MAX_CHUNK_BYTES,
    crc32,
    pack_begin,
    pack_chunk,
    unpack_ack,
    unpack_status,
)
from usb_mspkg_import import ImportProtocolError, Transport  # noqa: E402


def expect_error(transport: Transport, command: Command, payload: bytes, name: str) -> None:
    try:
        transport.request(command, payload)
    except ImportProtocolError as exc:
        if name not in str(exc):
            raise AssertionError(f"expected {name}, got {exc}") from exc
        print(f"PASS expected error: {name}")
        return
    raise AssertionError(f"expected device error {name}")


def begin(transport: Transport, name: str, data: bytes, expected_crc: int) -> None:
    ack = unpack_ack(
        transport.request(Command.BEGIN, pack_begin(name, len(data), expected_crc)).payload
    )
    assert ack["next_offset"] == 0, ack


def send_all(transport: Transport, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        chunk = data[offset : offset + MAX_CHUNK_BYTES]
        ack = unpack_ack(
            transport.request(Command.CHUNK, pack_chunk(offset, chunk)).payload
        )
        offset += len(chunk)
        assert ack["next_offset"] == offset, ack


def assert_idle(transport: Transport) -> None:
    status = unpack_status(
        transport.request(Command.STATUS, accept=(Command.STATUS_REPLY,)).payload
    )
    assert status["state"] == DeviceState.IDLE, status
    assert status["next_offset"] == 0 and status["total_length"] == 0, status


def run(port: str, source: Path) -> None:
    original = source.read_bytes()
    remote_name = "VOYAGER.MSP"
    transport = Transport(port, 115200, 3.0, 1)
    try:
        # The bytes differ from BEGIN's declared whole-file CRC.
        bad_file = bytearray(original)
        bad_file[-1] ^= 0x01
        begin(transport, remote_name, bad_file, crc32(original))
        send_all(transport, bad_file)
        expect_error(transport, Command.FINISH, b"", ErrorCode.FILE_CRC.name)
        assert_idle(transport)

        # Keep the whole-file CRC valid while leaving the header metadata CRC stale.
        metadata_bytes = struct.unpack_from("<I", original, 12)[0]
        assert metadata_bytes > 0
        bad_metadata = bytearray(original)
        bad_metadata[32 + metadata_bytes // 2] ^= 0x01
        begin(transport, remote_name, bad_metadata, crc32(bad_metadata))
        send_all(transport, bad_metadata)
        expect_error(transport, Command.FINISH, b"", ErrorCode.METADATA_CRC.name)
        assert_idle(transport)

        # Keep the whole-file CRC valid while leaving the header payload CRC stale.
        bad_payload = bytearray(original)
        bad_payload[32 + metadata_bytes] ^= 0x01
        begin(transport, remote_name, bad_payload, crc32(bad_payload))
        send_all(transport, bad_payload)
        expect_error(transport, Command.FINISH, b"", ErrorCode.PAYLOAD_CRC.name)
        assert_idle(transport)

        # A structurally invalid container with valid transfer CRC must be
        # rejected before it can replace the existing final file.
        bad_magic = bytearray(original)
        bad_magic[0:4] = b"BAD!"
        begin(transport, remote_name, bad_magic, crc32(bad_magic))
        send_all(transport, bad_magic)
        expect_error(transport, Command.FINISH, b"", ErrorCode.MSPKG_MAGIC.name)
        assert_idle(transport)

        # An ahead-of-device offset is rejected and does not advance progress.
        begin(transport, remote_name, original, crc32(original))
        expect_error(
            transport,
            Command.CHUNK,
            pack_chunk(1, original[:32]),
            ErrorCode.OFFSET_MISMATCH.name,
        )
        status = unpack_status(
            transport.request(Command.STATUS, accept=(Command.STATUS_REPLY,)).payload
        )
        assert status["state"] == DeviceState.RECEIVING, status
        assert status["next_offset"] == 0, status
        transport.request(Command.ABORT)
        assert_idle(transport)

        # Re-send an already accepted chunk with a new sequence. The device must
        # compare the bytes and ACK without advancing its authoritative offset.
        begin(transport, remote_name, original, crc32(original))
        first = original[:MAX_CHUNK_BYTES]
        ack = unpack_ack(
            transport.request(Command.CHUNK, pack_chunk(0, first)).payload
        )
        assert ack["next_offset"] == len(first), ack
        duplicate = unpack_ack(
            transport.request(Command.CHUNK, pack_chunk(0, first)).payload
        )
        assert duplicate["next_offset"] == len(first), duplicate
        transport.request(Command.ABORT)
        assert_idle(transport)
        print(f"PASS duplicate chunk idempotence: offset={len(first)}")
    finally:
        transport.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    run(args.port, args.source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
