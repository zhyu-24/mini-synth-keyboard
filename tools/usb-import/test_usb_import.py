from __future__ import annotations

from pathlib import Path
import importlib.util
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from mspkg_usb_protocol import (  # noqa: E402
    Command,
    ErrorCode,
    FrameDecoder,
    MAX_FILE_BYTES,
    crc32,
    encode_frame,
    pack_begin,
    pack_chunk,
    pack_filename,
    pack_init_storage_confirmation,
    sanitize_filename,
    unpack_begin,
    unpack_chunk,
    unpack_filename,
    validate_init_storage_confirmation,
)


def tlv(tag: int, kind: int, value: bytes) -> bytes:
    return struct.pack("<HBBI", tag, kind, 0, len(value)) + value


def make_mspkg(note_count: int = 1) -> bytes:
    notes = b"".join(struct.pack("<IIBBBB", i * 480, 240, 60 + i, 100, 0, 0) for i in range(note_count))
    duration = (note_count - 1) * 480 + 240
    payload = (
        struct.pack("<4sHHIIIHHI", b"MSQ1", 480, 12, note_count, 1, duration, 0, 0, 0)
        + struct.pack("<II", 0, 500000)
        + notes
    )
    source_hash = bytes(range(32))
    metadata = b"".join(
        [
            tlv(1, 1, b"USB Test"),
            tlv(5, 4, source_hash),
            tlv(17, 2, struct.pack("<I", note_count)),
            tlv(18, 2, struct.pack("<I", duration)),
            tlv(19, 3, struct.pack("<i", 0)),
            tlv(22, 2, struct.pack("<I", 1)),
        ]
    )
    header = struct.pack(
        "<4sBBBBIIIIII",
        b"MSPK",
        1,
        0,
        1,
        0,
        32,
        len(metadata),
        len(payload),
        crc32(metadata),
        crc32(payload),
        1,
    )
    return header + metadata + payload


def validate_mspkg(data: bytes) -> ErrorCode:
    if len(data) < 32:
        return ErrorCode.MSPKG_DECLARED_SIZE
    magic, major, minor, content_type, flags, header_bytes, metadata_bytes, payload_bytes, metadata_crc, payload_crc, abi = struct.unpack_from(
        "<4sBBBBIIIIII", data
    )
    if magic != b"MSPK":
        return ErrorCode.MSPKG_MAGIC
    if major != 1 or minor != 0 or flags != 0 or abi > 1:
        return ErrorCode.MSPKG_VERSION
    if content_type != 1:
        return ErrorCode.MSPKG_TYPE
    if header_bytes != 32 or metadata_bytes > 4096 or payload_bytes > 512 * 1024 or 32 + metadata_bytes + payload_bytes != len(data):
        return ErrorCode.MSPKG_DECLARED_SIZE
    metadata = data[32 : 32 + metadata_bytes]
    payload = data[32 + metadata_bytes :]
    if crc32(metadata) != metadata_crc:
        return ErrorCode.METADATA_CRC
    if crc32(payload) != payload_crc:
        return ErrorCode.PAYLOAD_CRC
    if len(payload) < 28 or payload[:4] != b"MSQ1":
        return ErrorCode.PAYLOAD_FORMAT
    return ErrorCode.OK


class MemoryFs:
    def __init__(self) -> None:
        self.files: dict[str, bytes] = {}
        self.fail_final_rename = False
        self.fail_rollback_rename = False

    def commit(self, name: str, temporary: bytes) -> ErrorCode:
        final = "/" + name
        backup = "/.usb-prev"
        had_final = final in self.files
        self.files["/.usb-upload"] = temporary
        if had_final:
            self.files.pop(backup, None)
            self.files[backup] = self.files.pop(final)
        if self.fail_final_rename:
            if had_final and not self.fail_rollback_rename:
                self.files[final] = self.files.pop(backup)
            return ErrorCode.COMMIT_FAILED if final in self.files or not had_final else ErrorCode.ROLLBACK_FAILED
        self.files[final] = self.files.pop("/.usb-upload")
        self.files.pop(backup, None)
        return ErrorCode.OK


class SimulatedDevice:
    def __init__(self, fs: MemoryFs) -> None:
        self.fs = fs
        self.reset_upload()

    def reset_upload(self) -> None:
        self.name = ""
        self.length = 0
        self.expected_crc = 0
        self.temp = bytearray()

    @property
    def offset(self) -> int:
        return len(self.temp)

    def begin(self, payload: bytes) -> ErrorCode:
        name, length, expected_crc = unpack_begin(payload)
        if length > MAX_FILE_BYTES:
            return ErrorCode.FILE_TOO_LARGE
        self.name, self.length, self.expected_crc = name, length, expected_crc
        self.temp = bytearray()
        return ErrorCode.OK

    def chunk(self, payload: bytes) -> ErrorCode:
        offset, data = unpack_chunk(payload)
        if offset == self.offset:
            if self.offset + len(data) > self.length:
                return ErrorCode.CHUNK_LENGTH
            self.temp.extend(data)
            return ErrorCode.OK
        if offset < self.offset and offset + len(data) <= self.offset:
            if self.temp[offset : offset + len(data)] == data:
                return ErrorCode.OK
        return ErrorCode.OFFSET_MISMATCH

    def finish(self) -> ErrorCode:
        if self.offset != self.length:
            return ErrorCode.FILE_LENGTH
        if crc32(self.temp) != self.expected_crc:
            return ErrorCode.FILE_CRC
        validation = validate_mspkg(bytes(self.temp))
        if validation != ErrorCode.OK:
            return validation
        result = self.fs.commit(self.name, bytes(self.temp))
        if result == ErrorCode.OK:
            self.reset_upload()
        return result


class ProtocolTests(unittest.TestCase):
    def test_frame_encode_decode_crc_and_resync(self) -> None:
        payload = b"abc\x00def"
        encoded = encode_frame(Command.CHUNK, 77, payload)
        decoder = FrameDecoder()
        frames = decoder.feed(b"noiseMU" + encoded[:5])
        self.assertEqual(frames, [])
        frames = decoder.feed(encoded[5:])
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].sequence, 77)
        self.assertEqual(frames[0].payload, payload)
        damaged = bytearray(encoded)
        damaged[-1] ^= 0x80
        self.assertEqual(decoder.feed(bytes(damaged)), [])
        self.assertIn(ErrorCode.BAD_FRAME_CRC, decoder.errors)
        # A plausible but incomplete corrupt frame must not hide a complete
        # valid frame that follows it in the same stream.
        corrupt_header = bytearray(encode_frame(Command.STATUS, 88))
        struct.pack_into("<I", corrupt_header, 12, 1000)
        recovered = decoder.feed(bytes(corrupt_header) + encode_frame(Command.STATUS, 89))
        self.assertEqual([frame.sequence for frame in recovered], [89])
        self.assertEqual(crc32(b"123456789"), 0xCBF43926)

    def test_path_sanitization(self) -> None:
        for good in ["song.mspkg", "SONG-01.MSP", "a_b.mspkg"]:
            self.assertEqual(sanitize_filename(good), good)
        for bad in ["", ".hidden.msp", "../song.msp", "dir/song.mspkg", "a\\b.msp", "中文.mspkg", "song.bin", "a..b.msp"]:
            with self.assertRaises(ValueError, msg=bad):
                sanitize_filename(bad)

    def test_delete_and_storage_init_codecs(self) -> None:
        self.assertEqual(unpack_filename(pack_filename("SONG01.MSP")), "SONG01.MSP")
        with self.assertRaises(ValueError):
            unpack_filename(b"\x05abc")
        confirmation = pack_init_storage_confirmation()
        validate_init_storage_confirmation(confirmation)
        with self.assertRaises(ValueError):
            validate_init_storage_confirmation(b"FORMAT_FFAT")
        self.assertEqual(Command.DELETE, 0x07)
        self.assertEqual(Command.INIT_STORAGE, 0x08)
        self.assertEqual(ErrorCode.DELETE_FAILED, 26)
        self.assertEqual(ErrorCode.FORMAT_FAILED, 27)

    def test_begin_and_chunk_codecs(self) -> None:
        name, length, checksum = unpack_begin(pack_begin("song.mspkg", 1234, 0x12345678))
        self.assertEqual((name, length, checksum), ("song.mspkg", 1234, 0x12345678))
        offset, data = unpack_chunk(pack_chunk(1024, b"xyz"))
        self.assertEqual((offset, data), (1024, b"xyz"))


class UploadSimulationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fs = MemoryFs()
        self.device = SimulatedDevice(self.fs)
        self.package = make_mspkg(3)

    def begin(self, expected_crc: int | None = None) -> None:
        checksum = crc32(self.package) if expected_crc is None else expected_crc
        self.assertEqual(self.device.begin(pack_begin("song.mspkg", len(self.package), checksum)), ErrorCode.OK)

    def upload_all(self, step: int = 37) -> None:
        for offset in range(0, len(self.package), step):
            self.assertEqual(self.device.chunk(pack_chunk(offset, self.package[offset : offset + step])), ErrorCode.OK)

    def test_normal_upload(self) -> None:
        self.begin()
        self.upload_all()
        self.assertEqual(self.device.finish(), ErrorCode.OK)
        self.assertEqual(self.fs.files["/song.mspkg"], self.package)

    def test_bad_whole_file_crc(self) -> None:
        self.begin(expected_crc=crc32(self.package) ^ 1)
        self.upload_all()
        self.assertEqual(self.device.finish(), ErrorCode.FILE_CRC)
        self.assertNotIn("/song.mspkg", self.fs.files)

    def test_wrong_offset(self) -> None:
        self.begin()
        self.assertEqual(self.device.chunk(pack_chunk(10, b"bad")), ErrorCode.OFFSET_MISMATCH)
        self.assertEqual(self.device.offset, 0)

    def test_duplicate_retry_chunk_is_idempotent(self) -> None:
        self.begin()
        first = self.package[:64]
        self.assertEqual(self.device.chunk(pack_chunk(0, first)), ErrorCode.OK)
        self.assertEqual(self.device.chunk(pack_chunk(0, first)), ErrorCode.OK)
        self.assertEqual(self.device.offset, 64)
        altered = bytes([first[0] ^ 1]) + first[1:]
        self.assertEqual(self.device.chunk(pack_chunk(0, altered)), ErrorCode.OFFSET_MISMATCH)

    def test_interrupted_upload_preserves_existing_final(self) -> None:
        old = b"old valid file"
        self.fs.files["/song.mspkg"] = old
        self.begin()
        self.assertEqual(self.device.chunk(pack_chunk(0, self.package[:40])), ErrorCode.OK)
        self.assertEqual(self.fs.files["/song.mspkg"], old)
        self.assertNotIn("/.usb-upload", self.fs.files)

    def test_atomic_commit_rollback(self) -> None:
        old = b"old valid file"
        self.fs.files["/song.mspkg"] = old
        self.fs.fail_final_rename = True
        self.begin()
        self.upload_all()
        self.assertEqual(self.device.finish(), ErrorCode.COMMIT_FAILED)
        self.assertEqual(self.fs.files["/song.mspkg"], old)
        self.assertNotIn("/.usb-prev", self.fs.files)

    def test_metadata_and_payload_crc_errors(self) -> None:
        metadata_bad = bytearray(self.package)
        metadata_bad[40] ^= 1
        self.assertEqual(validate_mspkg(bytes(metadata_bad)), ErrorCode.METADATA_CRC)
        payload_bad = bytearray(self.package)
        metadata_len = struct.unpack_from("<I", self.package, 12)[0]
        payload_bad[32 + metadata_len + 4] ^= 1
        self.assertEqual(validate_mspkg(bytes(payload_bad)), ErrorCode.PAYLOAD_CRC)


if __name__ == "__main__":
    unittest.main()
