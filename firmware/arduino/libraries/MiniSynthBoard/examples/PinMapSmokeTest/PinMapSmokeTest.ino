#include <MiniSynthPins.h>

struct KeyDefinition {
  const char *name;
  uint8_t pin;
  int lastState;
};

KeyDefinition keys[] = {
  {"do", MINI_SYNTH_PIN_KEY_DO, HIGH},
  {"re", MINI_SYNTH_PIN_KEY_RE, HIGH},
  {"mi", MINI_SYNTH_PIN_KEY_MI, HIGH},
  {"fa", MINI_SYNTH_PIN_KEY_FA, HIGH},
  {"sol", MINI_SYNTH_PIN_KEY_SOL, HIGH},
  {"la", MINI_SYNTH_PIN_KEY_LA, HIGH},
  {"ti", MINI_SYNTH_PIN_KEY_TI, HIGH},
  {"play/stop", MINI_SYNTH_PIN_KEY_PLAY_STOP, HIGH},
  {"fn", MINI_SYNTH_PIN_KEY_FN, HIGH},
};

constexpr size_t KEY_COUNT = sizeof(keys) / sizeof(keys[0]);

void setup() {
  // Hardware-safe startup: keep the NS4168 shut down before doing anything else.
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  for (size_t i = 0; i < KEY_COUNT; ++i) {
    pinMode(keys[i].pin, MINI_SYNTH_KEY_PIN_MODE);
    keys[i].lastState = digitalRead(keys[i].pin);
  }

  Serial.begin(115200);
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println("=== Mini Synth pin-map smoke test ===");
  Serial.printf("Board: %s\n", MINI_SYNTH_BOARD_NAME);
  Serial.printf("Chip: %s, revision %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("Flash: %u bytes (expected %u)\n",
                ESP.getFlashChipSize(), MINI_SYNTH_EXPECTED_FLASH_BYTES);
  Serial.printf("PSRAM: %u bytes (expected %u)\n",
                ESP.getPsramSize(), MINI_SYNTH_EXPECTED_PSRAM_BYTES);
  Serial.println("Press each key; state changes will be printed.");
}

void loop() {
  for (size_t i = 0; i < KEY_COUNT; ++i) {
    const int state = digitalRead(keys[i].pin);
    if (state != keys[i].lastState) {
      delay(15);  // Simple debounce for a board bring-up test.
      const int confirmedState = digitalRead(keys[i].pin);
      if (confirmedState != keys[i].lastState) {
        keys[i].lastState = confirmedState;
        Serial.printf("%s: %s\n", keys[i].name,
                      confirmedState == MINI_SYNTH_KEY_ACTIVE_LEVEL
                          ? "pressed"
                          : "released");
      }
    }
  }
  delay(1);
}
