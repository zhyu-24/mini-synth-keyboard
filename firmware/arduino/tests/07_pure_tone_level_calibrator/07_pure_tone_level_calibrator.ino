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
constexpr uint8_t OCTAVE_COUNT = 3;            // Low, middle, high.
constexpr uint8_t PITCH_COUNT = NOTE_COUNT * OCTAVE_COUNT;
constexpr uint8_t INPUT_COUNT = 9;
constexpr uint8_t DEBOUNCE_TICKS = 5;          // 5 ms at 1 kHz.
constexpr uint16_t INPUT_SCAN_PERIOD_MS = 1;
constexpr uint16_t UI_REFRESH_MS = 100;
constexpr float ATTACK_SECONDS = 0.010f;
constexpr float RELEASE_SECONDS = 0.025f;
constexpr float LEVEL_CROSSFADE_SECONDS = 0.030f;
constexpr int16_t CALIBRATION_MASTER_PEAK = 12000;
constexpr float MIN_ATTENUATION_DB = -36.0f;
constexpr float MAX_ATTENUATION_DB = 0.0f;
constexpr float ATTENUATION_STEP_DB = 1.0f;
constexpr uint16_t SINE_TABLE_SIZE = 256;
constexpr uint16_t STARTUP_SILENCE_BLOCKS = 25;
constexpr uint8_t OLED_ADDRESS_7BIT = 0x3C;
constexpr uint8_t OLED_YELLOW_ROWS = 16;
constexpr double PHASE_SCALE = 4294967296.0;  // 2^32.

// Calibrator controls:
// - Any note key selects that note inside the current low/mid/high range.
// - SW8 short press lowers the selected tone by 1 dB.
// - SW9 short press raises the selected tone by 1 dB (never above 0 dB).
// - SW8 long press cycles low -> middle -> high ranges.
// - SW9 long press prints the complete 21-value table to Serial.
constexpr uint8_t LEVEL_DOWN_PIN = MINI_SYNTH_PIN_KEY_PLAY_STOP;  // SW8.
constexpr uint8_t LEVEL_UP_PIN = MINI_SYNTH_PIN_KEY_FN;           // SW9.
constexpr uint16_t LONG_PRESS_MS = 700;

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
    LEVEL_DOWN_PIN,
    LEVEL_UP_PIN,
};

const char *const NOTE_NAMES[NOTE_COUNT] = {"C", "D", "E", "F", "G", "A", "B"};
const char *const SOLFEGE_NAMES[NOTE_COUNT] = {"DO", "RE", "MI", "FA", "SOL", "LA", "TI"};
const char *const RANGE_NAMES[OCTAVE_COUNT] = {"LOW", "MID", "HIGH"};
const float NOTE_FREQUENCIES_C4[NOTE_COUNT] = {
    261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f};

struct StereoFrame {
  int16_t left;
  int16_t right;
};
static_assert(sizeof(StereoFrame) == 4, "Unexpected stereo frame packing");

struct SharedCalibrationState {
  uint8_t selectedPitch;
  uint8_t pressedNoteMask;
  float attenuationDb;
  uint32_t revision;
};

I2SClass i2s;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE,
    MINI_SYNTH_PIN_I2C_SCL,
    MINI_SYNTH_PIN_I2C_SDA);

StereoFrame audioFrames[AUDIO_FRAMES_PER_BLOCK];
int16_t sineTable[SINE_TABLE_SIZE];
uint32_t phaseSteps[PITCH_COUNT];
uint32_t phase = 0;
float envelope = 0.0f;
float attenuationDb[PITCH_COUNT] = {};
uint8_t debounceCounters[INPUT_COUNT] = {};
bool debouncedPressed[INPUT_COUNT] = {};
bool previousPressed[INPUT_COUNT] = {};
uint32_t modifierPressedAt[2] = {};
bool modifierLongHandled[2] = {};

portMUX_TYPE calibrationMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t sharedSelectedPitch = NOTE_COUNT;  // Middle C / do.
volatile uint8_t sharedPressedNoteMask = 0;
volatile uint32_t sharedRevision = 0;

bool i2sReady = false;
bool displayReady = false;
volatile bool audioTaskHealthy = false;
volatile bool audioWriteFailed = false;
uint32_t lastUiRefreshAt = 0;
uint32_t lastReportedRevision = 0;

uint8_t octaveNumberFromRange(uint8_t rangeIndex) {
  return static_cast<uint8_t>(rangeIndex + 3);
}

float octaveMultiplier(uint8_t rangeIndex) {
  if (rangeIndex == 0) {
    return 0.5f;
  }
  if (rangeIndex == 2) {
    return 2.0f;
  }
  return 1.0f;
}

uint8_t pitchIndex(uint8_t rangeIndex, uint8_t noteIndex) {
  return static_cast<uint8_t>(rangeIndex * NOTE_COUNT + noteIndex);
}

uint8_t rangeFromPitch(uint8_t selectedPitch) {
  return static_cast<uint8_t>(selectedPitch / NOTE_COUNT);
}

uint8_t noteFromPitch(uint8_t selectedPitch) {
  return static_cast<uint8_t>(selectedPitch % NOTE_COUNT);
}

float dbToLinear(float db) {
  return powf(10.0f, db / 20.0f);
}

void initializeSynthesisTables() {
  for (uint16_t i = 0; i < SINE_TABLE_SIZE; ++i) {
    const float radians =
        6.28318530718f * static_cast<float>(i) /
        static_cast<float>(SINE_TABLE_SIZE);
    sineTable[i] = static_cast<int16_t>(sinf(radians) * 32767.0f);
  }

  for (uint8_t rangeIndex = 0; rangeIndex < OCTAVE_COUNT; ++rangeIndex) {
    const float multiplier = octaveMultiplier(rangeIndex);
    for (uint8_t noteIndex = 0; noteIndex < NOTE_COUNT; ++noteIndex) {
      const uint8_t pitch = pitchIndex(rangeIndex, noteIndex);
      const float frequency = NOTE_FREQUENCIES_C4[noteIndex] * multiplier;
      phaseSteps[pitch] = static_cast<uint32_t>(
          frequency * PHASE_SCALE / SAMPLE_RATE_HZ);
      attenuationDb[pitch] = 0.0f;
    }
  }
}

SharedCalibrationState readCalibrationState() {
  SharedCalibrationState snapshot;
  portENTER_CRITICAL(&calibrationMux);
  snapshot.selectedPitch = sharedSelectedPitch;
  snapshot.pressedNoteMask = sharedPressedNoteMask;
  snapshot.attenuationDb = attenuationDb[sharedSelectedPitch];
  snapshot.revision = sharedRevision;
  portEXIT_CRITICAL(&calibrationMux);
  return snapshot;
}

void selectPitch(uint8_t pitch) {
  portENTER_CRITICAL(&calibrationMux);
  sharedSelectedPitch = pitch;
  ++sharedRevision;
  portEXIT_CRITICAL(&calibrationMux);
}

void publishPressedNoteMask(uint8_t mask) {
  portENTER_CRITICAL(&calibrationMux);
  sharedPressedNoteMask = mask;
  portEXIT_CRITICAL(&calibrationMux);
}

void adjustSelectedLevel(float deltaDb) {
  portENTER_CRITICAL(&calibrationMux);
  float newValue = attenuationDb[sharedSelectedPitch] + deltaDb;
  if (newValue > MAX_ATTENUATION_DB) {
    newValue = MAX_ATTENUATION_DB;
  }
  if (newValue < MIN_ATTENUATION_DB) {
    newValue = MIN_ATTENUATION_DB;
  }
  attenuationDb[sharedSelectedPitch] = newValue;
  ++sharedRevision;
  portEXIT_CRITICAL(&calibrationMux);
}

void cycleRange() {
  portENTER_CRITICAL(&calibrationMux);
  const uint8_t note = noteFromPitch(sharedSelectedPitch);
  const uint8_t nextRange =
      static_cast<uint8_t>((rangeFromPitch(sharedSelectedPitch) + 1) % OCTAVE_COUNT);
  sharedSelectedPitch = pitchIndex(nextRange, note);
  sharedPressedNoteMask = 0;
  ++sharedRevision;
  portEXIT_CRITICAL(&calibrationMux);
}

void printCalibrationTable() {
  float snapshot[PITCH_COUNT];
  portENTER_CRITICAL(&calibrationMux);
  for (uint8_t i = 0; i < PITCH_COUNT; ++i) {
    snapshot[i] = attenuationDb[i];
  }
  portEXIT_CRITICAL(&calibrationMux);

  Serial.println("CALIBRATION_TABLE_BEGIN");
  Serial.printf("CALIBRATION_MASTER_PEAK,%d\n", CALIBRATION_MASTER_PEAK);
  Serial.println("constexpr float NOTE_ATTENUATION_DB[3][7] = {");
  for (uint8_t rangeIndex = 0; rangeIndex < OCTAVE_COUNT; ++rangeIndex) {
    Serial.printf("%s", "  {");
    for (uint8_t noteIndex = 0; noteIndex < NOTE_COUNT; ++noteIndex) {
      const uint8_t pitch = pitchIndex(rangeIndex, noteIndex);
      Serial.printf("%.1ff%s",
                    snapshot[pitch],
                    noteIndex + 1 == NOTE_COUNT ? "" : ", ");
    }
    Serial.printf("}%s  // %s C%u-B%u\n",
                  rangeIndex + 1 == OCTAVE_COUNT ? "" : ",",
                  RANGE_NAMES[rangeIndex],
                  octaveNumberFromRange(rangeIndex),
                  octaveNumberFromRange(rangeIndex));
  }
  Serial.println("};");
  Serial.println("CALIBRATION_TABLE_END");
}

void handleModifier(uint8_t modifierIndex,
                    uint8_t inputIndex,
                    float shortPressDeltaDb,
                    bool longPressCyclesRange) {
  const bool pressed = debouncedPressed[inputIndex];
  const bool wasPressed = previousPressed[inputIndex];
  const uint32_t now = millis();

  if (pressed && !wasPressed) {
    modifierPressedAt[modifierIndex] = now;
    modifierLongHandled[modifierIndex] = false;
  }

  if (pressed && !modifierLongHandled[modifierIndex] &&
      now - modifierPressedAt[modifierIndex] >= LONG_PRESS_MS) {
    modifierLongHandled[modifierIndex] = true;
    if (longPressCyclesRange) {
      cycleRange();
    } else {
      printCalibrationTable();
    }
  }

  if (!pressed && wasPressed && !modifierLongHandled[modifierIndex]) {
    adjustSelectedLevel(shortPressDeltaDb);
  }
}

void inputScanTask(void *) {
  TickType_t lastWake = xTaskGetTickCount();

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

    const uint8_t selectedRange = rangeFromPitch(readCalibrationState().selectedPitch);
    for (uint8_t noteIndex = 0; noteIndex < NOTE_COUNT; ++noteIndex) {
      const bool pressedEdge =
          debouncedPressed[noteIndex] && !previousPressed[noteIndex];
      if (pressedEdge) {
        selectPitch(pitchIndex(selectedRange, noteIndex));
      }
    }

    publishPressedNoteMask(noteMask);

    handleModifier(0, NOTE_COUNT, -ATTENUATION_STEP_DB, true);     // SW8.
    handleModifier(1, NOTE_COUNT + 1, ATTENUATION_STEP_DB, false); // SW9.

    for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
      previousPressed[i] = debouncedPressed[i];
    }

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
  const float levelFadeStep = 1.0f / (LEVEL_CROSSFADE_SECONDS * SAMPLE_RATE_HZ);
  uint8_t previousSelectedPitch = PITCH_COUNT;
  float currentGain = 1.0f;

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
    const SharedCalibrationState state = readCalibrationState();
    const uint8_t selectedNote = noteFromPitch(state.selectedPitch);
    const bool selectedNotePressed =
        (state.pressedNoteMask & static_cast<uint8_t>(1U << selectedNote)) != 0;
    const float targetGain = dbToLinear(state.attenuationDb);

    if (state.selectedPitch != previousSelectedPitch) {
      phase = 0;
      envelope = 0.0f;
      previousSelectedPitch = state.selectedPitch;
    }

    for (size_t frame = 0; frame < AUDIO_FRAMES_PER_BLOCK; ++frame) {
      if (currentGain < targetGain) {
        currentGain += levelFadeStep;
        if (currentGain > targetGain) {
          currentGain = targetGain;
        }
      } else if (currentGain > targetGain) {
        currentGain -= levelFadeStep;
        if (currentGain < targetGain) {
          currentGain = targetGain;
        }
      }

      if (selectedNotePressed) {
        envelope += attackStep;
        if (envelope > 1.0f) {
          envelope = 1.0f;
        }
      } else {
        envelope -= releaseStep;
        if (envelope < 0.0f) {
          envelope = 0.0f;
        }
      }

      const uint8_t tableIndex = static_cast<uint8_t>(phase >> 24);
      const float normalizedSample =
          static_cast<float>(sineTable[tableIndex]) / 32767.0f;
      const int16_t sample = static_cast<int16_t>(
          normalizedSample * envelope * currentGain * CALIBRATION_MASTER_PEAK);
      phase += phaseSteps[state.selectedPitch];

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

void updateDisplay(const SharedCalibrationState &state) {
  if (!displayReady) {
    return;
  }

  const uint8_t rangeIndex = rangeFromPitch(state.selectedPitch);
  const uint8_t noteIndex = noteFromPitch(state.selectedPitch);

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, "PURE CAL");
  const int16_t rangeWidth = display.getStrWidth(RANGE_NAMES[rangeIndex]);
  display.drawStr(126 - rangeWidth, 11, RANGE_NAMES[rangeIndex]);
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  char noteLabel[16];
  snprintf(noteLabel, sizeof(noteLabel), "%s / %s%u",
           SOLFEGE_NAMES[noteIndex],
           NOTE_NAMES[noteIndex],
           octaveNumberFromRange(rangeIndex));
  display.setFont(u8g2_font_ncenB12_tr);
  drawCentered(noteLabel, 37);

  char levelLabel[20];
  snprintf(levelLabel, sizeof(levelLabel), "LEVEL %+.1f dB", state.attenuationDb);
  display.setFont(u8g2_font_6x10_tf);
  drawCentered(levelLabel, 52);
  display.drawStr(1, 63, "SW8-1  SW9+1");

  display.sendBuffer();
}

void reportStateChange(const SharedCalibrationState &state) {
  if (state.revision == lastReportedRevision) {
    return;
  }

  const uint8_t rangeIndex = rangeFromPitch(state.selectedPitch);
  const uint8_t noteIndex = noteFromPitch(state.selectedPitch);
  Serial.printf("CAL,%s,%s%u,ATTENUATION_DB,%.1f,LINEAR_GAIN,%.4f\n",
                RANGE_NAMES[rangeIndex],
                NOTE_NAMES[noteIndex],
                octaveNumberFromRange(rangeIndex),
                state.attenuationDb,
                dbToLinear(state.attenuationDb));
  lastReportedRevision = state.revision;
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

  Serial.println();
  Serial.println("PURE_TONE_LEVEL_CALIBRATOR_BEGIN");
  Serial.printf("TEST,I2S_INIT,%s\n", i2sReady ? "PASS" : "FAIL");
  Serial.printf("INFO,CALIBRATION_MASTER_PEAK,%d\n", CALIBRATION_MASTER_PEAK);
  Serial.println("INFO,NOTE_KEY,SELECT_AND_HOLD_TO_LISTEN");
  Serial.println("INFO,SW8,SHORT_MINUS_1DB,LONG_CYCLE_RANGE");
  Serial.println("INFO,SW9,SHORT_PLUS_1DB,LONG_PRINT_TABLE");

  if (!i2sReady) {
    updateDisplay(readCalibrationState());
    return;
  }

  const BaseType_t inputTaskResult = xTaskCreate(
      inputScanTask, "cal-input", 4096, nullptr, 4, nullptr);
  const BaseType_t audioTaskResult = xTaskCreate(
      audioRenderTask, "cal-audio", 4096, nullptr, 3, nullptr);
  Serial.printf("TEST,INPUT_TASK,%s\n",
                inputTaskResult == pdPASS ? "PASS" : "FAIL");
  Serial.printf("TEST,AUDIO_TASK,%s\n",
                audioTaskResult == pdPASS ? "PASS" : "FAIL");

  if (inputTaskResult != pdPASS || audioTaskResult != pdPASS) {
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  }

  updateDisplay(readCalibrationState());
  lastUiRefreshAt = millis();
}

void loop() {
  const SharedCalibrationState state = readCalibrationState();
  reportStateChange(state);

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
