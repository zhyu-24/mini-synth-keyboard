#include <MiniSynthPins.h>

struct KeyState {
  const char *name;
  uint8_t pin;
  int stableState;
  int lastSample;
  uint32_t sampleChangedAt;
};

KeyState keys[] = {
  {"DO", MINI_SYNTH_PIN_KEY_DO, HIGH, HIGH, 0},
  {"RE", MINI_SYNTH_PIN_KEY_RE, HIGH, HIGH, 0},
  {"MI", MINI_SYNTH_PIN_KEY_MI, HIGH, HIGH, 0},
  {"FA", MINI_SYNTH_PIN_KEY_FA, HIGH, HIGH, 0},
  {"SOL", MINI_SYNTH_PIN_KEY_SOL, HIGH, HIGH, 0},
  {"LA", MINI_SYNTH_PIN_KEY_LA, HIGH, HIGH, 0},
  {"TI", MINI_SYNTH_PIN_KEY_TI, HIGH, HIGH, 0},
  {"PLAY_STOP", MINI_SYNTH_PIN_KEY_PLAY_STOP, HIGH, HIGH, 0},
  {"FN", MINI_SYNTH_PIN_KEY_FN, HIGH, HIGH, 0},
};

constexpr size_t KEY_COUNT = sizeof(keys) / sizeof(keys[0]);
constexpr uint32_t DEBOUNCE_MS = 15;

void setup() {
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  for (size_t i = 0; i < KEY_COUNT; ++i) {
    pinMode(keys[i].pin, MINI_SYNTH_KEY_PIN_MODE);
    const int state = digitalRead(keys[i].pin);
    keys[i].stableState = state;
    keys[i].lastSample = state;
    keys[i].sampleChangedAt = millis();
  }

  Serial.begin(115200);
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println("KEYTEST_BEGIN");
  Serial.println("INFO,KEYS,9");
  Serial.println("INFO,ACTIVE_LEVEL,LOW");
  Serial.println("Press and release each key. Use Ctrl+C in the monitor when finished.");
}

void loop() {
  const uint32_t now = millis();

  for (size_t i = 0; i < KEY_COUNT; ++i) {
    const int sample = digitalRead(keys[i].pin);

    if (sample != keys[i].lastSample) {
      keys[i].lastSample = sample;
      keys[i].sampleChangedAt = now;
    }

    if (sample != keys[i].stableState && now - keys[i].sampleChangedAt >= DEBOUNCE_MS) {
      keys[i].stableState = sample;
      Serial.printf("KEY,%s,%s,GPIO,%u\n",
                    keys[i].name,
                    sample == MINI_SYNTH_KEY_ACTIVE_LEVEL ? "PRESSED" : "RELEASED",
                    keys[i].pin);
    }
  }

  delay(1);
}
