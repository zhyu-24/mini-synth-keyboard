from __future__ import annotations

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import music_library as ml  # noqa: E402


def tlv(tag: int, kind: int, value: bytes) -> bytes:
    return struct.pack("<HBBI", tag, kind, 0, len(value)) + value


def make_mspkg(note_count: int = 2) -> bytes:
    notes = b"".join(
        struct.pack("<IIBBBB", index * 480, 240, 60 + index, 100, 0, 0)
        for index in range(note_count)
    )
    duration = (note_count - 1) * 480 + 240
    payload = (
        struct.pack("<4sHHIIIHHI", b"MSQ1", 480, 12, note_count, 1, duration, 1, 0, 0)
        + struct.pack("<II", 0, 500_000)
        + struct.pack("<IBBBB", 0, 4, 2, 24, 8)
        + notes
    )
    metadata = b"".join(
        [
            tlv(1, 1, "合成测试".encode("utf-8")),
            tlv(17, 2, struct.pack("<I", note_count)),
            tlv(18, 2, struct.pack("<I", duration)),
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
        zlib.crc32(metadata) & 0xFFFFFFFF,
        zlib.crc32(payload) & 0xFFFFFFFF,
        1,
    )
    return header + metadata + payload


def sized_library(
    total_bytes: int,
    start_position: int = 1,
    max_entry_bytes: int = ml.MAX_FILE_BYTES,
) -> list[tuple[str, int]]:
    entries: list[tuple[str, int]] = []
    remaining = total_bytes
    position = start_position
    while remaining:
        size = min(remaining, max_entry_bytes)
        if remaining - size and remaining - size < 32:
            size -= 32 - (remaining - size)
        entries.append((f"{position:02d}-capacity.mspkg", size))
        remaining -= size
        position += 1
    return entries


class SafeNameTests(unittest.TestCase):
    def test_ascii_is_deterministic_safe_and_ordered(self) -> None:
        first = ml.safe_device_name("My Song.musicxml", 1)
        second = ml.safe_device_name("My Song.musicxml", 1)
        self.assertEqual(first, second)
        self.assertEqual(first, "01-my-song.mspkg")
        self.assertLessEqual(len(first.encode("ascii")), 48)

    def test_chinese_and_unicode_stems_use_stable_hash(self) -> None:
        chinese = ml.safe_device_name("小星星.musicxml", 2)
        accent = ml.safe_device_name("Café Étude.xml", 3)
        self.assertRegex(chinese, r"^02-song-[0-9a-f]{8}\.mspkg$")
        self.assertRegex(accent, r"^03-cafe-etude-[0-9a-f]{8}\.mspkg$")
        self.assertEqual(chinese, ml.safe_device_name("小星星.musicxml", 2))

    def test_collision_suffix_and_extension_rules(self) -> None:
        original = ml.safe_device_name("Demo.mspkg", 1)
        collided = ml.safe_device_name("Demo.mspkg", 1, [original])
        self.assertEqual(original, "01-demo.mspkg")
        self.assertEqual(collided, "01-demo-2.mspkg")
        self.assertEqual(ml.safe_device_name("SHORT.MSP", 4), "04-short.msp")
        self.assertEqual(ml.safe_device_name("score.xml", 4), "04-score.mspkg")
        with self.assertRaises(ValueError):
            ml.safe_device_name("score.xml", 1, output_extension=".bin")

    def test_two_digit_prefixes_cover_positions_1_9_10_30(self) -> None:
        expected = {1: "01", 9: "09", 10: "10", 30: "30"}
        for position, prefix in expected.items():
            with self.subTest(position=position):
                self.assertEqual(ml.safe_device_name("Song.musicxml", position), f"{prefix}-song.mspkg")

    def test_existing_device_order_prefix_is_not_duplicated(self) -> None:
        self.assertEqual(ml.safe_device_name("04-glory.msp", 1), "01-glory.msp")
        with self.assertRaisesRegex(ValueError, "1..30"):
            ml.safe_device_name("Song.musicxml", 31)

    def test_generated_names_accept_30_and_reject_31(self) -> None:
        sources = [f"song-{index}.xml" for index in range(1, 31)]
        names = ml.generate_safe_device_names(sources)
        self.assertEqual(len(names), 30)
        self.assertTrue(names[0].startswith("01-"))
        self.assertTrue(names[8].startswith("09-"))
        self.assertTrue(names[9].startswith("10-"))
        self.assertTrue(names[29].startswith("30-"))
        with self.assertRaisesRegex(ValueError, "at most 30"):
            ml.generate_safe_device_names(sources + ["song-31.xml"])


class PackageValidationTests(unittest.TestCase):
    def test_synthetic_fixture_validates_and_prepares(self) -> None:
        data = make_mspkg(3)
        info = ml.validate_package_bytes(data)
        self.assertEqual(info.note_count, 3)
        self.assertEqual(info.content_type, "SEQUENCE")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "测试包.mspkg"
            source.write_bytes(data)
            prepared = ml.prepare_sources([source], root / "out")
            self.assertEqual(len(prepared), 1)
            item = prepared[0]
            self.assertEqual(item.size, len(data))
            self.assertTrue(item.device_name.endswith(".mspkg"))
            self.assertEqual(Path(item.prepared_path).read_bytes(), data)
            self.assertFalse(item.converted)

    def test_package_rejects_bad_crc_size_and_extension(self) -> None:
        corrupted = bytearray(make_mspkg())
        corrupted[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "payload CRC"):
            ml.validate_package_bytes(bytes(corrupted))
        with self.assertRaises(ValueError):
            ml.validate_package_bytes(b"MSPK")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "wrong.bin"
            path.write_bytes(make_mspkg())
            with self.assertRaisesRegex(ValueError, "扩展名"):
                ml.validate_package(path)

    def test_prepare_max_30_and_metadata_builder(self) -> None:
        with self.assertRaisesRegex(ValueError, "最多"):
            ml.prepare_sources([Path(f"{index}.mspkg") for index in range(31)], Path("unused"))
        info = ml.validate_package_bytes(make_mspkg())
        metadata = ml.source_preparation_metadata("a.mspkg", "b.mspkg", "01-a.mspkg", info, False)
        self.assertEqual(metadata.device_name, "01-a.mspkg")
        self.assertEqual(metadata.package["note_count"], 2)

    def test_prepare_cli_emits_final_result_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "one.mspkg"
            package.write_bytes(make_mspkg())
            command = [sys.executable, str(ROOT / "music_library.py"), "prepare", str(package), "--output", str(root / "out")]
            # Force the child and parent to agree even when the Windows console,
            # Python UTF-8 mode, and the active system code page differ.
            environment = os.environ.copy()
            environment["PYTHONIOENCODING"] = "utf-8"
            completed = subprocess.run(
                command,
                text=True,
                encoding="utf-8",
                errors="strict",
                capture_output=True,
                check=False,
                env=environment,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            last = completed.stdout.strip().splitlines()[-1]
            self.assertTrue(last.startswith("RESULT_JSON="))
            payload = json.loads(last.removeprefix("RESULT_JSON="))
            self.assertTrue(payload["ok"])
            self.assertEqual(payload["count"], 1)


class SyncPlanTests(unittest.TestCase):
    def test_add_replace_delete_and_exact(self) -> None:
        current = [("01-old.mspkg", 10), ("02-keep.mspkg", 20), ("03-change.mspkg", 30)]
        desired = [("02-keep.mspkg", 20), ("03-change.mspkg", 31), ("04-new.mspkg", 40)]
        plan = ml.calculate_sync_plan(current, desired)
        self.assertEqual(plan.delete, ("01-old.mspkg",))
        self.assertEqual(plan.add, ("04-new.mspkg",))
        self.assertEqual(plan.replace, ("03-change.mspkg",))
        self.assertEqual(plan.exact, ("02-keep.mspkg",))
        self.assertEqual(plan.upload, tuple(name for name, _size in desired))

    def test_exact_match_is_rewritten_to_guarantee_content(self) -> None:
        current = [("01-a.mspkg", 100), ("02-b.msp", 200)]
        plan = ml.calculate_sync_plan(current, current)
        self.assertFalse(plan.delete)
        self.assertFalse(plan.add)
        self.assertFalse(plan.replace)
        self.assertEqual(plan.exact, ("01-a.mspkg", "02-b.msp"))
        self.assertEqual(plan.upload, ("01-a.mspkg", "02-b.msp"))

    def test_empty_is_guarded_and_allowed_explicitly(self) -> None:
        current = [("01-a.mspkg", 100)]
        with self.assertRaisesRegex(ValueError, "空音乐库"):
            ml.calculate_sync_plan(current, [])
        plan = ml.calculate_sync_plan(current, [], allow_empty=True)
        self.assertEqual(plan.delete, ("01-a.mspkg",))
        self.assertEqual(plan.upload, ())

    def test_desired_accepts_30_rejects_31_and_guards_duplicates(self) -> None:
        desired = [(f"{index:02d}-x.mspkg", index) for index in range(1, 31)]
        plan = ml.calculate_sync_plan([], desired)
        self.assertEqual(len(plan.desired), 30)
        with self.assertRaisesRegex(ValueError, "最多.*30"):
            ml.calculate_sync_plan([], desired + [("31-x.mspkg", 31)])
        with self.assertRaisesRegex(ValueError, "重复"):
            ml.calculate_sync_plan([], [("01-a.mspkg", 1), ("01-A.MSPKG", 2)])


class ImportPlanTests(unittest.TestCase):
    def test_empty_device_adds_in_input_order(self) -> None:
        plan = ml.calculate_import_plan([], [("01-alpha.msp", 100), ("02-beta.msp", 120)])
        self.assertEqual(plan.preserved, ())
        self.assertEqual(plan.added, ("01-alpha.msp", "02-beta.msp"))
        self.assertEqual(plan.replaced, ())
        self.assertEqual(plan.final, (("01-alpha.msp", 100), ("02-beta.msp", 120)))

    def test_new_song_appends_and_preserves_existing(self) -> None:
        current = [(f"{index:02d}-song-{index}.msp", 100 + index) for index in range(1, 5)]
        plan = ml.calculate_import_plan(current, [("01-new-song.msp", 200)])
        self.assertEqual(plan.added, ("05-new-song.msp",))
        self.assertEqual(plan.preserved, tuple(name for name, _size in current))
        self.assertFalse(plan.replaced)
        self.assertTrue(all(name in dict(plan.final) for name, _size in current))

    def test_new_songs_fill_lowest_numbering_gaps_first(self) -> None:
        current = [("02-reverse.msp", 100), ("03-spring.msp", 110)]
        plan = ml.calculate_import_plan(
            current,
            [("01-glory.msp", 120), ("02-voyager.msp", 130)],
        )
        self.assertEqual(plan.added, ("01-glory.msp", "04-voyager.msp"))

    def test_same_logical_name_ignores_prefix_extension_and_case(self) -> None:
        current = [("05-Glory.MSP", 100), ("06-keep.msp", 110)]
        plan = ml.calculate_import_plan(current, [("01-glory.mspkg", 140)])
        self.assertEqual(plan.replaced, ("05-Glory.MSP",))
        self.assertEqual(plan.preserved, ("06-keep.msp",))
        self.assertEqual(plan.added, ())
        self.assertEqual(plan.assignments[0].target_name, "05-Glory.MSP")
        self.assertEqual(dict(plan.final)["05-Glory.MSP"], 140)

    def test_same_size_replacement_is_still_uploaded(self) -> None:
        plan = ml.calculate_import_plan([("03-demo.msp", 100)], [("01-DEMO.mspkg", 100)])
        self.assertEqual(plan.replaced, ("03-demo.msp",))
        self.assertEqual(plan.upload, ("03-demo.msp",))

    def test_highest_30_fills_smallest_numbering_gaps(self) -> None:
        current = [("01-first.msp", 100), ("30-last.msp", 100)]
        plan = ml.calculate_import_plan(current, [("01-new-a.msp", 100), ("02-new-b.msp", 100)])
        self.assertEqual(plan.added, ("02-new-a.msp", "03-new-b.msp"))

    def test_ambiguous_names_and_limit_fail_before_writes(self) -> None:
        with self.assertRaisesRegex(ValueError, "设备曲库存在同名歧义"):
            ml.calculate_import_plan(
                [("01-demo.msp", 100), ("02-DEMO.mspkg", 110)],
                [("01-new.msp", 120)],
            )
        with self.assertRaisesRegex(ValueError, "待导入列表存在同名歧义"):
            ml.calculate_import_plan(
                [],
                [("01-demo.msp", 100), ("02-DEMO.mspkg", 110)],
            )
        full = [(f"{index:02d}-song-{index}.msp", 100) for index in range(1, 31)]
        with self.assertRaisesRegex(ValueError, "超过设备上限"):
            ml.calculate_import_plan(full, [("01-new.msp", 120)])


class ReorderPlanTests(unittest.TestCase):
    def test_reorders_complete_local_set_to_continuous_prefixes(self) -> None:
        current = [
            ("02-reverse.msp", 100),
            ("03-spring.msp", 110),
            ("04-glory.msp", 120),
            ("05-voyager.msp", 130),
        ]
        incoming = [
            ("01-glory.msp", 120),
            ("02-reverse.msp", 100),
            ("03-spring.msp", 110),
            ("04-voyager.msp", 130),
        ]
        plan = ml.calculate_reorder_plan(current, incoming)
        self.assertEqual(
            plan.final,
            (
                ("01-glory.msp", 120),
                ("02-reverse.msp", 100),
                ("03-spring.msp", 110),
                ("04-voyager.msp", 130),
            ),
        )
        self.assertEqual(plan.renamed, (("04-glory.msp", "01-glory.msp"), ("05-voyager.msp", "04-voyager.msp")))

    def test_arbitrary_swap_and_unnumbered_file(self) -> None:
        current = [("alpha.msp", 100), ("02-beta.msp", 110)]
        plan = ml.calculate_reorder_plan(current, [("01-beta.msp", 110), ("02-alpha.msp", 100)])
        self.assertEqual(plan.final, (("01-beta.msp", 110), ("02-alpha.msp", 100)))

    def test_missing_extra_duplicate_and_size_mismatch_are_rejected(self) -> None:
        current = [("01-alpha.msp", 100), ("02-beta.msp", 110)]
        with self.assertRaisesRegex(ValueError, "一一对应"):
            ml.calculate_reorder_plan(current, [("01-alpha.msp", 100)])
        with self.assertRaisesRegex(ValueError, "逻辑歌曲不一致"):
            ml.calculate_reorder_plan(current, [("01-alpha.msp", 100), ("02-gamma.msp", 110)])
        with self.assertRaisesRegex(ValueError, "同名歧义"):
            ml.calculate_reorder_plan(current, [("01-alpha.msp", 100), ("02-ALPHA.mspkg", 110)])
        with self.assertRaisesRegex(ValueError, "大小不一致"):
            ml.calculate_reorder_plan(current, [("01-alpha.msp", 101), ("02-beta.msp", 110)])


class BusyRetryTests(unittest.TestCase):
    @staticmethod
    def busy_error() -> ml.ImportProtocolError:
        return ml.ImportProtocolError(
            "device error BUSY; state=IDLE; next_offset=0",
            code=int(ml.ErrorCode.BUSY),
            state=0,
            next_offset=0,
        )

    def test_sync_retries_busy_across_consecutive_deletes_and_uploads(self) -> None:
        current = [("01-old.msp", 100), ("02-old.msp", 120)]
        prepared = [
            ml.PreparedSource("a.msp", "msp", "a.msp", "01-new.msp", 130, "a", False, {}),
            ml.PreparedSource("b.msp", "msp", "b.msp", "02-new.msp", 140, "b", False, {}),
        ]
        desired = [(item.device_name, item.size) for item in prepared]
        delete_attempts: dict[str, int] = {}
        upload_attempts: dict[str, int] = {}

        def flaky_delete(_transport: object, name: str) -> None:
            delete_attempts[name] = delete_attempts.get(name, 0) + 1
            if delete_attempts[name] == 1:
                raise self.busy_error()

        def flaky_upload(
            _transport: object,
            _path: Path,
            name: str,
            progress: object = None,
        ) -> None:
            del progress
            upload_attempts[name] = upload_attempts.get(name, 0) + 1
            if upload_attempts[name] == 1:
                raise self.busy_error()

        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", side_effect=[current, desired]),
            patch.object(ml, "delete_file", side_effect=flaky_delete),
            patch.object(ml, "upload_file", side_effect=flaky_upload),
            patch.object(ml.time, "sleep") as sleep,
        ):
            report = ml.sync_prepared("COM12", prepared)

        self.assertEqual(report["after"], [{"name": name, "size": size} for name, size in desired])
        self.assertEqual(delete_attempts, {"01-old.msp": 2, "02-old.msp": 2})
        self.assertEqual(upload_attempts, {"01-new.msp": 2, "02-new.msp": 2})
        self.assertEqual(sleep.call_count, 4)
        transport.close.assert_called_once_with()

    def test_non_busy_protocol_error_is_not_retried(self) -> None:
        operation = Mock(
            side_effect=ml.ImportProtocolError(
                "device error BAD_PAYLOAD",
                code=int(ml.ErrorCode.BAD_PAYLOAD),
            )
        )
        with self.assertRaises(ml.ImportProtocolError):
            ml._run_with_busy_retry(operation)
        operation.assert_called_once_with()

    def test_import_retries_busy_and_never_deletes_unrelated_files(self) -> None:
        current = [("01-keep.msp", 100), ("02-replace.msp", 120)]
        prepared = [
            ml.PreparedSource("replace.msp", "msp", "replace.msp", "01-replace.msp", 130, "a", False, {}),
            ml.PreparedSource("new.msp", "msp", "new.msp", "02-new.msp", 140, "b", False, {}),
        ]
        final = [("01-keep.msp", 100), ("02-replace.msp", 130), ("03-new.msp", 140)]
        attempts: dict[str, int] = {}

        def flaky_upload(_transport: object, _path: Path, name: str, progress: object = None) -> None:
            del progress
            attempts[name] = attempts.get(name, 0) + 1
            if attempts[name] == 1:
                raise self.busy_error()

        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", side_effect=[current, final]),
            patch.object(ml, "upload_file", side_effect=flaky_upload),
            patch.object(ml, "delete_file") as delete,
            patch.object(ml.time, "sleep"),
        ):
            report = ml.import_prepared("COM12", prepared, expected_current=current)

        self.assertTrue(report["verified"])
        self.assertEqual(report["preserved"], ["01-keep.msp"])
        self.assertEqual(report["replaced"], ["02-replace.msp"])
        self.assertEqual(report["added"], ["03-new.msp"])
        self.assertEqual(attempts, {"02-replace.msp": 2, "03-new.msp": 2})
        delete.assert_not_called()

    def test_multi_delete_retries_busy_and_verifies_absence(self) -> None:
        current = [("01-a.msp", 100), ("02-b.msp", 110), ("03-c.msp", 120)]
        after = [("03-c.msp", 120)]
        attempts: dict[str, int] = {}

        def flaky_delete(_transport: object, name: str) -> None:
            attempts[name] = attempts.get(name, 0) + 1
            if attempts[name] == 1:
                raise self.busy_error()

        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", side_effect=[current, after]),
            patch.object(ml, "delete_file", side_effect=flaky_delete),
            patch.object(ml.time, "sleep"),
        ):
            report = ml.delete_device_files(
                "COM12",
                ["01-a.msp", "02-b.msp"],
                expected_current=current,
            )

        self.assertTrue(report["verified"])
        self.assertEqual(report["files"], [{"name": "03-c.msp", "size": 120}])
        self.assertEqual(attempts, {"01-a.msp": 2, "02-b.msp": 2})

    def test_reorder_retries_busy_and_verifies_exact_order(self) -> None:
        current = [("02-beta.msp", 110), ("04-alpha.msp", 100)]
        final = [("01-alpha.msp", 100), ("02-beta.msp", 110)]
        prepared = [
            ml.PreparedSource("alpha.msp", "msp", "alpha.msp", "01-alpha.msp", 100, "a", False, {}),
            ml.PreparedSource("beta.msp", "msp", "beta.msp", "02-beta.msp", 110, "b", False, {}),
        ]
        delete_attempts: dict[str, int] = {}
        upload_attempts: dict[str, int] = {}

        def flaky_delete(_transport: object, name: str) -> None:
            delete_attempts[name] = delete_attempts.get(name, 0) + 1
            if delete_attempts[name] == 1:
                raise self.busy_error()

        def flaky_upload(_transport: object, _path: Path, name: str, progress: object = None) -> None:
            del progress
            upload_attempts[name] = upload_attempts.get(name, 0) + 1
            if upload_attempts[name] == 1:
                raise self.busy_error()

        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", side_effect=[current, final]),
            patch.object(ml, "delete_file", side_effect=flaky_delete),
            patch.object(ml, "upload_file", side_effect=flaky_upload),
            patch.object(ml.time, "sleep"),
        ):
            report = ml.reorder_prepared("COM12", prepared, expected_current=current)

        self.assertTrue(report["verified"])
        self.assertEqual(report["mode"], "reorder")
        self.assertEqual(report["renamed"], [{"from": "04-alpha.msp", "to": "01-alpha.msp"}])
        self.assertEqual(delete_attempts, {"04-alpha.msp": 2})
        self.assertEqual(upload_attempts, {"01-alpha.msp": 2, "02-beta.msp": 2})

    def test_reorder_stale_snapshot_stops_before_delete_or_upload(self) -> None:
        expected = [("01-alpha.msp", 100)]
        changed = [("01-alpha.msp", 100), ("02-beta.msp", 110)]
        prepared = [
            ml.PreparedSource("alpha.msp", "msp", "alpha.msp", "01-alpha.msp", 100, "a", False, {})
        ]
        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", return_value=changed),
            patch.object(ml, "delete_file") as delete,
            patch.object(ml, "upload_file") as upload,
            self.assertRaisesRegex(RuntimeError, "确认后发生变化"),
        ):
            ml.reorder_prepared("COM12", prepared, expected_current=expected)
        delete.assert_not_called()
        upload.assert_not_called()

    def test_reorder_capacity_failure_happens_before_delete_or_upload(self) -> None:
        raw = sized_library(ml.FFAT_SAFE_LIBRARY_BYTES)
        current = [(f"{index:02d}-capacity-{index}.mspkg", size) for index, (_name, size) in enumerate(raw, 1)]
        prepared = [
            ml.PreparedSource(name, "msp", name, name, size, str(index), False, {})
            for index, (name, size) in enumerate(current, 1)
        ]
        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", return_value=current),
            patch.object(ml, "delete_file") as delete,
            patch.object(ml, "upload_file") as upload,
            self.assertRaisesRegex(ValueError, "容量预检失败"),
        ):
            ml.reorder_prepared("COM12", prepared)
        delete.assert_not_called()
        upload.assert_not_called()

    def test_reorder_final_order_mismatch_is_reported(self) -> None:
        current = [("02-beta.msp", 110), ("04-alpha.msp", 100)]
        prepared = [
            ml.PreparedSource("alpha.msp", "msp", "alpha.msp", "01-alpha.msp", 100, "a", False, {}),
            ml.PreparedSource("beta.msp", "msp", "beta.msp", "02-beta.msp", 110, "b", False, {}),
        ]
        wrong_after = [("01-alpha.msp", 100), ("03-beta.msp", 110)]
        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", side_effect=[current, wrong_after]),
            patch.object(ml, "delete_file"),
            patch.object(ml, "upload_file"),
            self.assertRaisesRegex(RuntimeError, "最终校验失败"),
        ):
            ml.reorder_prepared("COM12", prepared)

    def test_import_capacity_failure_happens_before_upload(self) -> None:
        raw_current = sized_library(ml.FFAT_SAFE_LIBRARY_BYTES - 100)
        current = [
            (f"{index:02d}-capacity-{index}.mspkg", size)
            for index, (_name, size) in enumerate(raw_current, 1)
        ]
        prepared = [
            ml.PreparedSource("new.msp", "msp", "new.msp", "01-new.msp", 200, "a", False, {})
        ]
        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", return_value=current),
            patch.object(ml, "upload_file") as upload,
            self.assertRaisesRegex(ValueError, "容量预检失败"),
        ):
            ml.import_prepared("COM12", prepared)
        upload.assert_not_called()
        transport.close.assert_called_once_with()

    def test_import_stale_preview_happens_before_upload(self) -> None:
        expected = [("01-old.msp", 100)]
        changed = [("01-old.msp", 100), ("02-external.msp", 110)]
        prepared = [
            ml.PreparedSource("new.msp", "msp", "new.msp", "01-new.msp", 120, "a", False, {})
        ]
        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", return_value=changed),
            patch.object(ml, "upload_file") as upload,
            self.assertRaisesRegex(RuntimeError, "确认后发生变化"),
        ):
            ml.import_prepared("COM12", prepared, expected_current=expected)
        upload.assert_not_called()

    def test_import_final_mismatch_is_reported(self) -> None:
        current = [("01-keep.msp", 100)]
        prepared = [
            ml.PreparedSource("new.msp", "msp", "new.msp", "01-new.msp", 120, "a", False, {})
        ]
        wrong_after = [("01-keep.msp", 100)]
        transport = Mock()
        with (
            patch.object(ml, "Transport", return_value=transport),
            patch.object(ml, "get_files", side_effect=[current, wrong_after]),
            patch.object(ml, "upload_file"),
            self.assertRaisesRegex(RuntimeError, "增量导入最终校验失败"),
        ):
            ml.import_prepared("COM12", prepared)


class CliTests(unittest.TestCase):
    def test_import_result_json_contains_mode_and_verified(self) -> None:
        report = {
            "port": "COM12",
            "mode": "import",
            "preserved": ["01-old.msp"],
            "added": ["02-new.msp"],
            "replaced": [],
            "after": [{"name": "01-old.msp", "size": 100}, {"name": "02-new.msp", "size": 120}],
            "verified": True,
        }
        output = io.StringIO()
        with patch.object(ml, "import_sources", return_value=report), redirect_stdout(output):
            exit_code = ml.main(["import", "--port", "COM12", "new.msp"])
        self.assertEqual(exit_code, 0)
        result_line = output.getvalue().strip().splitlines()[-1]
        payload = json.loads(result_line.removeprefix("RESULT_JSON="))
        self.assertEqual(payload["command"], "import")
        self.assertEqual(payload["mode"], "import")
        self.assertTrue(payload["verified"])

    def test_reorder_result_json_contains_mode_renames_and_verified(self) -> None:
        report = {
            "port": "COM12",
            "mode": "reorder",
            "before": [{"name": "04-alpha.msp", "size": 100}],
            "after": [{"name": "01-alpha.msp", "size": 100}],
            "renamed": [{"from": "04-alpha.msp", "to": "01-alpha.msp"}],
            "verified": True,
        }
        output = io.StringIO()
        with patch.object(ml, "reorder_sources", return_value=report), redirect_stdout(output):
            exit_code = ml.main(["reorder", "--port", "COM12", "alpha.msp"])
        self.assertEqual(exit_code, 0)
        payload = json.loads(output.getvalue().strip().splitlines()[-1].removeprefix("RESULT_JSON="))
        self.assertEqual(payload["command"], "reorder")
        self.assertEqual(payload["mode"], "reorder")
        self.assertTrue(payload["verified"])


class CapacityPreflightTests(unittest.TestCase):
    def test_named_partition_and_reserve_constants(self) -> None:
        self.assertEqual(ml.FFAT_PARTITION_CAPACITY_BYTES, 0x9E0000)
        minimum_reserve = (ml.FFAT_PARTITION_CAPACITY_BYTES * 5 + 99) // 100 + 128 * 1024
        self.assertGreaterEqual(ml.FFAT_RESERVED_BYTES, minimum_reserve)
        self.assertEqual(
            ml.FFAT_SAFE_LIBRARY_BYTES,
            ml.FFAT_PARTITION_CAPACITY_BYTES - ml.FFAT_RESERVED_BYTES,
        )

    def test_final_library_capacity_boundary_is_accepted_and_rejected(self) -> None:
        accepted_desired = sized_library(ml.FFAT_SAFE_LIBRARY_BYTES)
        accepted = ml.preflight_library_capacity([], accepted_desired)
        self.assertEqual(accepted.desired_bytes, ml.FFAT_SAFE_LIBRARY_BYTES)
        self.assertEqual(accepted.estimated_peak_bytes, ml.FFAT_SAFE_LIBRARY_BYTES)
        with self.assertRaisesRegex(ValueError, "容量预检失败.*设备尚未被修改"):
            ml.preflight_library_capacity([], sized_library(ml.FFAT_SAFE_LIBRARY_BYTES + 1))

    def test_atomic_replacement_peak_boundary_is_accepted_and_rejected(self) -> None:
        replacement_size = 500_000
        final_total = ml.FFAT_SAFE_LIBRARY_BYTES - replacement_size * 2
        filler = sized_library(final_total - replacement_size, start_position=2, max_entry_bytes=replacement_size)
        accepted_library = [("01-replace.mspkg", replacement_size), *filler]
        accepted = ml.preflight_library_capacity(accepted_library, accepted_library)
        self.assertEqual(accepted.desired_bytes, final_total)
        self.assertEqual(accepted.estimated_peak_bytes, ml.FFAT_SAFE_LIBRARY_BYTES)

        rejected_desired = [("01-replace.mspkg", replacement_size + 1), *filler]
        with self.assertRaisesRegex(ValueError, "当前文件、备份和临时文件"):
            ml.preflight_library_capacity(accepted_library, rejected_desired)


if __name__ == "__main__":
    unittest.main()
