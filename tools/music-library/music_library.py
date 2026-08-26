#!/usr/bin/env python3
"""Mini Synth music-library preparation and USB synchronization tool.

Python 3.10+. The manager itself uses only the standard library and pyserial.
MusicXML conversion is delegated to the repository's existing converter.
"""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
from dataclasses import asdict, dataclass
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import time
from typing import Callable, Iterable, Sequence
import unicodedata
import zlib

HERE = Path(__file__).resolve().parent
TOOLS_DIR = HERE.parent
USB_IMPORT_DIR = TOOLS_DIR / "usb-import"
MEDIA_IMPORT_DIR = TOOLS_DIR / "media-import"
for import_dir in (USB_IMPORT_DIR, MEDIA_IMPORT_DIR):
    if str(import_dir) not in sys.path:
        sys.path.insert(0, str(import_dir))

from mspkg_usb_protocol import (  # noqa: E402
    MAX_FILE_BYTES,
    MAX_FILENAME_BYTES,
    MAX_METADATA_BYTES,
    MAX_MSPKG_PAYLOAD_BYTES,
    ErrorCode,
    sanitize_filename,
)
from usb_mspkg_import import (  # noqa: E402
    ERROR_NAMES,
    ImportProtocolError,
    STATE_NAMES,
    Transport,
    delete_file,
    get_files,
    get_status,
    initialize_storage,
    upload_file,
)

MAX_LIBRARY_SONGS = 30
FFAT_PARTITION_CAPACITY_BYTES = 0x9E0000
FFAT_RESERVE_PERCENT = 5
FFAT_FIXED_RESERVE_BYTES = 128 * 1024
FFAT_RESERVED_BYTES = (
    (FFAT_PARTITION_CAPACITY_BYTES * FFAT_RESERVE_PERCENT + 99) // 100
    + FFAT_FIXED_RESERVE_BYTES
)
FFAT_SAFE_LIBRARY_BYTES = FFAT_PARTITION_CAPACITY_BYTES - FFAT_RESERVED_BYTES
SUPPORTED_EXTENSIONS = {".mspkg", ".msp", ".musicxml", ".xml"}
PACKAGE_EXTENSIONS = {".mspkg", ".msp"}
MUSICXML_EXTENSIONS = {".musicxml", ".xml"}
MSPKG_HEADER = struct.Struct("<4sBBBBIIIIII")
TLV_HEADER = struct.Struct("<HBBI")
SEQUENCE_HEADER = struct.Struct("<4sHHIIIHHI")
DEVICE_BUSY_RETRY_SECONDS = 5.0
DEVICE_BUSY_RETRY_INTERVAL_SECONDS = 0.05

ProgressCallback = Callable[[str, dict[str, object]], None]


@dataclass(frozen=True)
class PackageInfo:
    size: int
    content_type: str
    format_version: str
    metadata_bytes: int
    payload_bytes: int
    note_count: int
    tempo_count: int
    duration_ticks: int
    sha256: str


@dataclass(frozen=True)
class PreparedSource:
    source: str
    source_type: str
    prepared_path: str
    device_name: str
    size: int
    sha256: str
    converted: bool
    package: dict[str, object]


@dataclass(frozen=True)
class SyncPlan:
    desired: tuple[tuple[str, int], ...]
    current: tuple[tuple[str, int], ...]
    delete: tuple[str, ...]
    add: tuple[str, ...]
    replace: tuple[str, ...]
    exact: tuple[str, ...]
    upload: tuple[str, ...]


@dataclass(frozen=True)
class ImportAssignment:
    incoming_name: str
    target_name: str
    size: int
    replaces_existing: bool


@dataclass(frozen=True)
class ImportPlan:
    current: tuple[tuple[str, int], ...]
    final: tuple[tuple[str, int], ...]
    preserved: tuple[str, ...]
    added: tuple[str, ...]
    replaced: tuple[str, ...]
    assignments: tuple[ImportAssignment, ...]
    upload: tuple[str, ...]


@dataclass(frozen=True)
class ReorderAssignment:
    incoming_name: str
    current_name: str
    target_name: str
    size: int


@dataclass(frozen=True)
class ReorderPlan:
    current: tuple[tuple[str, int], ...]
    final: tuple[tuple[str, int], ...]
    assignments: tuple[ReorderAssignment, ...]
    renamed: tuple[tuple[str, str], ...]
    delete: tuple[str, ...]
    upload: tuple[str, ...]


@dataclass(frozen=True)
class CapacityPreflight:
    partition_bytes: int
    reserved_bytes: int
    safe_library_bytes: int
    desired_bytes: int
    estimated_peak_bytes: int


def _jsonable(value: object) -> object:
    if hasattr(value, "__dataclass_fields__"):
        return asdict(value)  # type: ignore[arg-type]
    if isinstance(value, tuple):
        return [_jsonable(item) for item in value]
    if isinstance(value, list):
        return [_jsonable(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _jsonable(item) for key, item in value.items()}
    return value


def result_json(payload: dict[str, object]) -> None:
    print("RESULT_JSON=" + json.dumps(_jsonable(payload), ensure_ascii=False, sort_keys=True))


def _emit(callback: ProgressCallback | None, event: str, **details: object) -> None:
    if callback is not None:
        callback(event, details)


def _run_with_busy_retry(
    operation: Callable[[], None],
    *,
    timeout: float = DEVICE_BUSY_RETRY_SECONDS,
    interval: float = DEVICE_BUSY_RETRY_INTERVAL_SECONDS,
) -> None:
    """Retry a mutating command while firmware refreshes its runtime catalog."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            operation()
            return
        except ImportProtocolError as exc:
            if exc.code != int(ErrorCode.BUSY) or time.monotonic() >= deadline:
                raise
            time.sleep(interval)


def _slug_from_stem(stem: str) -> str:
    normalized = unicodedata.normalize("NFKD", stem)
    ascii_text = normalized.encode("ascii", "ignore").decode("ascii").lower()
    pieces: list[str] = []
    previous_dash = False
    for char in ascii_text:
        if char.isalnum():
            pieces.append(char)
            previous_dash = False
        elif not previous_dash and pieces:
            pieces.append("-")
            previous_dash = True
    slug = "".join(pieces).strip("-")
    had_non_ascii = any(ord(char) > 127 for char in stem)
    digest = hashlib.sha256(stem.encode("utf-8")).hexdigest()[:8]
    if not slug:
        return f"song-{digest}"
    if had_non_ascii:
        return f"{slug}-{digest}"
    return slug


def safe_device_name(
    source_name: str,
    position: int,
    used_names: Iterable[str] = (),
    output_extension: str | None = None,
) -> str:
    """Create a deterministic, ordering-aware, safe ASCII device filename.

    The two-digit prefix makes the desired GUI order equal the firmware's
    case-insensitive lexical catalog order. Unicode-only names receive a stable
    hash rather than collapsing to the same generic name.
    """
    if not 1 <= position <= MAX_LIBRARY_SONGS:
        raise ValueError(f"position must be 1..{MAX_LIBRARY_SONGS}")
    source = Path(source_name)
    source_extension = source.suffix.lower()
    extension = (output_extension or (source_extension if source_extension in PACKAGE_EXTENSIONS else ".mspkg")).lower()
    if extension not in PACKAGE_EXTENSIONS:
        raise ValueError("device filename extension must be .mspkg or .msp")

    used_lower = {name.lower() for name in used_names}
    prefix = f"{position:02d}-"
    reserve = len(prefix) + len(extension)
    max_slug = MAX_FILENAME_BYTES - reserve
    if max_slug < 1:
        raise AssertionError("filename constants leave no room for a stem")
    # A package copied back from the device already has an order prefix. Do
    # not turn it into e.g. 01-04-song when preparing a new operation.
    source_stem = re.sub(r"^\d{2}-(?=.+)", "", source.stem)
    slug = _slug_from_stem(source_stem)[:max_slug].rstrip("-") or "song"
    candidate = f"{prefix}{slug}{extension}"
    suffix_number = 2
    while candidate.lower() in used_lower:
        suffix = f"-{suffix_number}"
        base = slug[: max_slug - len(suffix)].rstrip("-") or "song"
        candidate = f"{prefix}{base}{suffix}{extension}"
        suffix_number += 1
    return sanitize_filename(candidate)


def generate_safe_device_names(source_names: Sequence[str]) -> list[str]:
    if len(source_names) > MAX_LIBRARY_SONGS:
        raise ValueError(f"the device supports at most {MAX_LIBRARY_SONGS} file songs")
    names: list[str] = []
    for position, source_name in enumerate(source_names, 1):
        extension = Path(source_name).suffix.lower()
        output_extension = extension if extension in PACKAGE_EXTENSIONS else ".mspkg"
        names.append(safe_device_name(source_name, position, names, output_extension))
    return names


def validate_package_bytes(data: bytes) -> PackageInfo:
    """Validate the same MSPKG container and basic sequence rules as USB firmware."""
    if len(data) < MSPKG_HEADER.size:
        raise ValueError("文件小于 32 字节，不是完整的 MSPKG")
    (
        magic,
        major,
        minor,
        content_type,
        flags,
        header_bytes,
        metadata_bytes,
        payload_bytes,
        metadata_crc,
        payload_crc,
        minimum_abi,
    ) = MSPKG_HEADER.unpack_from(data)
    if magic != b"MSPK":
        raise ValueError("MSPKG magic 错误")
    if major != 1 or minor != 0 or flags != 0 or minimum_abi > 1:
        raise ValueError("设备仅支持 MSPKG v1.0、flags=0、固件 ABI<=1")
    if content_type != 1:
        raise ValueError("设备当前仅支持 SEQUENCE 类型 MSPKG")
    if (
        header_bytes != MSPKG_HEADER.size
        or metadata_bytes > MAX_METADATA_BYTES
        or payload_bytes > MAX_MSPKG_PAYLOAD_BYTES
        or header_bytes + metadata_bytes + payload_bytes != len(data)
        or len(data) > MAX_FILE_BYTES
    ):
        raise ValueError("MSPKG 声明尺寸不合法或超出设备容量")

    metadata = data[header_bytes : header_bytes + metadata_bytes]
    payload = data[header_bytes + metadata_bytes :]
    if zlib.crc32(metadata) & 0xFFFFFFFF != metadata_crc:
        raise ValueError("MSPKG metadata CRC-32 校验失败")
    if zlib.crc32(payload) & 0xFFFFFFFF != payload_crc:
        raise ValueError("MSPKG payload CRC-32 校验失败")

    offset = 0
    while offset < len(metadata):
        if len(metadata) - offset < TLV_HEADER.size:
            raise ValueError("MSPKG metadata TLV 不完整")
        _tag, _kind, _tlv_flags, length = TLV_HEADER.unpack_from(metadata, offset)
        offset += TLV_HEADER.size
        if length > len(metadata) - offset:
            raise ValueError("MSPKG metadata TLV 长度越界")
        offset += length

    if len(payload) < SEQUENCE_HEADER.size:
        raise ValueError("MSPKG sequence header 不完整")
    (
        sequence_magic,
        ticks_per_quarter,
        note_record_bytes,
        note_count,
        tempo_count,
        duration_ticks,
        time_signature_count,
        reserved16,
        reserved32,
    ) = SEQUENCE_HEADER.unpack_from(payload)
    if (
        sequence_magic != b"MSQ1"
        or ticks_per_quarter == 0
        or note_record_bytes != 12
        or note_count == 0
        or tempo_count == 0
        or reserved16 != 0
        or reserved32 != 0
    ):
        raise ValueError("MSPKG sequence 基本结构不受支持")
    expected_payload = SEQUENCE_HEADER.size + tempo_count * 8 + time_signature_count * 8 + note_count * 12
    if expected_payload != len(payload):
        raise ValueError("MSPKG sequence 记录数量与 payload 尺寸不一致")
    return PackageInfo(
        size=len(data),
        content_type="SEQUENCE",
        format_version=f"{major}.{minor}",
        metadata_bytes=metadata_bytes,
        payload_bytes=payload_bytes,
        note_count=note_count,
        tempo_count=tempo_count,
        duration_ticks=duration_ticks,
        sha256=hashlib.sha256(data).hexdigest(),
    )


def validate_package(path: Path | str) -> PackageInfo:
    package_path = Path(path)
    if package_path.suffix.lower() not in PACKAGE_EXTENSIONS:
        raise ValueError("包文件扩展名必须是 .mspkg 或 .msp")
    if not package_path.is_file():
        raise ValueError(f"文件不存在：{package_path}")
    return validate_package_bytes(package_path.read_bytes())


def source_preparation_metadata(
    source: Path | str,
    prepared_path: Path | str,
    device_name: str,
    package_info: PackageInfo,
    converted: bool,
) -> PreparedSource:
    """Pure construction of stable metadata describing one prepared source."""
    source_path = Path(source)
    return PreparedSource(
        source=str(source_path.resolve()),
        source_type="musicxml" if source_path.suffix.lower() in MUSICXML_EXTENSIONS else "package",
        prepared_path=str(Path(prepared_path).resolve()),
        device_name=device_name,
        size=package_info.size,
        sha256=package_info.sha256,
        converted=converted,
        package=asdict(package_info),
    )


def _load_converter_build() -> Callable[[Path, Path, Path], object]:
    converter_path = MEDIA_IMPORT_DIR / "convert_musicxml.py"
    spec = importlib.util.spec_from_file_location("mini_synth_convert_musicxml", converter_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法载入 MusicXML 转换器：{converter_path}")
    module = importlib.util.module_from_spec(spec)
    # dataclasses and similar runtime helpers resolve the defining module via
    # sys.modules while decorators execute.
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except ModuleNotFoundError as exc:
        if exc.name == "numpy":
            raise RuntimeError("MusicXML 转换需要现有转换器的 NumPy 依赖；请先运行：python -m pip install numpy") from exc
        raise
    return module.build


def prepare_sources(
    sources: Sequence[Path | str],
    output_dir: Path | str,
    progress: ProgressCallback | None = None,
) -> list[PreparedSource]:
    """Validate/copy packages and convert MusicXML into a clean work directory."""
    if len(sources) > MAX_LIBRARY_SONGS:
        raise ValueError(f"设备最多只能保存 {MAX_LIBRARY_SONGS} 首文件歌曲")
    source_paths = [Path(source).expanduser() for source in sources]
    for source in source_paths:
        if not source.is_file():
            raise ValueError(f"找不到源文件：{source}")
        if source.suffix.lower() not in SUPPORTED_EXTENSIONS:
            raise ValueError(f"不支持的文件类型：{source.name}")

    output = Path(output_dir).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    names = generate_safe_device_names([source.name for source in source_paths])
    converter_build: Callable[[Path, Path, Path], object] | None = None
    prepared: list[PreparedSource] = []

    for index, (source, device_name) in enumerate(zip(source_paths, names), 1):
        source = source.resolve()
        extension = source.suffix.lower()
        converted = extension in MUSICXML_EXTENSIONS
        _emit(progress, "prepare_start", index=index, total=len(source_paths), source=str(source))
        if converted:
            if converter_build is None:
                converter_build = _load_converter_build()
            conversion_dir = output / f"conversion-{index:02d}"
            if conversion_dir.exists():
                shutil.rmtree(conversion_dir)
            conversion_dir.mkdir(parents=True)
            # The existing converter prints its manifest; this manager owns the
            # CLI/GUI output, so keep conversion stdout out of RESULT_JSON/logs.
            with open(conversion_dir / "converter-output.txt", "w", encoding="utf-8") as converter_log:
                with redirect_stdout(converter_log):
                    converter_build(source, conversion_dir, MEDIA_IMPORT_DIR / "device-profile.json")
            packages = sorted(conversion_dir.glob("*.mspkg"))
            if len(packages) != 1:
                raise RuntimeError(f"MusicXML 转换未产生唯一 MSPKG：{source.name}")
            package_source = packages[0]
        else:
            package_source = source

        destination = output / device_name
        if destination.resolve() != package_source.resolve():
            shutil.copyfile(package_source, destination)
        copied_info = validate_package(destination)
        metadata = source_preparation_metadata(source, destination, device_name, copied_info, converted)
        prepared.append(metadata)
        _emit(progress, "prepare_done", index=index, total=len(source_paths), item=asdict(metadata))
    preflight_library_capacity([], [(item.device_name, item.size) for item in prepared])
    return prepared


def _format_capacity_bytes(size: int) -> str:
    return f"{size:,} 字节（{size / (1024 * 1024):.2f} MiB）"


def preflight_library_capacity(
    current: Iterable[tuple[str, int]],
    desired: Iterable[tuple[str, int]],
    *,
    upload_names: Iterable[str] | None = None,
) -> CapacityPreflight:
    """Conservatively prove that a sync fits the fixed FFat partition.

    Files outside the desired set are deleted before uploading, so they do not
    contribute to the upload-stage peak. For a replacement, budget the current
    file, an equally sized rollback backup, and the new temporary upload at the
    same time. ``upload_names`` limits peak simulation to files actually written
    by incremental import; exact sync keeps the conservative all-files default.
    """
    current_tuple = tuple((sanitize_filename(name), int(size)) for name, size in current)
    desired_tuple = tuple((sanitize_filename(name), int(size)) for name, size in desired)
    if len(desired_tuple) > MAX_LIBRARY_SONGS:
        raise ValueError(f"设备最多只能保存 {MAX_LIBRARY_SONGS} 首文件歌曲")
    for name, size in desired_tuple:
        if size < 32 or size > MAX_FILE_BYTES:
            raise ValueError(f"目标文件 {name} 的尺寸必须为 32..{MAX_FILE_BYTES} 字节")
    for name, size in current_tuple:
        if size < 0:
            raise ValueError(f"设备文件 {name} 报告了无效的负数尺寸")

    desired_names = [name.lower() for name, _size in desired_tuple]
    if len(set(desired_names)) != len(desired_names):
        raise ValueError("目标音乐库含重复设备文件名")

    desired_map = {name.lower(): size for name, size in desired_tuple}
    upload_keys = (
        set(desired_names)
        if upload_names is None
        else {sanitize_filename(name).lower() for name in upload_names}
    )
    unknown_uploads = upload_keys.difference(desired_map)
    if unknown_uploads:
        raise ValueError(f"上传计划包含不在目标曲库中的文件：{sorted(unknown_uploads)}")
    live_sizes = {
        name.lower(): size
        for name, size in current_tuple
        if name.lower() in desired_map
    }
    live_bytes = sum(live_sizes.values())
    desired_bytes = sum(size for _name, size in desired_tuple)
    estimated_peak_bytes = max(live_bytes, desired_bytes)

    for name, new_size in desired_tuple:
        key = name.lower()
        if key not in upload_keys:
            continue
        old_size = live_sizes.get(key, 0)
        # live_bytes already includes the current file. Add a conservative
        # second old-size allowance for rollback backup plus the new temp file.
        estimated_peak_bytes = max(estimated_peak_bytes, live_bytes + old_size + new_size)
        live_bytes += new_size - old_size
        live_sizes[key] = new_size

    report = CapacityPreflight(
        partition_bytes=FFAT_PARTITION_CAPACITY_BYTES,
        reserved_bytes=FFAT_RESERVED_BYTES,
        safe_library_bytes=FFAT_SAFE_LIBRARY_BYTES,
        desired_bytes=desired_bytes,
        estimated_peak_bytes=estimated_peak_bytes,
    )
    if desired_bytes > FFAT_SAFE_LIBRARY_BYTES or estimated_peak_bytes > FFAT_SAFE_LIBRARY_BYTES:
        excess = max(desired_bytes, estimated_peak_bytes) - FFAT_SAFE_LIBRARY_BYTES
        raise ValueError(
            "目标音乐库容量预检失败：FFat 分区为 "
            f"{_format_capacity_bytes(FFAT_PARTITION_CAPACITY_BYTES)}，已预留至少 5% 加 128 KiB "
            f"（共 {_format_capacity_bytes(FFAT_RESERVED_BYTES)}）用于 FAT、磨损均衡以及原子临时/备份文件；"
            f"安全可用上限为 {_format_capacity_bytes(FFAT_SAFE_LIBRARY_BYTES)}。"
            f"目标文件合计 {_format_capacity_bytes(desired_bytes)}，按替换时同时保留当前文件、备份和临时文件估算，"
            f"峰值为 {_format_capacity_bytes(estimated_peak_bytes)}，超出 {_format_capacity_bytes(excess)}。"
            "请减少歌曲数量或使用更小的包。容量预检发生在删除和上传之前，设备尚未被修改。"
        )
    return report


def calculate_sync_plan(
    current: Iterable[tuple[str, int]],
    desired: Iterable[tuple[str, int]],
    *,
    allow_empty: bool = False,
) -> SyncPlan:
    """Pure calculation of add/replace/delete/exact sets.

    Every desired file is uploaded through the device's CRC-checked atomic
    BEGIN/CHUNK/FINISH path. LIST exposes only filename and size, so rewriting
    exact-size entries is required to guarantee the selected content itself.
    """
    current_tuple = tuple((sanitize_filename(name), int(size)) for name, size in current)
    desired_tuple = tuple((sanitize_filename(name), int(size)) for name, size in desired)
    if len(desired_tuple) > MAX_LIBRARY_SONGS:
        raise ValueError(f"设备最多只能保存 {MAX_LIBRARY_SONGS} 首文件歌曲")
    if not desired_tuple and not allow_empty:
        raise ValueError("空音乐库会删除设备上的全部文件歌曲；必须显式允许空同步")
    desired_names = [name.lower() for name, _size in desired_tuple]
    if len(set(desired_names)) != len(desired_names):
        raise ValueError("目标音乐库含重复设备文件名")

    current_map = {name.lower(): (name, size) for name, size in current_tuple}
    desired_map = {name.lower(): (name, size) for name, size in desired_tuple}
    delete = tuple(name for name, _size in current_tuple if name.lower() not in desired_map)
    add = tuple(name for name, _size in desired_tuple if name.lower() not in current_map)
    replace = tuple(
        name
        for name, size in desired_tuple
        if name.lower() in current_map and current_map[name.lower()][1] != size
    )
    exact = tuple(
        name
        for name, size in desired_tuple
        if name.lower() in current_map and current_map[name.lower()][1] == size
    )
    return SyncPlan(
        desired=desired_tuple,
        current=current_tuple,
        delete=delete,
        add=add,
        replace=replace,
        exact=exact,
        upload=tuple(name for name, _size in desired_tuple),
    )


DEVICE_ORDER_PREFIX = re.compile(r"^(?P<position>\d{2})-(?P<logical>.+)$")


def logical_device_name(name: str) -> str:
    """Return a case-insensitive identity without order prefix or extension."""
    safe_name = sanitize_filename(name)
    suffix = Path(safe_name).suffix.lower()
    if suffix not in PACKAGE_EXTENSIONS:
        raise ValueError(f"设备歌曲必须使用 .mspkg 或 .msp 扩展名：{safe_name}")
    stem = Path(safe_name).stem
    match = DEVICE_ORDER_PREFIX.fullmatch(stem)
    logical = match.group("logical") if match else stem
    if not logical:
        raise ValueError(f"设备歌曲缺少逻辑文件名：{safe_name}")
    return logical.casefold()


def _logical_device_stem(name: str) -> str:
    """Return the spelling of a logical name while removing its order prefix."""
    safe_name = sanitize_filename(name)
    stem = Path(safe_name).stem
    match = DEVICE_ORDER_PREFIX.fullmatch(stem)
    return match.group("logical") if match else stem


def _device_position(name: str) -> int | None:
    match = DEVICE_ORDER_PREFIX.fullmatch(Path(name).stem)
    if not match:
        return None
    position = int(match.group("position"))
    return position if 1 <= position <= MAX_LIBRARY_SONGS else None


def _unique_logical_map(files: Iterable[tuple[str, int]], label: str) -> dict[str, tuple[str, int]]:
    result: dict[str, tuple[str, int]] = {}
    for raw_name, raw_size in files:
        name = sanitize_filename(raw_name)
        key = logical_device_name(name)
        if key in result:
            raise ValueError(
                f"{label}存在同名歧义：{result[key][0]} 与 {name}；请先删除或重命名其中一项"
            )
        result[key] = (name, int(raw_size))
    return result


def calculate_import_plan(
    current: Iterable[tuple[str, int]],
    incoming: Iterable[tuple[str, int]],
) -> ImportPlan:
    """Plan non-destructive additions and same-logical-name replacements."""
    current_tuple = tuple((sanitize_filename(name), int(size)) for name, size in current)
    incoming_tuple = tuple((sanitize_filename(name), int(size)) for name, size in incoming)
    if not incoming_tuple:
        raise ValueError("增量导入至少需要一首歌曲")
    if len(incoming_tuple) > MAX_LIBRARY_SONGS:
        raise ValueError(f"一次最多只能导入 {MAX_LIBRARY_SONGS} 首歌曲")
    for name, size in incoming_tuple:
        if size < 32 or size > MAX_FILE_BYTES:
            raise ValueError(f"导入文件 {name} 的尺寸必须为 32..{MAX_FILE_BYTES} 字节")

    current_by_logical = _unique_logical_map(current_tuple, "设备曲库")
    incoming_by_logical = _unique_logical_map(incoming_tuple, "待导入列表")
    final_count = len(current_tuple) + sum(1 for key in incoming_by_logical if key not in current_by_logical)
    if final_count > MAX_LIBRARY_SONGS:
        raise ValueError(
            f"增量导入后将有 {final_count} 首，超过设备上限 {MAX_LIBRARY_SONGS} 首；请先删除或使用精确同步整理"
        )

    used_positions = {
        position
        for name, _size in current_tuple
        if (position := _device_position(name)) is not None
    }
    assignments: list[ImportAssignment] = []
    added: list[str] = []
    replaced: list[str] = []
    replacement_sizes: dict[str, int] = {}
    additions: list[tuple[str, int]] = []

    for incoming_name, size in incoming_tuple:
        logical = logical_device_name(incoming_name)
        existing = current_by_logical.get(logical)
        if existing is not None:
            target_name = existing[0]
            replaces_existing = True
            replaced.append(target_name)
            replacement_sizes[target_name.lower()] = size
        else:
            position = next(
                (candidate for candidate in range(1, MAX_LIBRARY_SONGS + 1) if candidate not in used_positions),
                0,
            )
            if position == 0:
                raise ValueError("设备没有可用的歌曲编号位置")
            used_positions.add(position)
            extension = Path(incoming_name).suffix.lower()
            target_name = sanitize_filename(f"{position:02d}-{logical}{extension}")
            replaces_existing = False
            added.append(target_name)
            additions.append((target_name, size))
        assignments.append(ImportAssignment(incoming_name, target_name, size, replaces_existing))

    final_files = [
        (name, replacement_sizes.get(name.lower(), size))
        for name, size in current_tuple
    ]
    final_files.extend(additions)
    final_tuple = tuple(sorted(final_files, key=lambda item: item[0].casefold()))
    replaced_keys = {name.lower() for name in replaced}
    preserved = tuple(name for name, _size in current_tuple if name.lower() not in replaced_keys)
    upload = tuple(assignment.target_name for assignment in assignments)
    return ImportPlan(
        current=current_tuple,
        final=final_tuple,
        preserved=preserved,
        added=tuple(added),
        replaced=tuple(replaced),
        assignments=tuple(assignments),
        upload=upload,
    )


def calculate_reorder_plan(
    current: Iterable[tuple[str, int]],
    incoming: Iterable[tuple[str, int]],
) -> ReorderPlan:
    """Plan a continuous 01..N device order using a complete ordered local set."""
    current_tuple = tuple((sanitize_filename(name), int(size)) for name, size in current)
    incoming_tuple = tuple((sanitize_filename(name), int(size)) for name, size in incoming)
    if not current_tuple:
        raise ValueError("设备音乐库为空，无需重排")
    if not incoming_tuple:
        raise ValueError("重排需要设备全部歌曲对应的本地源文件")
    if len(current_tuple) != len(incoming_tuple):
        raise ValueError(
            f"重排要求本地歌曲与设备曲库一一对应：设备 {len(current_tuple)} 首，本地 {len(incoming_tuple)} 首"
        )

    current_by_logical = _unique_logical_map(current_tuple, "设备曲库")
    incoming_by_logical = _unique_logical_map(incoming_tuple, "本地重排列表")
    current_keys = set(current_by_logical)
    incoming_keys = set(incoming_by_logical)
    if current_keys != incoming_keys:
        missing = sorted(current_keys - incoming_keys)
        extra = sorted(incoming_keys - current_keys)
        details: list[str] = []
        if missing:
            details.append("本地缺少：" + "、".join(missing))
        if extra:
            details.append("本地多出：" + "、".join(extra))
        raise ValueError("重排列表与设备逻辑歌曲不一致；" + "；".join(details))

    assignments: list[ReorderAssignment] = []
    renamed: list[tuple[str, str]] = []
    final: list[tuple[str, int]] = []
    for position, (incoming_name, incoming_size) in enumerate(incoming_tuple, 1):
        logical = logical_device_name(incoming_name)
        current_name, current_size = current_by_logical[logical]
        if incoming_size != current_size:
            raise ValueError(
                f"重排源与设备文件大小不一致：{incoming_name} 为 {incoming_size} 字节，"
                f"{current_name} 为 {current_size} 字节；为避免意外替换内容，未写入设备"
            )
        target_name = sanitize_filename(
            f"{position:02d}-{_logical_device_stem(current_name)}{Path(current_name).suffix.lower()}"
        )
        assignments.append(ReorderAssignment(incoming_name, current_name, target_name, incoming_size))
        final.append((target_name, incoming_size))
        if target_name.casefold() != current_name.casefold():
            renamed.append((current_name, target_name))

    target_keys = {name.casefold() for name, _size in final}
    delete = tuple(name for name, _size in current_tuple if name.casefold() not in target_keys)
    return ReorderPlan(
        current=current_tuple,
        final=tuple(final),
        assignments=tuple(assignments),
        renamed=tuple(renamed),
        delete=delete,
        upload=tuple(name for name, _size in final),
    )


def list_ports_data() -> list[dict[str, str]]:
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("缺少 pyserial；请运行：python -m pip install pyserial") from exc
    return [
        {"device": port.device, "description": port.description, "hwid": port.hwid}
        for port in list_ports.comports()
    ]


def _catalog_order(files: Iterable[tuple[str, int]]) -> list[tuple[str, int]]:
    """Mirror the firmware catalog's case-insensitive filename ordering."""
    return sorted(
        ((sanitize_filename(name), int(size)) for name, size in files),
        key=lambda item: (item[0].casefold(), item[0]),
    )


def show_device(port: str, *, baud: int = 115200, timeout: float = 2.0, retries: int = 3) -> dict[str, object]:
    transport = Transport(port, baud, timeout, retries)
    try:
        status = get_status(transport)
        files = _catalog_order(get_files(transport))
    finally:
        transport.close()
    return {
        "port": port,
        "status": {
            **status,
            "state_name": STATE_NAMES.get(status["state"], str(status["state"])),
            "result_name": ERROR_NAMES.get(status["code"], str(status["code"])),
        },
        "files": [{"name": name, "size": size} for name, size in files],
    }


def sync_prepared(
    port: str,
    prepared: Sequence[PreparedSource],
    *,
    allow_empty: bool = False,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
    progress: ProgressCallback | None = None,
) -> dict[str, object]:
    if len(prepared) > MAX_LIBRARY_SONGS:
        raise ValueError(f"设备最多只能保存 {MAX_LIBRARY_SONGS} 首文件歌曲")
    desired = [(item.device_name, item.size) for item in prepared]
    transport = Transport(port, baud, timeout, retries)
    try:
        _emit(progress, "list_start", port=port)
        before = _catalog_order(get_files(transport))
        capacity = preflight_library_capacity(before, desired)
        plan = calculate_sync_plan(before, desired, allow_empty=allow_empty)
        _emit(progress, "capacity", capacity=asdict(capacity))
        _emit(progress, "plan", plan=asdict(plan))
        for index, name in enumerate(plan.delete, 1):
            _emit(progress, "delete_start", index=index, total=len(plan.delete), name=name)
            _run_with_busy_retry(lambda n=name: delete_file(transport, n))
            _emit(progress, "delete_done", index=index, total=len(plan.delete), name=name)
        # Delete all files that are outside the target set first. Selected
        # filenames remain available until each atomic BEGIN/FINISH replacement
        # commits, so power loss cannot leave an individual selection partial.
        by_name = {item.device_name: item for item in prepared}
        for index, name in enumerate(plan.upload, 1):
            item = by_name[name]
            _emit(progress, "upload_start", index=index, total=len(plan.upload), name=name, size=item.size)
            _run_with_busy_retry(
                lambda item=item, name=name, index=index: upload_file(
                    transport,
                    Path(item.prepared_path),
                    name,
                    progress=lambda sent, total, n=name, i=index: _emit(
                        progress, "upload_progress", index=i, count=len(plan.upload), name=n, sent=sent, total=total
                    ),
                )
            )
            _emit(progress, "upload_done", index=index, total=len(plan.upload), name=name, size=item.size)
        after = _catalog_order(get_files(transport))
    finally:
        transport.close()

    expected_set = {(name.lower(), size) for name, size in desired}
    actual_set = {(name.lower(), size) for name, size in after}
    verified = expected_set == actual_set and len(after) == len(desired)
    if not verified:
        raise RuntimeError(f"最终校验失败：期望 {desired}，设备返回 {after}；请刷新设备后重试同步")
    _emit(progress, "verified", files=after)
    return {
        "port": port,
        "plan": asdict(plan),
        "capacity": asdict(capacity),
        "before": [{"name": name, "size": size} for name, size in before],
        "after": [{"name": name, "size": size} for name, size in after],
        "verified": True,
        "prepared": [asdict(item) for item in prepared],
    }


def _normalized_file_set(files: Iterable[tuple[str, int]]) -> set[tuple[str, int]]:
    return {(sanitize_filename(name).lower(), int(size)) for name, size in files}


def import_prepared(
    port: str,
    prepared: Sequence[PreparedSource],
    *,
    expected_current: Iterable[tuple[str, int]] | None = None,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
    progress: ProgressCallback | None = None,
) -> dict[str, object]:
    """Add new songs and atomically replace logical-name matches without deleting others."""
    if not prepared:
        raise ValueError("增量导入至少需要一首歌曲")
    if len(prepared) > MAX_LIBRARY_SONGS:
        raise ValueError(f"一次最多只能导入 {MAX_LIBRARY_SONGS} 首歌曲")
    incoming = [(item.device_name, item.size) for item in prepared]
    transport = Transport(port, baud, timeout, retries)
    try:
        _emit(progress, "list_start", port=port)
        before = _catalog_order(get_files(transport))
        if expected_current is not None and _normalized_file_set(before) != _normalized_file_set(expected_current):
            raise RuntimeError("设备音乐库在确认后发生变化；未写入任何文件，请刷新状态后重新确认")
        plan = calculate_import_plan(before, incoming)
        capacity = preflight_library_capacity(before, plan.final, upload_names=plan.upload)
        _emit(progress, "capacity", capacity=asdict(capacity))
        _emit(progress, "import_plan", plan=asdict(plan))
        for index, (item, assignment) in enumerate(zip(prepared, plan.assignments), 1):
            _emit(
                progress,
                "upload_start",
                index=index,
                total=len(plan.assignments),
                name=assignment.target_name,
                size=item.size,
            )
            _run_with_busy_retry(
                lambda item=item, assignment=assignment, index=index: upload_file(
                    transport,
                    Path(item.prepared_path),
                    assignment.target_name,
                    progress=lambda sent, total, n=assignment.target_name, i=index: _emit(
                        progress,
                        "upload_progress",
                        index=i,
                        count=len(plan.assignments),
                        name=n,
                        sent=sent,
                        total=total,
                    ),
                )
            )
            _emit(
                progress,
                "upload_done",
                index=index,
                total=len(plan.assignments),
                name=assignment.target_name,
                size=item.size,
            )
        after = _catalog_order(get_files(transport))
    finally:
        transport.close()

    if _normalized_file_set(after) != _normalized_file_set(plan.final) or len(after) != len(plan.final):
        raise RuntimeError(f"增量导入最终校验失败：期望 {plan.final}，设备返回 {after}")
    _emit(progress, "verified", files=after)
    return {
        "port": port,
        "mode": "import",
        "plan": asdict(plan),
        "capacity": asdict(capacity),
        "before": [{"name": name, "size": size} for name, size in before],
        "after": [{"name": name, "size": size} for name, size in after],
        "preserved": list(plan.preserved),
        "added": list(plan.added),
        "replaced": list(plan.replaced),
        "verified": True,
        "prepared": [asdict(item) for item in prepared],
    }


def reorder_prepared(
    port: str,
    prepared: Sequence[PreparedSource],
    *,
    expected_current: Iterable[tuple[str, int]] | None = None,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
    progress: ProgressCallback | None = None,
) -> dict[str, object]:
    """Rewrite a complete local source set under continuous device prefixes."""
    incoming = [(item.device_name, item.size) for item in prepared]
    transport = Transport(port, baud, timeout, retries)
    try:
        _emit(progress, "list_start", port=port)
        before = _catalog_order(get_files(transport))
        if expected_current is not None and _normalized_file_set(before) != _normalized_file_set(expected_current):
            raise RuntimeError("设备音乐库在确认后发生变化；未重排任何文件，请刷新状态后重新确认")
        plan = calculate_reorder_plan(before, incoming)
        capacity = preflight_library_capacity(before, plan.final)
        _emit(progress, "capacity", capacity=asdict(capacity))
        _emit(progress, "reorder_plan", plan=asdict(plan))

        for index, name in enumerate(plan.delete, 1):
            _emit(progress, "delete_start", index=index, total=len(plan.delete), name=name)
            _run_with_busy_retry(lambda n=name: delete_file(transport, n))
            _emit(progress, "delete_done", index=index, total=len(plan.delete), name=name)

        by_name = {item.device_name.casefold(): item for item in prepared}
        for index, assignment in enumerate(plan.assignments, 1):
            item = by_name[assignment.incoming_name.casefold()]
            _emit(
                progress,
                "upload_start",
                index=index,
                total=len(plan.assignments),
                name=assignment.target_name,
                size=item.size,
            )
            _run_with_busy_retry(
                lambda item=item, assignment=assignment, index=index: upload_file(
                    transport,
                    Path(item.prepared_path),
                    assignment.target_name,
                    progress=lambda sent, total, n=assignment.target_name, i=index: _emit(
                        progress,
                        "upload_progress",
                        index=i,
                        count=len(plan.assignments),
                        name=n,
                        sent=sent,
                        total=total,
                    ),
                )
            )
            _emit(
                progress,
                "upload_done",
                index=index,
                total=len(plan.assignments),
                name=assignment.target_name,
                size=item.size,
            )
        after = _catalog_order(get_files(transport))
    finally:
        transport.close()

    expected_order = [(name.casefold(), size) for name, size in plan.final]
    actual_order = [(sanitize_filename(name).casefold(), int(size)) for name, size in after]
    if actual_order != expected_order:
        raise RuntimeError(f"设备重排最终校验失败：期望 {plan.final}，设备返回 {after}")
    _emit(progress, "verified", files=after)
    return {
        "port": port,
        "mode": "reorder",
        "plan": asdict(plan),
        "capacity": asdict(capacity),
        "before": [{"name": name, "size": size} for name, size in before],
        "after": [{"name": name, "size": size} for name, size in after],
        "renamed": [{"from": old, "to": new} for old, new in plan.renamed],
        "verified": True,
        "prepared": [asdict(item) for item in prepared],
    }


def sync_sources(
    port: str,
    sources: Sequence[Path | str],
    *,
    work_dir: Path | str | None = None,
    allow_empty: bool = False,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
    progress: ProgressCallback | None = None,
) -> dict[str, object]:
    if not sources and not allow_empty:
        raise ValueError("未选择歌曲。若确实要清空文件音乐库，请加 --allow-empty")
    if work_dir is not None:
        prepared = prepare_sources(sources, work_dir, progress)
        return sync_prepared(
            port, prepared, allow_empty=allow_empty, baud=baud, timeout=timeout, retries=retries, progress=progress
        )
    with tempfile.TemporaryDirectory(prefix="mini-synth-library-") as temporary:
        prepared = prepare_sources(sources, temporary, progress)
        return sync_prepared(
            port, prepared, allow_empty=allow_empty, baud=baud, timeout=timeout, retries=retries, progress=progress
        )


def import_sources(
    port: str,
    sources: Sequence[Path | str],
    *,
    work_dir: Path | str | None = None,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
    progress: ProgressCallback | None = None,
) -> dict[str, object]:
    if not sources:
        raise ValueError("增量导入至少需要一首歌曲")
    if work_dir is not None:
        prepared = prepare_sources(sources, work_dir, progress)
        return import_prepared(
            port, prepared, baud=baud, timeout=timeout, retries=retries, progress=progress
        )
    with tempfile.TemporaryDirectory(prefix="mini-synth-import-") as temporary:
        prepared = prepare_sources(sources, temporary, progress)
        return import_prepared(
            port, prepared, baud=baud, timeout=timeout, retries=retries, progress=progress
        )


def reorder_sources(
    port: str,
    sources: Sequence[Path | str],
    *,
    work_dir: Path | str | None = None,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
    progress: ProgressCallback | None = None,
) -> dict[str, object]:
    if not sources:
        raise ValueError("重排需要设备全部歌曲对应的本地源文件")
    if work_dir is not None:
        prepared = prepare_sources(sources, work_dir, progress)
        return reorder_prepared(
            port, prepared, baud=baud, timeout=timeout, retries=retries, progress=progress
        )
    with tempfile.TemporaryDirectory(prefix="mini-synth-reorder-") as temporary:
        prepared = prepare_sources(sources, temporary, progress)
        return reorder_prepared(
            port, prepared, baud=baud, timeout=timeout, retries=retries, progress=progress
        )


def delete_device_files(
    port: str,
    names: Sequence[str],
    *,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
    expected_current: Iterable[tuple[str, int]] | None = None,
    progress: ProgressCallback | None = None,
) -> dict[str, object]:
    safe_names = [sanitize_filename(name) for name in names]
    if not safe_names:
        raise ValueError("至少选择一个要删除的设备文件")
    if len({name.lower() for name in safe_names}) != len(safe_names):
        raise ValueError("删除列表包含重复设备文件名")
    transport = Transport(port, baud, timeout, retries)
    try:
        before = _catalog_order(get_files(transport))
        if expected_current is not None and _normalized_file_set(before) != _normalized_file_set(expected_current):
            raise RuntimeError("设备音乐库在确认后发生变化；未删除任何文件，请刷新状态后重新确认")
        current_names = {name.lower() for name, _size in before}
        missing = [name for name in safe_names if name.lower() not in current_names]
        if missing:
            raise ValueError(f"设备中不存在待删除文件：{missing}")
        for index, name in enumerate(safe_names, 1):
            _emit(progress, "delete_start", index=index, total=len(safe_names), name=name)
            _run_with_busy_retry(lambda n=name: delete_file(transport, n))
            _emit(progress, "delete_done", index=index, total=len(safe_names), name=name)
        after = _catalog_order(get_files(transport))
    finally:
        transport.close()
    remaining_names = {name.lower() for name, _size in after}
    not_deleted = [name for name in safe_names if name.lower() in remaining_names]
    if not_deleted:
        raise RuntimeError(f"删除后校验失败，设备仍报告：{not_deleted}")
    _emit(progress, "verified", files=after)
    return {
        "port": port,
        "deleted": safe_names,
        "before": [{"name": name, "size": size} for name, size in before],
        "files": [{"name": name, "size": size} for name, size in after],
        "verified": True,
    }


def initialize_device_storage(
    port: str,
    confirmation: str,
    *,
    baud: int = 115200,
    timeout: float = 2.0,
    retries: int = 3,
) -> dict[str, object]:
    if confirmation != "ERASE-MUSIC-LIBRARY":
        raise ValueError("必须原样输入 ERASE-MUSIC-LIBRARY")
    transport = Transport(port, baud, timeout, retries)
    try:
        initialize_storage(transport, confirmation)
        after = _catalog_order(get_files(transport))
    finally:
        transport.close()
    if after:
        raise RuntimeError(f"初始化后设备仍报告文件：{after}")
    return {"port": port, "initialized": True, "files": []}


def _add_transport_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--baud", type=int, default=115200, help=argparse.SUPPRESS)
    parser.add_argument("--timeout", type=float, default=2.0, help=argparse.SUPPRESS)
    parser.add_argument("--retries", type=int, default=3, help=argparse.SUPPRESS)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=f"Mini Synth 音乐库管理工具（最多 {MAX_LIBRARY_SONGS} 首文件歌曲）")
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("ports", help="列出可用串口")

    show = subcommands.add_parser("show", help="显示设备状态和音乐文件")
    show.add_argument("--port", required=True, help="运行时指定的串口，例如 COM6")
    _add_transport_options(show)

    prepare = subcommands.add_parser("prepare", help="转换或验证歌曲源文件")
    prepare.add_argument("source", nargs="+", type=Path, help=f"1..{MAX_LIBRARY_SONGS} 个 MSPKG/MusicXML 源文件")
    prepare.add_argument("--output", required=True, type=Path)

    sync = subcommands.add_parser("sync", help="使设备音乐库与所选文件完全一致")
    sync.add_argument("--port", required=True, help="运行时指定的串口，例如 COM6")
    sync.add_argument("source", nargs="*", type=Path, help=f"0..{MAX_LIBRARY_SONGS} 个 MSPKG/MusicXML 源文件")
    sync.add_argument("--work-dir", type=Path)
    sync.add_argument("--allow-empty", action="store_true", help="允许删除全部文件歌曲")
    _add_transport_options(sync)

    import_command = subcommands.add_parser("import", help="保留设备现有歌曲，新增或覆盖同名歌曲")
    import_command.add_argument("--port", required=True, help="运行时指定的串口，例如 COM6")
    import_command.add_argument("source", nargs="+", type=Path, help=f"1..{MAX_LIBRARY_SONGS} 个 MSPKG/MusicXML 源文件")
    import_command.add_argument("--work-dir", type=Path)
    _add_transport_options(import_command)

    reorder = subcommands.add_parser("reorder", help="使用完整本地源列表按指定顺序重排设备歌曲")
    reorder.add_argument("--port", required=True, help="运行时指定的串口，例如 COM6")
    reorder.add_argument("source", nargs="+", type=Path, help=f"设备全部歌曲的 {MAX_LIBRARY_SONGS} 个以内有序源文件")
    reorder.add_argument("--work-dir", type=Path)
    _add_transport_options(reorder)

    delete = subcommands.add_parser("delete", help="删除指定设备文件")
    delete.add_argument("--port", required=True)
    delete.add_argument("device_name", nargs="+")
    _add_transport_options(delete)

    init_storage = subcommands.add_parser("init-storage", help="破坏性初始化/修复音乐存储")
    init_storage.add_argument("--port", required=True)
    init_storage.add_argument("--confirm", required=True)
    _add_transport_options(init_storage)
    return parser


def _console_progress(event: str, details: dict[str, object]) -> None:
    if event == "prepare_start":
        print(f"准备 {details['index']}/{details['total']}：{Path(str(details['source'])).name}")
    elif event == "prepare_done":
        item = details["item"]
        assert isinstance(item, dict)
        print(f"  已就绪：{item['device_name']}（{item['size']} 字节）")
    elif event == "list_start":
        print(f"读取设备音乐库：{details['port']}")
    elif event == "capacity":
        raw = details["capacity"]
        assert isinstance(raw, dict)
        print(
            f"容量预检：目标 {int(raw['desired_bytes']):,} 字节，"
            f"估算峰值 {int(raw['estimated_peak_bytes']):,} / {int(raw['safe_library_bytes']):,} 字节"
        )
    elif event == "plan":
        raw = details["plan"]
        assert isinstance(raw, dict)
        print(f"计划：删除 {len(raw['delete'])}，写入 {len(raw['upload'])}，最终 {len(raw['desired'])} 首")
    elif event == "import_plan":
        raw = details["plan"]
        assert isinstance(raw, dict)
        print(
            f"增量导入：保留 {len(raw['preserved'])}，新增 {len(raw['added'])}，"
            f"覆盖 {len(raw['replaced'])}，最终 {len(raw['final'])} 首"
        )
    elif event == "reorder_plan":
        raw = details["plan"]
        assert isinstance(raw, dict)
        print(f"设备重排：改名 {len(raw['renamed'])}，重写 {len(raw['upload'])}，最终 {len(raw['final'])} 首")
    elif event == "delete_start":
        print(f"删除 {details['name']} ...")
    elif event == "upload_start":
        print(f"写入 {details['index']}/{details['total']}：{details['name']}")
    elif event == "verified":
        print("最终文件名与尺寸校验通过")


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "ports":
            ports = list_ports_data()
            for item in ports:
                print(f"{item['device']}\t{item['description']}\t{item['hwid']}")
            payload: dict[str, object] = {"ok": True, "command": "ports", "ports": ports}
        elif args.command == "show":
            device = show_device(args.port, baud=args.baud, timeout=args.timeout, retries=args.retries)
            status = device["status"]
            assert isinstance(status, dict)
            print(f"设备 {args.port}：{status['state_name']}，存储 {'可用' if status['mounted'] else '不可用'}")
            for item in device["files"]:  # type: ignore[union-attr]
                print(f"{item['size']:8d}  {item['name']}")
            payload = {"ok": True, "command": "show", **device}
        elif args.command == "prepare":
            prepared = prepare_sources(args.source, args.output, _console_progress)
            payload = {
                "ok": True,
                "command": "prepare",
                "output": str(args.output.resolve()),
                "count": len(prepared),
                "items": [asdict(item) for item in prepared],
            }
        elif args.command == "sync":
            report = sync_sources(
                args.port,
                args.source,
                work_dir=args.work_dir,
                allow_empty=args.allow_empty,
                baud=args.baud,
                timeout=args.timeout,
                retries=args.retries,
                progress=_console_progress,
            )
            payload = {"ok": True, "command": "sync", **report}
        elif args.command == "import":
            report = import_sources(
                args.port,
                args.source,
                work_dir=args.work_dir,
                baud=args.baud,
                timeout=args.timeout,
                retries=args.retries,
                progress=_console_progress,
            )
            payload = {"ok": True, "command": "import", **report}
        elif args.command == "reorder":
            report = reorder_sources(
                args.port,
                args.source,
                work_dir=args.work_dir,
                baud=args.baud,
                timeout=args.timeout,
                retries=args.retries,
                progress=_console_progress,
            )
            payload = {"ok": True, "command": "reorder", **report}
        elif args.command == "delete":
            report = delete_device_files(
                args.port,
                args.device_name,
                baud=args.baud,
                timeout=args.timeout,
                retries=args.retries,
                progress=_console_progress,
            )
            print(f"已删除 {len(report['deleted'])} 个文件")
            payload = {"ok": True, "command": "delete", **report}
        elif args.command == "init-storage":
            report = initialize_device_storage(
                args.port, args.confirm, baud=args.baud, timeout=args.timeout, retries=args.retries
            )
            print("音乐存储初始化完成；全部文件歌曲已删除")
            payload = {"ok": True, "command": "init-storage", **report}
        else:  # pragma: no cover
            raise AssertionError(args.command)
        result_json(payload)
        return 0
    except Exception as exc:
        print(f"错误：{exc}", file=sys.stderr)
        result_json({"ok": False, "command": args.command, "error": str(exc), "error_type": type(exc).__name__})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
