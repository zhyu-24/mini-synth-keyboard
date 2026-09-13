"""Run production C++ settings/audio on a host compiler; never access a device.

python3 run_tests.py [--baseline path/to/pre-change/mini_synth_v1.ino]
Requires g++ (Windows users can run this script in WSL). Generated files stay in
an automatically cleaned temporary directory, not in the production sketch.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SKETCH = HERE.parents[1] / "apps/mini_synth_v1/mini_synth_v1.ino"


def function(source, name):
    match = re.search(r"^[\w :*&]+\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(f"Missing production function: {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def between(source, start, end):
    return source[source.index(start):source.index(end)]


def structure(source, name):
    return re.search(r"struct " + name + r" \{.*?^};", source, re.M | re.S)[0]


def extract(source, current):
    parts = [
        between(source, "constexpr uint32_t SAMPLE_RATE_HZ", "// BLE game keyboard:"),
        between(source, "constexpr uint8_t EMBEDDED_SONG_COUNT", "// MUSB v1"),
        between(source, "enum class AppId", "enum class KeyboardBleState"),
    ]
    for name in ("StereoFrame", "SharedPerformanceState", "SongEvent", "SequenceVoice",
                 "CatalogSong", "SettingsSnapshot", "SongSnapshot"):
        parts.append(structure(source, name))
    parts += [
        "constexpr uint8_t SEQUENCE_VOICE_COUNT = MAX_SEQUENCE_VOICES;",
        between(source, "StereoFrame audioFrames[", "uint8_t debounceCounters["),
        between(source, "volatile uint8_t sharedNoteMask", "volatile bool sharedKeyboardConnected"),
        '#include "host_stubs.hpp"',
    ]
    names = ["readSettingsSnapshot", "settingsBytesChecksum", "settingsChecksum",
             "makeSettingsRecord", "validSettingsRecord", "decodeSettingsRecord",
             "loadPersistentSettings", "markSettingsChanged", "serviceSettingsPersistence",
             "selectSettingRow", "changeSelectedSetting", "readSongSnapshot", "setSongStatus",
             "nonSineOctaveCompensation", "phaseStepForOctave", "octaveGainIndex", "smoothstep01",
             "setPitchImmediately", "startOctaveGlide", "advanceOctaveGlide", "fillSilenceBlock",
             "interpolatedSinePitchGain", "normalizationPitchGain", "calibratedPitchGain",
             "phaseStepForMidi", "findSongEventIndex", "sineForPhase", "renderSongOscillator",
             "audioRenderTask", "songTimbreName", "octaveModeName", "settingsFirstVisibleRow",
             "drawSettingsDisplay"]
    added = {"settingsBytesChecksum", "decodeSettingsRecord", "normalizationPitchGain",
             "settingsFirstVisibleRow"}
    parts.extend(function(source, n) for n in names if current or n not in added)
    parts += ["void setEq(bool enabled) { " +
              ("sharedPitchEqEnabled = enabled;" if current else "(void)enabled;") + " }",
              '#include "audio_fixture.hpp"']
    return "\n".join(parts)


def check_invariants(source, baseline):
    assert "constexpr int16_t MASTER_PEAK = 18000;" in source
    assert "constexpr uint16_t OCTAVE_GLIDE_MS = 12;" in source
    audio = function(source, "audioRenderTask")
    assert "Preferences" not in audio and "Serial" not in audio and "FFat" not in audio
    assert audio.count("readSettingsSnapshot()") == 1
    assert "latchedPlayPitchEq[note] = audioSettings.pitchEqEnabled;" in audio
    assert "normalizationPitchGain(event.midiPitch, audioSettings.pitchEqEnabled)" in audio
    if baseline:
        # Large immutable areas (songs, protocols, input and startup) are unchanged.
        for start, end in [
            ("constexpr float NOTE_ATTENUATION_DB", "constexpr uint8_t OLED_ADDRESS"),
            ("constexpr SongEvent SONG_VOYAGERFAREWELL_EVENTS", "StereoFrame audioFrames["),
            ("void inputScanTask", "float interpolatedSinePitchGain"),
            ("void setup()", "void loop()"),
        ]:
            assert between(source, start, end) == between(baseline, start, end), start
        assert function(source, "renderSongOscillator") == function(baseline, "renderSongOscillator")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=pathlib.Path)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = SKETCH.read_text(encoding="utf-8-sig")
    baseline = args.baseline.read_text(encoding="utf-8-sig") if args.baseline else None
    check_invariants(source, baseline)
    with tempfile.TemporaryDirectory(prefix="mini-synth-pitch-eq-") as directory:
        directory = pathlib.Path(directory)
        (directory / "current.inc").write_text(extract(source, True), encoding="utf-8")
        flags = []
        if baseline:
            assert "DEFAULT_PITCH_EQ_ENABLED" not in baseline, "Baseline must predate pitch EQ"
            (directory / "baseline.inc").write_text(extract(baseline, False), encoding="utf-8")
            flags.append("-DHAS_BASELINE")
        executable = directory / "pitch_eq_tests"
        subprocess.run([args.cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all", *flags,
                        "-I", str(directory), "-I", str(HERE), str(HERE / "test_pitch_eq.cpp"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: production source invariants; no device I/O")


if __name__ == "__main__":
    main()
