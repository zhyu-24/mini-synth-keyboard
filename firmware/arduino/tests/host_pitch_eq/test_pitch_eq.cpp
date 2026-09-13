#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace current {
#include "current.inc"
}
#ifdef HAS_BASELINE
namespace baseline {
#include "baseline.inc"
}
#endif

void testSettings() {
  using namespace current;
  const auto assertDefaults = [] {
    assert(sharedGlobalVolumePercent == 100);
    assert(sharedPlayOctaveMode == PlayOctaveMode::Hold);
    assert(sharedPlayTimbre == SongTimbre::Sine);
    assert(!sharedPitchEqEnabled && sharedLatchedPlayOctave == 0);
  };
  nvs.clear(); loadPersistentSettings(); assertDefaults();
  // Exhaustively preserve every legal legacy combination, with no write on load.
  for (int volume = 0; volume <= 100; volume += 10)
    for (int mode = 0; mode < 2; ++mode)
      for (int timbre = 0; timbre < 4; ++timbre) {
        PersistentSettingsRecordV1 old = {SETTINGS_MAGIC, 1, static_cast<uint8_t>(volume),
            static_cast<uint8_t>(mode), static_cast<uint8_t>(timbre), 0};
        old.checksum = settingsBytesChecksum(&old, offsetof(PersistentSettingsRecordV1, checksum));
        nvs.assign(reinterpret_cast<uint8_t *>(&old), reinterpret_cast<uint8_t *>(&old) + sizeof(old));
        const auto saved = nvs;
        loadPersistentSettings();
        assert(nvs == saved && nvs.size() == 12);
        assert(sharedGlobalVolumePercent == volume);
        assert(static_cast<int>(sharedPlayOctaveMode) == mode);
        assert(static_cast<int>(sharedPlayTimbre) == timbre);
        assert(!sharedPitchEqEnabled);
        for (bool enabled : {false, true}) {
          sharedPitchEqEnabled = enabled;
          markSettingsChanged(); serviceSettingsPersistence();
          assert(nvs.size() == 13 && !sharedSettingsSavePending);
          sharedPitchEqEnabled = !enabled; sharedGlobalVolumePercent = 100;
          loadPersistentSettings();
          assert(sharedPitchEqEnabled == enabled && sharedGlobalVolumePercent == volume);
          assert(static_cast<int>(sharedPlayOctaveMode) == mode);
          assert(static_cast<int>(sharedPlayTimbre) == timbre);
        }
      }
  const auto good = nvs;
  // Every bit is covered by the checksum, including the new boolean and checksum itself.
  for (size_t byte = 0; byte < good.size(); ++byte)
    for (int bit = 0; bit < 8; ++bit) {
      nvs = good; nvs[byte] ^= 1U << bit;
      loadPersistentSettings(); assertDefaults();
    }
  auto record = makeSettingsRecord(readSettingsSnapshot());
  const auto reject = [&](PersistentSettingsRecord bad) {
    bad.checksum = settingsChecksum(bad); // semantic validation even with valid checksum
    nvs.assign(reinterpret_cast<uint8_t *>(&bad), reinterpret_cast<uint8_t *>(&bad) + sizeof(bad));
    loadPersistentSettings(); assertDefaults();
  };
  for (int value = 2; value < 256; ++value) {
    auto bad = record; bad.pitchEqEnabled = value; reject(bad);
  }
  for (int value : {1, 9, 99, 101, 255}) { auto bad = record; bad.volumePercent = value; reject(bad); }
  { auto bad = record; bad.octaveMode = 2; reject(bad); }
  { auto bad = record; bad.playTimbre = 4; reject(bad); }
  for (int version : {0, 1, 3, 255}) { auto bad = record; bad.version = version; reject(bad); }
  { auto bad = record; bad.magic = 0; reject(bad); }
  PersistentSettingsRecordV1 old = {SETTINGS_MAGIC, 1, 70, 1, 3, 0};
  for (int variant = 0; variant < 6; ++variant) {
    auto bad = old;
    if (variant == 0) bad.volumePercent = 71;
    if (variant == 1) bad.octaveMode = 2;
    if (variant == 2) bad.playTimbre = 4;
    if (variant == 3) bad.version = 2;
    if (variant == 4) bad.magic = 0;
    bad.checksum = settingsBytesChecksum(&bad, offsetof(PersistentSettingsRecordV1, checksum));
    if (variant == 5) bad.checksum ^= 1;
    nvs.assign(reinterpret_cast<uint8_t *>(&bad), reinterpret_cast<uint8_t *>(&bad) + sizeof(bad));
    loadPersistentSettings(); assertDefaults();
  }
  for (int length : {0, 1, 11, 14, 100}) {
    nvs.assign(length, 0); loadPersistentSettings(); assertDefaults();
  }
  nvs = good; nvsShortRead = true; loadPersistentSettings(); assertDefaults(); nvsShortRead = false;
  nvsAvailable = false; loadPersistentSettings(); assertDefaults(); nvsAvailable = true;
  sharedPitchEqEnabled = true; markSettingsChanged(); nvsWriteFails = true;
  serviceSettingsPersistence(); assert(sharedSettingsSavePending);
  nvsWriteFails = false; serviceSettingsPersistence(); assert(!sharedSettingsSavePending);
  loadPersistentSettings(); assert(sharedPitchEqEnabled);
  std::cout << "PASS: v1 migration, v2 round trips, malformed fields/checksums, NVS failures\n";
}

void testMenu() {
  using namespace current;
  sharedSettingRow = SettingRow::Volume;
  selectSettingRow(-1); assert(sharedSettingRow == SettingRow::PitchEq);
  selectSettingRow(1); assert(sharedSettingRow == SettingRow::Volume);
  for (int cycle = 0; cycle < 8; ++cycle) {
    const int selected = static_cast<int>(sharedSettingRow);
    assert(selected == cycle % 4);
    const int first = settingsFirstVisibleRow(sharedSettingRow);
    assert(first <= selected && selected < first + 3 && first + 3 <= 4);
    drawSettingsDisplay();
    assert(display.rows.count(24) && display.rows.count(34) && display.rows.count(44));
    assert(display.rows[24 + (selected - first) * 10][0] == '>');
    assert(display.rows[55] == "C/D ROW  E/F CHANGE");
    assert(display.rows[63] == "G RESET  HOLD BOTH HOME");
    for (int y : {24, 34, 44}) assert(display.rows[y].size() * 5 + 1 <= 128);
    selectSettingRow(1);
  }
  assert(badgeDraws == 8);
  sharedSettingRow = SettingRow::PitchEq;
  changeSelectedSetting(-1, false); assert(!sharedPitchEqEnabled);
  changeSelectedSetting(1, false); assert(sharedPitchEqEnabled);
  drawSettingsDisplay(); assert(display.rows[44].find("PITCH EQ:    ON") != std::string::npos);
  changeSelectedSetting(0, true); assert(!sharedPitchEqEnabled);
  drawSettingsDisplay(); assert(display.rows[44].find("PITCH EQ:    OFF") != std::string::npos);
  sharedSettingRow = SettingRow::Volume; changeSelectedSetting(0, true);
  changeSelectedSetting(-1, false); assert(sharedGlobalVolumePercent == 90);
  changeSelectedSetting(1, false); assert(sharedGlobalVolumePercent == 100);
  sharedSettingRow = SettingRow::OctaveMode; changeSelectedSetting(1, false);
  assert(sharedPlayOctaveMode == PlayOctaveMode::Latch);
  changeSelectedSetting(0, true); assert(sharedPlayOctaveMode == PlayOctaveMode::Hold);
  sharedSettingRow = SettingRow::PlaySound; changeSelectedSetting(0, true);
  changeSelectedSetting(1, false); assert(sharedPlayTimbre == SongTimbre::EightBit);
  changeSelectedSetting(0, true); assert(sharedPlayTimbre == SongTimbre::Sine);
  std::cout << "PASS: four-row wrap, three visible rows, E/F/G, BLE badge and footer\n";
}

void testGains() {
  using namespace current;
  initializeTables();
#ifdef HAS_BASELINE
  baseline::initializeTables();
#endif
  for (int timbre = 0; timbre < 4; ++timbre) {
    const auto sound = static_cast<SongTimbre>(timbre);
    for (int midi = 0; midi < 128; ++midi) {
      assert(calibratedPitchGain(sound, midi, false) == 1.0f);
      assert(normalizationPitchGain(midi, false) == 1.0f);
      const float expected = interpolatedSinePitchGain(midi) * nonSineOctaveCompensation(sound, midi);
      assert(calibratedPitchGain(sound, midi, true) == expected);
      assert(normalizationPitchGain(midi, true) == interpolatedSinePitchGain(midi));
#ifdef HAS_BASELINE
      assert(calibratedPitchGain(sound, midi, true) == baseline::calibratedPitchGain(
          static_cast<baseline::SongTimbre>(timbre), midi));
#endif
    }
    for (int octave = -1; octave <= 1; ++octave)
      for (int note = 0; note < 7; ++note) {
        setPitchImmediately(note, octave, sound, true);
        const float expected = noteGainByOctave[octave + 1][note] *
            nonSineOctaveCompensation(sound, 60 + note + 12 * octave);
        assert(currentPitchGains[note] == expected);
#ifdef HAS_BASELINE
        baseline::setPitchImmediately(note, octave, static_cast<baseline::SongTimbre>(timbre));
        assert(currentPitchGains[note] == baseline::currentPitchGains[note]);
#endif
        setPitchImmediately(note, octave, sound, false);
        assert(currentPitchGains[note] == 1 && glideStartPitchGains[note] == 1 && glideTargetPitchGains[note] == 1);
        assert(currentNormalizationPitchGains[note] == 1 && glideTargetNormalizationPitchGains[note] == 1);
      }
  }
  std::cout << "PASS: all 128 MIDI pitches / 4 timbres; all 21 Play gains and cache resets\n";
}

void testAudio() {
  using namespace current;
  for (int timbre = 0; timbre < 4; ++timbre) {
    for (bool song : {false, true}) {
      const auto on = runAudio(timbre, song, true);
      assert(std::any_of(on.begin(), on.end(), [](int16_t sample) { return sample != 0; }));
#ifdef HAS_BASELINE
      assert(on == baseline::runAudio(timbre, song, true));
#endif
      const auto off = runAudio(timbre, song, false);
      assert(off != on);
      if (!song) {
        for (const auto &gains : capturedGains) for (float gain : gains) assert(gain == 1.0f);
        for (const auto &weights : capturedWeights) for (float gain : weights) assert(gain == 1.0f);
      }
      const auto muted = runAudio(timbre, song, false, [](int) { sharedGlobalVolumePercent = 0; });
      assert(std::all_of(muted.begin(), muted.end(), [](int16_t sample) { return sample == 0; }));
      // Toggle during sustained notes and release tails: existing audio stays bit-identical.
      const int changeBlock = song ? 15 : 19;
      const int nextAttackBlock = song ? 21 : 25;
      const auto changed = runAudio(timbre, song, true,
          [=](int block) { if (block == changeBlock) setEq(false); });
      assert(std::equal(on.begin(), on.begin() + nextAttackBlock * 64, changed.begin()));
      assert(!std::equal(on.begin() + (nextAttackBlock + 1) * 64, on.end(),
                        changed.begin() + (nextAttackBlock + 1) * 64));
      const auto changeOn = runAudio(timbre, song, false,
          [=](int block) { if (block == changeBlock) setEq(true); });
      assert(std::equal(off.begin(), off.begin() + nextAttackBlock * 64, changeOn.begin()));
      assert(!std::equal(off.begin() + (nextAttackBlock + 1) * 64, off.end(),
                        changeOn.begin() + (nextAttackBlock + 1) * 64));
    }
  }
  // Check every sample of the retained 12 ms smoothstep, and mixed latched states.
  SongTimbre timbres[7]; bool flags[7];
  for (int note = 0; note < 7; ++note) {
    timbres[note] = SongTimbre::Organ; flags[note] = note % 2;
    envelopes[note] = 1; setPitchImmediately(note, 0, timbres[note], flags[note]);
  }
  startOctaveGlide(1, 0x7f, timbres, flags);
  assert(glideSamplesRemaining == 192);
  for (int sample = 1; sample <= 192; ++sample) {
    advanceOctaveGlide();
    const float blend = smoothstep01(sample / 192.0f);
    for (int note = 0; note < 7; ++note) {
      assert(currentPitchGains[note] == glideStartPitchGains[note] +
          (glideTargetPitchGains[note] - glideStartPitchGains[note]) * blend);
      if (!flags[note]) assert(currentPitchGains[note] == 1 && currentNormalizationPitchGains[note] == 1);
    }
  }
  assert(glideSamplesRemaining == 0);
  std::cout << "PASS: complete Play/Song render, chords, tail latching, next attacks, 192-sample glide, mute/clamp\n";
#ifdef HAS_BASELINE
  std::cout << "PASS: EQ ON is sample-identical to the pre-change audio task (both apps / four timbres)\n";
#else
  std::cout << "SKIP: optional pre-change sample parity (provide --baseline)\n";
#endif
}

int main() {
  testSettings(); testMenu(); testGains(); testAudio();
}
