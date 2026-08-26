#include <ESP_I2S.h>
#include <MiniSynthPins.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

namespace {
constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr size_t AUDIO_FRAMES_PER_BLOCK = 64;  // 4 ms.
constexpr uint8_t NOTE_COUNT = 7;
constexpr uint8_t INPUT_COUNT = 9;
constexpr uint8_t OCTAVE_COUNT = 3;            // C3, C4, C5 bases.
constexpr uint8_t DEBOUNCE_TICKS = 5;          // 5 ms at 1 kHz.
constexpr uint16_t INPUT_SCAN_PERIOD_MS = 1;
constexpr uint16_t UI_REFRESH_MS = 100;
constexpr float ATTACK_SECONDS = 0.010f;
constexpr float RELEASE_SECONDS = 0.025f;
constexpr float MODE_CROSSFADE_SECONDS = 0.030f;
constexpr int16_t MASTER_PEAK = 1800;
constexpr uint16_t WAVE_TABLE_SIZE = 256;
constexpr uint8_t MAX_ADDED_HARMONICS = 5;
constexpr float HARMONIC_BAND_LOW_HZ = 800.0f;
constexpr float HARMONIC_BAND_HIGH_HZ = 1800.0f;
constexpr float HARMONIC_FUNDAMENTAL_WEIGHT = 0.20f;
constexpr uint16_t STARTUP_SILENCE_BLOCKS = 25;
constexpr uint8_t OLED_ADDRESS_7BIT = 0x3C;
constexpr uint8_t OLED_YELLOW_ROWS = 16;
constexpr double PHASE_SCALE = 4294967296.0;  // 2^32.

// Test-only controls. The normal performance firmware is not changed.
constexpr uint8_t OCTAVE_CYCLE_PIN = MINI_SYNTH_PIN_KEY_PLAY_STOP;  // SW8.
constexpr uint8_t MODE_TOGGLE_PIN = MINI_SYNTH_PIN_KEY_FN;          // SW9.

const uint8_t NOTE_PINS[NOTE_COUNT] = {
    MINI_SYNTH_PIN_KEY_DO,
    MINI_SYNTH_PIN_KEY_RE,
    MINI_SYNTH_PIN_KEY_MI,
    MINI_SYNTH_PIN_KEY_FA,
    MINI_SYNTH_PIN_KEY_SOL,
    MINI_SYNTH_PIN_KEY_LA,
    MINI_SYNTH_PIN_KEY_TI,
};

const uint8_t ALL_INPUT_PINS[INPUT_COUNT] = {
    MINI_SYNTH_PIN_KEY_DO,
    MINI_SYNTH_PIN_KEY_RE,
    MINI_SYNTH_PIN_KEY_MI,
    MINI_SYNTH_PIN_KEY_FA,
    MINI_SYNTH_PIN_KEY_SOL,
    MINI_SYNTH_PIN_KEY_LA,
    MINI_SYNTH_PIN_KEY_TI,
    OCTAVE_CYCLE_PIN,
    MODE_TOGGLE_PIN,
};

const char *const NOTE_NAMES[NOTE_COUNT] = {"C", "D", "E", "F", "G", "A", "B"};
const float NOTE_FREQUENCIES_C4[NOTE_COUNT] = {
    261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f};

struct StereoFrame {
  int16_t left;
  int16_t right;
};
static_assert(sizeof(StereoFrame) == 4, "Unexpected stereo frame packing");

struct SharedTestState {
  uint8_t noteMask;
  uint8_t octaveIndex;
  bool harmonicMode;
};

I2SClass i2s;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE,
    MINI_SYNTH_PIN_I2C_SCL,
    MINI_SYNTH_PIN_I2C_SDA);

StereoFrame audioFrames[AUDIO_FRAMES_PER_BLOCK];
int16_t pureSineTable[WAVE_TABLE_SIZE];
int16_t harmonicTables[OCTAVE_COUNT][NOTE_COUNT][WAVE_TABLE_SIZE];
uint32_t phaseSteps[OCTAVE_COUNT][NOTE_COUNT];
uint32_t phases[NOTE_COUNT] = {};
float envelopes[NOTE_COUNT] = {};
uint8_t debounceCounters[INPUT_COUNT] = {};
bool debouncedPressed[INPUT_COUNT] = {};
bool previousPressed[INPUT_COUNT] = {};

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t sharedNoteMask = 0;
volatile uint8_t sharedOctaveIndex = 1;  // Start at C4-B4.
volatile bool sharedHarmonicMode = false;
volatile bool audioTaskHealthy = false;
volatile bool audioWriteFailed = false;

bool i2sReady = false;
bool displayReady = false;
uint32_t lastUiRefreshAt = 0;
uint8_t lastReportedNoteMask = 0;
uint8_t lastReportedOctaveIndex = 1;
bool lastReportedHarmonicMode = false;

float octaveMultiplier(uint8_t octaveIndex) {
  if (octaveIndex == 0) {
    return 0.5f;
  }
  if (octaveIndex == 2) {
    return 2.0f;
  }
  return 1.0f;
}

uint8_t octaveNumber(uint8_t octaveIndex) {
  return static_cast<uint8_t>(octaveIndex + 3);
}

void buildHarmonicTable(uint8_t octaveIndex, uint8_t noteIndex) {
  const float fundamentalHz =
      NOTE_FREQUENCIES_C4[noteIndex] * octaveMultiplier(octaveIndex);
  uint8_t firstHarmonic = static_cast<uint8_t>(ceilf(HARMONIC_BAND_LOW_HZ / fundamentalHz));
  if (firstHarmonic < 2) {
    firstHarmonic = 2;
  }

  uint8_t lastHarmonic = static_cast<uint8_t>(floorf(HARMONIC_BAND_HIGH_HZ / fundamentalHz));
  const uint8_t cappedLast =
      static_cast<uint8_t>(firstHarmonic + MAX_ADDED_HARMONICS - 1);
  if (lastHarmonic > cappedLast) {
    lastHarmonic = cappedLast;
  }

  float rawTable[WAVE_TABLE_SIZE];
  float peak = 0.0f;
  const bool hasAddedHarmonics = firstHarmonic <= lastHarmonic;

  for (uint16_t sampleIndex = 0; sampleIndex < WAVE_TABLE_SIZE; ++sampleIndex) {
    const float phase =
        6.28318530718f * static_cast<float>(sampleIndex) /
        static_cast<float>(WAVE_TABLE_SIZE);
    float value = sinf(phase);

    if (hasAddedHarmonics) {
      value = HARMONIC_FUNDAMENTAL_WEIGHT * sinf(phase);
      uint8_t orderInBand = 0;
      for (uint8_t harmonic = firstHarmonic;
           harmonic <= lastHarmonic;
           ++harmonic, ++orderInBand) {
        // Consecutive integer harmonics preserve the original periodicity.
        // Gentle weighting avoids making the highest component dominate.
        const float weight = 1.0f / sqrtf(1.0f + static_cast<float>(orderInBand));
        value += weight * sinf(phase * harmonic);
      }
    }

    rawTable[sampleIndex] = value;
    const float absoluteValue = fabsf(value);
    if (absoluteValue > peak) {
      peak = absoluteValue;
    }
  }

  if (peak < 0.0001f) {
    peak = 1.0f;
  }

  for (uint16_t sampleIndex = 0; sampleIndex < WAVE_TABLE_SIZE; ++sampleIndex) {
    harmonicTables[octaveIndex][noteIndex][sampleIndex] =
        static_cast<int16_t>(rawTable[sampleIndex] * 32767.0f / peak);
  }

  Serial.printf("HARMONICS,%s%u,FUNDAMENTAL,%.2f,FIRST,%u,LAST,%u\n",
                NOTE_NAMES[noteIndex],
                octaveNumber(octaveIndex),
                fundamentalHz,
                hasAddedHarmonics ? firstHarmonic : 0,
                hasAddedHarmonics ? lastHarmonic : 0);
}

void initializeSynthesisTables() {
  for (uint16_t i = 0; i < WAVE_TABLE_SIZE; ++i) {
    const float phase =
        6.28318530718f * static_cast<float>(i) /
        static_cast<float>(WAVE_TABLE_SIZE);
    pureSineTable[i] = static_cast<int16_t>(sinf(phase) * 32767.0f);
  }

  for (uint8_t octaveIndex = 0; octaveIndex < OCTAVE_COUNT; ++octaveIndex) {
    const float multiplier = octaveMultiplier(octaveIndex);
    for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
      const float frequency = NOTE_FREQUENCIES_C4[note] * multiplier;
      phaseSteps[octaveIndex][note] = static_cast<uint32_t>(
          frequency * PHASE_SCALE / SAMPLE_RATE_HZ);
      buildHarmonicTable(octaveIndex, note);
    }
  }
}

SharedTestState readTestState() {
  SharedTestState snapshot;
  portENTER_CRITICAL(&stateMux);
  snapshot.noteMask = sharedNoteMask;
  snapshot.octaveIndex = sharedOctaveIndex;
  snapshot.harmonicMode = sharedHarmonicMode;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

void publishTestState(uint8_t noteMask, uint8_t octaveIndex, bool harmonicMode) {
  portENTER_CRITICAL(&stateMux);
  sharedNoteMask = noteMask;
  sharedOctaveIndex = octaveIndex;
  sharedHarmonicMode = harmonicMode;
  portEXIT_CRITICAL(&stateMux);
}

void inputScanTask(void *) {
  TickType_t lastWake = xTaskGetTickCount();
  uint8_t octaveIndex = 1;
  bool harmonicMode = false;

  for (;;) {
    uint8_t noteMask = 0;

    for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
      const bool rawPressed =
          digitalRead(ALL_INPUT_PINS[i]) == MINI_SYNTH_KEY_ACTIVE_LEVEL;

      if (rawPressed) {
        if (debounceCounters[i] < DEBOUNCE_TICKS) {
          ++debounceCounters[i];
        }
      } else if (debounceCounters[i] > 0) {
        --debounceCounters[i];
      }

      if (debounceCounters[i] == DEBOUNCE_TICKS) {
        debouncedPressed[i] = true;
      } else if (debounceCounters[i] == 0) {
        debouncedPressed[i] = false;
      }

      if (i < NOTE_COUNT && debouncedPressed[i]) {
        noteMask |= static_cast<uint8_t>(1U << i);
      }
    }

    const bool octavePressedEdge =
        debouncedPressed[NOTE_COUNT] && !previousPressed[NOTE_COUNT];
    const bool modePressedEdge =
        debouncedPressed[NOTE_COUNT + 1] && !previousPressed[NOTE_COUNT + 1];

    if (octavePressedEdge) {
      octaveIndex = static_cast<uint8_t>((octaveIndex + 1) % OCTAVE_COUNT);
    }
    if (modePressedEdge) {
      harmonicMode = !harmonicMode;
    }

    for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
      previousPressed[i] = debouncedPressed[i];
    }

    publishTestState(noteMask, octaveIndex, harmonicMode);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(INPUT_SCAN_PERIOD_MS));
  }
}

bool writeAudioFrames() {
  const size_t byteCount = sizeof(audioFrames);
  const size_t written = i2s.write(audioFrames, byteCount);
  if (written != byteCount) {
    audioWriteFailed = true;
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
    return false;
  }
  return true;
}

void fillSilenceBlock() {
  for (size_t frame = 0; frame < AUDIO_FRAMES_PER_BLOCK; ++frame) {
    audioFrames[frame] = {0, 0};
  }
}

void audioRenderTask(void *) {
  const float attackStep = 1.0f / (ATTACK_SECONDS * SAMPLE_RATE_HZ);
  const float releaseStep = 1.0f / (RELEASE_SECONDS * SAMPLE_RATE_HZ);
  const float modeFadeStep = 1.0f / (MODE_CROSSFADE_SECONDS * SAMPLE_RATE_HZ);
  uint8_t previousNoteMask = 0;
  float harmonicMix = 0.0f;

  fillSilenceBlock();
  for (uint16_t block = 0; block < STARTUP_SILENCE_BLOCKS; ++block) {
    if (!writeAudioFrames()) {
      vTaskDelete(nullptr);
      return;
    }
  }

  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_ENABLE_LEVEL);
  audioTaskHealthy = true;

  for (;;) {
    const SharedTestState state = readTestState();
    const uint8_t pressedEdges =
        static_cast<uint8_t>(state.noteMask & static_cast<uint8_t>(~previousNoteMask));

    for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
      if (pressedEdges & static_cast<uint8_t>(1U << note)) {
        phases[note] = 0;
        envelopes[note] = 0.0f;
      }
    }
    previousNoteMask = state.noteMask;

    for (size_t frame = 0; frame < AUDIO_FRAMES_PER_BLOCK; ++frame) {
      const float harmonicTarget = state.harmonicMode ? 1.0f : 0.0f;
      if (harmonicMix < harmonicTarget) {
        harmonicMix += modeFadeStep;
        if (harmonicMix > harmonicTarget) {
          harmonicMix = harmonicTarget;
        }
      } else if (harmonicMix > harmonicTarget) {
        harmonicMix -= modeFadeStep;
        if (harmonicMix < harmonicTarget) {
          harmonicMix = harmonicTarget;
        }
      }

      float mixed = 0.0f;
      float envelopeWeight = 0.0f;

      for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
        const bool pressed =
            (state.noteMask & static_cast<uint8_t>(1U << note)) != 0;

        if (pressed) {
          envelopes[note] += attackStep;
          if (envelopes[note] > 1.0f) {
            envelopes[note] = 1.0f;
          }
        } else {
          envelopes[note] -= releaseStep;
          if (envelopes[note] < 0.0f) {
            envelopes[note] = 0.0f;
          }
        }

        if (envelopes[note] > 0.0001f) {
          const uint8_t tableIndex = static_cast<uint8_t>(phases[note] >> 24);
          const float pureSample =
              static_cast<float>(pureSineTable[tableIndex]) / 32767.0f;
          const float harmonicSample =
              static_cast<float>(
                  harmonicTables[state.octaveIndex][note][tableIndex]) /
              32767.0f;
          const float selectedSample =
              pureSample + (harmonicSample - pureSample) * harmonicMix;

          mixed += selectedSample * envelopes[note];
          envelopeWeight += envelopes[note];
          phases[note] += phaseSteps[state.octaveIndex][note];
        }
      }

      if (envelopeWeight > 1.0f) {
        mixed /= envelopeWeight;
      }

      if (mixed > 1.0f) {
        mixed = 1.0f;
      } else if (mixed < -1.0f) {
        mixed = -1.0f;
      }

      const int16_t sample = static_cast<int16_t>(mixed * MASTER_PEAK);
      audioFrames[frame].left = 0;
      audioFrames[frame].right = sample;
    }

    if (!writeAudioFrames()) {
      audioTaskHealthy = false;
      vTaskDelete(nullptr);
      return;
    }
  }
}

void drawCentered(const char *text, uint8_t baselineY) {
  const int16_t width = display.getStrWidth(text);
  int16_t x = static_cast<int16_t>((128 - width) / 2);
  if (x < 0) {
    x = 0;
  }
  display.drawStr(x, baselineY, text);
}

void buildChordLabel(uint8_t noteMask, char *buffer, size_t size) {
  buffer[0] = '\0';
  bool found = false;

  for (uint8_t i = 0; i < NOTE_COUNT; ++i) {
    if ((noteMask & static_cast<uint8_t>(1U << i)) == 0) {
      continue;
    }
    if (found) {
      strncat(buffer, " ", size - strlen(buffer) - 1);
    }
    strncat(buffer, NOTE_NAMES[i], size - strlen(buffer) - 1);
    found = true;
  }

  if (!found) {
    strncpy(buffer, "READY", size - 1);
    buffer[size - 1] = '\0';
  }
}

void updateDisplay(const SharedTestState &state) {
  if (!displayReady) {
    return;
  }

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, state.harmonicMode ? "HARM" : "PURE");

  char octaveLabel[8];
  snprintf(octaveLabel, sizeof(octaveLabel), "C%u-B%u",
           octaveNumber(state.octaveIndex),
           octaveNumber(state.octaveIndex));
  const int16_t octaveWidth = display.getStrWidth(octaveLabel);
  display.drawStr(126 - octaveWidth, 11, octaveLabel);
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  char chordLabel[24];
  buildChordLabel(state.noteMask, chordLabel, sizeof(chordLabel));
  display.setFont(u8g2_font_ncenB12_tr);
  drawCentered(chordLabel, 38);

  display.setFont(u8g2_font_5x7_tf);
  constexpr uint8_t cellWidth = 16;
  constexpr uint8_t cellGap = 2;
  constexpr uint8_t cellsX = 2;
  constexpr uint8_t cellsY = 46;
  constexpr uint8_t cellsHeight = 16;

  for (uint8_t i = 0; i < NOTE_COUNT; ++i) {
    const uint8_t x = static_cast<uint8_t>(cellsX + i * (cellWidth + cellGap));
    const bool pressed =
        (state.noteMask & static_cast<uint8_t>(1U << i)) != 0;

    if (pressed) {
      display.drawBox(x, cellsY, cellWidth, cellsHeight);
      display.setDrawColor(0);
    } else {
      display.drawFrame(x, cellsY, cellWidth, cellsHeight);
      display.setDrawColor(1);
    }

    const int16_t labelWidth = display.getStrWidth(NOTE_NAMES[i]);
    display.drawStr(x + (cellWidth - labelWidth) / 2, 58, NOTE_NAMES[i]);
    display.setDrawColor(1);
  }

  display.sendBuffer();
}

void reportStateChanges(const SharedTestState &state) {
  if (state.noteMask != lastReportedNoteMask) {
    Serial.printf("NOTE_MASK,0x%02X\n", state.noteMask);
  }
  if (state.octaveIndex != lastReportedOctaveIndex) {
    Serial.printf("OCTAVE,C%u-B%u\n",
                  octaveNumber(state.octaveIndex),
                  octaveNumber(state.octaveIndex));
  }
  if (state.harmonicMode != lastReportedHarmonicMode) {
    Serial.printf("MODE,%s\n", state.harmonicMode ? "HARM" : "PURE");
  }

  lastReportedNoteMask = state.noteMask;
  lastReportedOctaveIndex = state.octaveIndex;
  lastReportedHarmonicMode = state.harmonicMode;
}
}  // namespace

void setup() {
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  Serial.begin(115200);
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }

  for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
    pinMode(ALL_INPUT_PINS[i], MINI_SYNTH_KEY_PIN_MODE);
  }

  Serial.println();
  Serial.println("HARMONIC_AB_TEST_BEGIN");
  initializeSynthesisTables();

  Wire.begin(MINI_SYNTH_PIN_I2C_SDA, MINI_SYNTH_PIN_I2C_SCL);
  Wire.setClock(400000);
  display.setI2CAddress(OLED_ADDRESS_7BIT << 1);
  display.setBusClock(400000);
  display.begin();
  display.setContrast(96);
  displayReady = true;

  i2s.setPins(MINI_SYNTH_PIN_I2S_BCLK,
              MINI_SYNTH_PIN_I2S_LRCLK,
              MINI_SYNTH_PIN_I2S_DOUT);
  i2sReady = i2s.begin(I2S_MODE_STD,
                       SAMPLE_RATE_HZ,
                       I2S_DATA_BIT_WIDTH_16BIT,
                       I2S_SLOT_MODE_STEREO);

  Serial.printf("TEST,I2S_INIT,%s\n", i2sReady ? "PASS" : "FAIL");
  Serial.println("INFO,SW8,SHORT_PRESS_CYCLES_C3_C4_C5");
  Serial.println("INFO,SW9,SHORT_PRESS_TOGGLES_PURE_HARM");
  Serial.printf("INFO,MASTER_PEAK,%d,SAME_PEAK_BOTH_MODES\n", MASTER_PEAK);
  Serial.printf("INFO,HARMONIC_TARGET_BAND,%.0F,%.0F\n",
                HARMONIC_BAND_LOW_HZ,
                HARMONIC_BAND_HIGH_HZ);

  if (!i2sReady) {
    updateDisplay(readTestState());
    return;
  }

  const BaseType_t inputTaskResult = xTaskCreate(
      inputScanTask, "ab-input", 3072, nullptr, 4, nullptr);
  const BaseType_t audioTaskResult = xTaskCreate(
      audioRenderTask, "ab-audio", 4096, nullptr, 3, nullptr);
  Serial.printf("TEST,INPUT_TASK,%s\n",
                inputTaskResult == pdPASS ? "PASS" : "FAIL");
  Serial.printf("TEST,AUDIO_TASK,%s\n",
                audioTaskResult == pdPASS ? "PASS" : "FAIL");

  if (inputTaskResult != pdPASS || audioTaskResult != pdPASS) {
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  }

  updateDisplay(readTestState());
  lastUiRefreshAt = millis();
}

void loop() {
  const SharedTestState state = readTestState();
  reportStateChanges(state);

  if (audioWriteFailed) {
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  }

  const uint32_t now = millis();
  if (now - lastUiRefreshAt >= UI_REFRESH_MS) {
    updateDisplay(state);
    lastUiRefreshAt = now;
  }

  delay(2);
}
