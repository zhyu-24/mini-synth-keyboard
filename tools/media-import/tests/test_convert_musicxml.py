from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import convert_musicxml as converter  # noqa: E402


def note(step: str) -> str:
    return (
        "<note><pitch><step>" + step + "</step><octave>4</octave></pitch>"
        "<duration>1</duration><voice>1</voice></note>"
    )


def measure(number: int, step: str, *, left: str = "", right: str = "", prefix: str = "") -> str:
    attributes = "<attributes><divisions>1</divisions><time><beats>4</beats><beat-type>4</beat-type></time></attributes>" if number == 1 else ""
    return f'<measure number="{number}">{prefix}{left}{attributes}{note(step)}{right}</measure>'


def repeat(direction: str, *, times: int | None = None, location: str = "right") -> str:
    times_attribute = "" if times is None else f' times="{times}"'
    return f'<barline location="{location}"><repeat direction="{direction}"{times_attribute}/></barline>'


def ending(number: str, kind: str, *, location: str) -> str:
    return f'<barline location="{location}"><ending number="{number}" type="{kind}"/></barline>'


class RepeatExpansionTests(unittest.TestCase):
    def parse_measures(self, measures: list[str]):
        xml = (
            '<?xml version="1.0" encoding="UTF-8"?>'
            '<score-partwise version="4.0"><part-list><score-part id="P1"><part-name>Piano</part-name>'
            '</score-part></part-list><part id="P1">' + "".join(measures) + "</part></score-partwise>"
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.musicxml"
            path.write_text(xml, encoding="utf-8")
            return converter.parse(path)

    def test_two_sequential_default_repeats_expand_in_order(self) -> None:
        parsed = self.parse_measures(
            [
                measure(1, "C", left=repeat("forward", location="left")),
                measure(2, "D", right=repeat("backward")),
                measure(3, "E", left=repeat("forward", location="left")),
                measure(4, "F", right=repeat("backward")),
            ]
        )
        self.assertEqual([event.midi for event in parsed["events"]], [60, 62, 60, 62, 64, 65, 64, 65])
        self.assertEqual(parsed["repeat_blocks"], 2)
        self.assertEqual(parsed["repeat_expanded_measures"], 8)

    def test_repeat_times_duplicates_tempo_and_time_positions(self) -> None:
        tempo = '<direction><sound tempo="100"/></direction>'
        parsed = self.parse_measures(
            [
                measure(1, "C", left=repeat("forward", location="left"), prefix=tempo),
                measure(2, "D", right=repeat("backward", times=3)),
            ]
        )
        self.assertEqual(len(parsed["events"]), 6)
        self.assertEqual([tick for tick, _bpm in parsed["tempos"]], [0, 960, 1920])
        self.assertEqual([tick for tick, _num, _den in parsed["times"]], [0, 960, 1920])

    def test_first_and_second_endings_follow_play_pass(self) -> None:
        parsed = self.parse_measures(
            [
                measure(1, "C", left=repeat("forward", location="left")),
                measure(
                    2,
                    "D",
                    left=ending("1", "start", location="left"),
                    right=ending("1", "stop", location="right") + repeat("backward"),
                ),
                measure(
                    3,
                    "E",
                    left=ending("2", "start", location="left"),
                    right=ending("2", "stop", location="right"),
                ),
            ]
        )
        self.assertEqual([event.midi for event in parsed["events"]], [60, 62, 60, 64])

    def test_nested_unclosed_invalid_times_and_jumps_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "nested"):
            self.parse_measures(
                [
                    measure(1, "C", left=repeat("forward", location="left")),
                    measure(2, "D", left=repeat("forward", location="left")),
                    measure(3, "E", right=repeat("backward")),
                ]
            )
        with self.assertRaisesRegex(ValueError, "not closed"):
            self.parse_measures([measure(1, "C", left=repeat("forward", location="left"))])
        with self.assertRaisesRegex(ValueError, "1..16"):
            self.parse_measures([measure(1, "C", right=repeat("backward", times=17))])
        with self.assertRaisesRegex(ValueError, "playback jumps"):
            self.parse_measures([measure(1, "C", prefix='<direction><sound dacapo="yes"/></direction>')])
        with self.assertRaisesRegex(ValueError, "no associated repeat"):
            self.parse_measures(
                [
                    measure(
                        1,
                        "C",
                        left=ending("1", "start", location="left"),
                        right=ending("1", "stop", location="right"),
                    )
                ]
            )


if __name__ == "__main__":
    unittest.main()
