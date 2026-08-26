#!/usr/bin/env python3
"""MSPKG USB import protocol codec and shared validation helpers."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct
import zlib

MAGIC = b"MUSB"
VERSION = 1
HEADER = struct.Struct("<4sBBHIII")
HEADER_SIZE = HEADER.size
MAX_FRAME_PAYLOAD = 1152
MAX_CHUNK_BYTES = 1024
MAX_FILENAME_BYTES = 48
MAX_METADATA_BYTES = 4096
MAX_MSPKG_PAYLOAD_BYTES = 512 * 1024
MAX_FILE_BYTES = 32 + MAX_METADATA_BYTES + MAX_MSPKG_PAYLOAD_BYTES


class Command(IntEnum):
    LIST = 0x01
    STATUS = 0x02
    BEGIN = 0x03
    CHUNK = 0x04
    FINISH = 0x05
    ABORT = 0x06
    DELETE = 0x07
    INIT_STORAGE = 0x08
    ACK = 0x80
    ERROR = 0x81
    LIST_ENTRY = 0x82
    LIST_DONE = 0x83
    STATUS_REPLY = 0x84


class DeviceState(IntEnum):
    IDLE = 0
    RECEIVING = 1
    VALIDATING = 2
    COMMITTING = 3
    ERROR = 4


class ErrorCode(IntEnum):
    OK = 0
    BAD_FRAME_CRC = 1
    PROTOCOL_VERSION = 2
    UNKNOWN_COMMAND = 3
    BAD_PAYLOAD = 4
    UNSAFE_FILENAME = 5
    FILE_TOO_LARGE = 6
    FS_NOT_MOUNTED = 7
    BUSY = 8
    NO_UPLOAD = 9
    OFFSET_MISMATCH = 10
    CHUNK_LENGTH = 11
    FILE_IO = 12
    FILE_LENGTH = 13
    FILE_CRC = 14
    MSPKG_MAGIC = 15
    MSPKG_VERSION = 16
    MSPKG_TYPE = 17
    MSPKG_DECLARED_SIZE = 18
    METADATA_CRC = 19
    PAYLOAD_CRC = 20
    PAYLOAD_FORMAT = 21
    COMMIT_FAILED = 22
    ROLLBACK_FAILED = 23
    NO_SPACE = 24
    FRAME_TOO_LARGE = 25
    DELETE_FAILED = 26
    FORMAT_FAILED = 27


@dataclass(frozen=True)
class Frame:
    command: int
    sequence: int
    payload: bytes = b""
    flags: int = 0
    version: int = VERSION


def crc32(data: bytes, seed: int = 0) -> int:
    return zlib.crc32(data, seed) & 0xFFFFFFFF


def encode_frame(command: int, sequence: int, payload: bytes = b"", flags: int = 0) -> bytes:
    if len(payload) > MAX_FRAME_PAYLOAD:
        raise ValueError("frame payload too large")
    header = HEADER.pack(
        MAGIC,
        VERSION,
        int(command) & 0xFF,
        flags & 0xFFFF,
        sequence & 0xFFFFFFFF,
        len(payload),
        crc32(payload),
    )
    return header + payload


class FrameDecoder:
    """Incremental decoder that discards noise and resynchronizes on MAGIC."""

    def __init__(self) -> None:
        self._buffer = bytearray()
        self.errors: list[ErrorCode] = []

    def feed(self, data: bytes) -> list[Frame]:
        self._buffer.extend(data)
        frames: list[Frame] = []
        while True:
            magic_at = self._buffer.find(MAGIC)
            if magic_at < 0:
                if len(self._buffer) > len(MAGIC) - 1:
                    del self._buffer[: -(len(MAGIC) - 1)]
                break
            if magic_at:
                del self._buffer[:magic_at]
            if len(self._buffer) < HEADER_SIZE:
                break
            magic, version, command, flags, sequence, payload_len, payload_crc = HEADER.unpack_from(self._buffer)
            if magic != MAGIC:
                del self._buffer[0]
                continue
            if version != VERSION:
                self.errors.append(ErrorCode.PROTOCOL_VERSION)
                del self._buffer[0]
                continue
            if payload_len > MAX_FRAME_PAYLOAD:
                self.errors.append(ErrorCode.FRAME_TOO_LARGE)
                del self._buffer[0]
                continue
            frame_len = HEADER_SIZE + payload_len
            if len(self._buffer) < frame_len:
                # A corrupted length can otherwise hide a later valid frame
                # indefinitely. If another magic already begins after this
                # header, abandon one byte and resynchronize there.
                next_magic = self._buffer.find(MAGIC, 1)
                if next_magic >= 0:
                    del self._buffer[:next_magic]
                    continue
                break
            payload = bytes(self._buffer[HEADER_SIZE:frame_len])
            del self._buffer[:frame_len]
            if crc32(payload) != payload_crc:
                self.errors.append(ErrorCode.BAD_FRAME_CRC)
                continue
            frames.append(Frame(command, sequence, payload, flags, version))
        return frames


def sanitize_filename(name: str) -> str:
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError("filename must be ASCII") from exc
    if not encoded or len(encoded) > MAX_FILENAME_BYTES:
        raise ValueError("filename length must be 1..48 ASCII bytes")
    if name.startswith(".") or ".." in name or "/" in name or "\\" in name:
        raise ValueError("filename must be a plain root filename")
    if any(not (ch.isalnum() or ch in "._-") for ch in name):
        raise ValueError("filename contains unsafe characters")
    lower = name.lower()
    if not (lower.endswith(".mspkg") or lower.endswith(".msp")):
        raise ValueError("filename extension must be .mspkg or .msp")
    return name




INIT_STORAGE_CONFIRMATION = b"FORMAT_FFAT_V1"

def pack_filename(name: str) -> bytes:
    safe = sanitize_filename(name).encode("ascii")
    return struct.pack("<B", len(safe)) + safe


def unpack_filename(payload: bytes) -> str:
    if not payload:
        raise ValueError("short filename payload")
    name_len = payload[0]
    if name_len == 0 or len(payload) != 1 + name_len:
        raise ValueError("bad filename payload length")
    return sanitize_filename(payload[1:].decode("ascii"))


def pack_init_storage_confirmation() -> bytes:
    return INIT_STORAGE_CONFIRMATION


def validate_init_storage_confirmation(payload: bytes) -> None:
    if payload != INIT_STORAGE_CONFIRMATION:
        raise ValueError("invalid storage initialization confirmation")

def pack_begin(filename: str, file_length: int, file_crc: int) -> bytes:
    safe = sanitize_filename(filename)
    if file_length < 32 or file_length > MAX_FILE_BYTES:
        raise ValueError(f"file length must be 32..{MAX_FILE_BYTES}")
    name = safe.encode("ascii")
    return struct.pack("<IIB", file_length, file_crc & 0xFFFFFFFF, len(name)) + name


def unpack_begin(payload: bytes) -> tuple[str, int, int]:
    if len(payload) < 9:
        raise ValueError("short BEGIN")
    file_length, file_crc, name_len = struct.unpack_from("<IIB", payload)
    if len(payload) != 9 + name_len:
        raise ValueError("bad BEGIN filename length")
    name = payload[9:].decode("ascii")
    return sanitize_filename(name), file_length, file_crc


def pack_chunk(offset: int, data: bytes) -> bytes:
    if not data or len(data) > MAX_CHUNK_BYTES:
        raise ValueError("chunk length must be 1..1024")
    return struct.pack("<IHH", offset, len(data), 0) + data


def unpack_chunk(payload: bytes) -> tuple[int, bytes]:
    if len(payload) < 8:
        raise ValueError("short CHUNK")
    offset, chunk_len, reserved = struct.unpack_from("<IHH", payload)
    if reserved != 0 or chunk_len == 0 or chunk_len > MAX_CHUNK_BYTES or len(payload) != 8 + chunk_len:
        raise ValueError("bad CHUNK length")
    return offset, payload[8:]


ACK = struct.Struct("<BBHIII")
STATUS = struct.Struct("<BBHIIII")


def unpack_ack(payload: bytes) -> dict[str, int]:
    if len(payload) != ACK.size:
        raise ValueError("bad ACK/ERROR payload")
    request_command, state, code, next_offset, total_length, file_crc = ACK.unpack(payload)
    return {
        "request_command": request_command,
        "state": state,
        "code": code,
        "next_offset": next_offset,
        "total_length": total_length,
        "file_crc": file_crc,
    }


def unpack_status(payload: bytes) -> dict[str, int]:
    if len(payload) != STATUS.size:
        raise ValueError("bad STATUS payload")
    state, mounted, code, next_offset, total_length, expected_crc, running_crc = STATUS.unpack(payload)
    return {
        "state": state,
        "mounted": mounted,
        "code": code,
        "next_offset": next_offset,
        "total_length": total_length,
        "expected_crc": expected_crc,
        "running_crc": running_crc,
    }
