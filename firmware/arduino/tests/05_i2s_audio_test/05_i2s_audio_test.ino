#include <ESP_I2S.h>
#include <MiniSynthPins.h>
#include <math.h>

namespace {
constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr float TONE_FREQUENCY_HZ = 660.0f;
constexpr int16_t TONE_PEAK = 1200;  // About 3.7% of full scale: intentionally quiet.
constexpr uint32_t TONE_DURATION_MS = 350;
constexpr uint32_t FADE_DURATION_MS = 35;
constexpr uint32_t SILENCE_DURATION_MS = 30;
constexpr size_t FRAMES_PER_BLOCK = 128;
constexpr float PHASE_CYCLE_RADIANS = 6.28318530718f;

struct StereoFrame {
  int16_t left;
  int16_t right;
};
static_assert(sizeof(StereoFrame) == 4, "Unexpected stereo frame packing");

I2SClass i2s;
StereoFrame frames[FRAMES_PER_BLOCK];
bool i2sReady = false;

bool writeFrames(const StereoFrame *data, size_t frameCount) {
  const size_t byteCount = frameCount * sizeof(StereoFrame);
  const size_t written = i2s.write(data, byteCount);
  if (written != byteCount) {
    Serial.printf("AUDIO_ERROR,SHORT_WRITE,%u,%u\n",
                  static_cast<unsigned>(written),
                  static_cast<unsigned>(byteCount));
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
    return false;
  }
  return true;
}

bool writeSilence(uint32_t durationMs) {
  for (size_t i = 0; i < FRAMES_PER_BLOCK; ++i) {
    frames[i] = {0, 0};
  }

  uint32_t remaining = (SAMPLE_RATE_HZ * durationMs) / 1000;
  while (remaining > 0) {
    const size_t count = remaining > FRAMES_PER_BLOCK ? FRAMES_PER_BLOCK : remaining;
    if (!writeFrames(frames, count)) {
      return false;
    }
    remaining -= count;
  }
  return true;
}

void playTestTone(bool bothChannels) {
  if (!i2sReady) {
    Serial.println("AUDIO_ERROR,I2S_NOT_READY");
    return;
  }

  const char *modeName = bothChannels ? "BOTH_REFERENCE" : "RIGHT_ONLY";
  Serial.printf("AUDIO_BEGIN,%s,%uHZ,PEAK,%d\n",
                modeName,
                static_cast<unsigned>(TONE_FREQUENCY_HZ),
                TONE_PEAK);

  // Establish valid clocks and silence before enabling the NS4168.
  if (!writeSilence(SILENCE_DURATION_MS)) {
    return;
  }
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_ENABLE_LEVEL);
  delay(10);

  const uint32_t totalFrames = (SAMPLE_RATE_HZ * TONE_DURATION_MS) / 1000;
  const uint32_t fadeFrames = (SAMPLE_RATE_HZ * FADE_DURATION_MS) / 1000;
  float phase = 0.0f;
  const float phaseStep = PHASE_CYCLE_RADIANS * TONE_FREQUENCY_HZ / SAMPLE_RATE_HZ;
  uint32_t generated = 0;
  bool writeOk = true;

  while (generated < totalFrames) {
    const size_t count =
        (totalFrames - generated) > FRAMES_PER_BLOCK
            ? FRAMES_PER_BLOCK
            : (totalFrames - generated);

    for (size_t i = 0; i < count; ++i) {
      const uint32_t frameIndex = generated + i;
      float envelope = 1.0f;
      if (frameIndex < fadeFrames) {
        envelope = static_cast<float>(frameIndex) / fadeFrames;
      } else if (frameIndex >= totalFrames - fadeFrames) {
        envelope = static_cast<float>(totalFrames - 1 - frameIndex) / fadeFrames;
      }
      if (envelope < 0.0f) {
        envelope = 0.0f;
      }

      const int16_t sample =
          static_cast<int16_t>(sinf(phase) * TONE_PEAK * envelope);
      phase += phaseStep;
      if (phase >= PHASE_CYCLE_RADIANS) {
        phase -= PHASE_CYCLE_RADIANS;
      }

      // Standard I2S frame order is left, then right. The NS4168 CTRL HIGH
      // setting used by this PCB selects the right-channel wire slot.
      frames[i].left = bothChannels ? sample : 0;
      frames[i].right = sample;
    }

    if (!writeFrames(frames, count)) {
      writeOk = false;
      break;
    }
    generated += count;
  }

  // Queue silence before shutting the amplifier down to reduce clicks.
  if (writeOk) {
    writeSilence(SILENCE_DURATION_MS);
  }
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  Serial.printf("AUDIO_END,%s,%s\n", modeName, writeOk ? "PASS" : "FAIL");
}

bool confirmedPress(uint8_t pin) {
  if (digitalRead(pin) != MINI_SYNTH_KEY_ACTIVE_LEVEL) {
    return false;
  }
  delay(20);
  if (digitalRead(pin) != MINI_SYNTH_KEY_ACTIVE_LEVEL) {
    return false;
  }
  return true;
}

void waitForRelease(uint8_t pin) {
  while (digitalRead(pin) == MINI_SYNTH_KEY_ACTIVE_LEVEL) {
    delay(5);
  }
  delay(20);
}
}  // namespace

void setup() {
  // Safety first: never make sound automatically at boot.
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  pinMode(MINI_SYNTH_PIN_KEY_PLAY_STOP, MINI_SYNTH_KEY_PIN_MODE);
  pinMode(MINI_SYNTH_PIN_KEY_FN, MINI_SYNTH_KEY_PIN_MODE);

  Serial.begin(115200);
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }

  i2s.setPins(MINI_SYNTH_PIN_I2S_BCLK,
              MINI_SYNTH_PIN_I2S_LRCLK,
              MINI_SYNTH_PIN_I2S_DOUT);
  i2sReady = i2s.begin(I2S_MODE_STD,
                       SAMPLE_RATE_HZ,
                       I2S_DATA_BIT_WIDTH_16BIT,
                       I2S_SLOT_MODE_STEREO);

  Serial.println();
  Serial.println("AUDIOTEST_BEGIN");
  Serial.printf("INFO,I2S_PINS,BCLK,%d,LRCLK,%d,SDATA,%d\n",
                MINI_SYNTH_PIN_I2S_BCLK,
                MINI_SYNTH_PIN_I2S_LRCLK,
                MINI_SYNTH_PIN_I2S_DOUT);
  Serial.printf("INFO,FORMAT,%uHZ,16BIT,STEREO\n", SAMPLE_RATE_HZ);
  Serial.println("INFO,AMP,DISABLED_AT_IDLE");
  Serial.printf("TEST,I2S_INIT,%s\n", i2sReady ? "PASS" : "FAIL");

  if (i2sReady) {
    writeSilence(SILENCE_DURATION_MS);
    Serial.println("READY,PLAY_STOP=RIGHT_ONLY,FN=BOTH_REFERENCE");
  } else {
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  }
}

void loop() {
  if (!i2sReady) {
    delay(1000);
    return;
  }

  if (confirmedPress(MINI_SYNTH_PIN_KEY_PLAY_STOP)) {
    playTestTone(false);
    waitForRelease(MINI_SYNTH_PIN_KEY_PLAY_STOP);
  } else if (confirmedPress(MINI_SYNTH_PIN_KEY_FN)) {
    playTestTone(true);
    waitForRelease(MINI_SYNTH_PIN_KEY_FN);
  }

  delay(2);
}
