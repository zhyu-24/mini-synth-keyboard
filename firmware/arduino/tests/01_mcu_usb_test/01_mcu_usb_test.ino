#include <MiniSynthPins.h>
#include <esp_system.h>

static const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

static void printTest(const char *name, bool pass, uint32_t value) {
  Serial.printf("TEST,%s,%s,%lu\n", name, pass ? "PASS" : "FAIL",
                static_cast<unsigned long>(value));
}

void setup() {
  // Safety first: keep the NS4168 shut down throughout this test.
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  Serial.begin(115200);
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }

  const uint32_t flashBytes = ESP.getFlashChipSize();
  const uint32_t psramBytes = ESP.getPsramSize();
  const bool chipPass = String(ESP.getChipModel()).indexOf("ESP32-S3") >= 0;
  const bool flashPass = flashBytes == MINI_SYNTH_EXPECTED_FLASH_BYTES;
  const bool psramPass = psramBytes == MINI_SYNTH_EXPECTED_PSRAM_BYTES;
  const bool allPass = chipPass && flashPass && psramPass;

  Serial.println();
  Serial.println("SELFTEST_BEGIN");
  Serial.printf("INFO,BOARD,%s\n", MINI_SYNTH_BOARD_NAME);
  Serial.printf("INFO,CHIP,%s\n", ESP.getChipModel());
  Serial.printf("INFO,REVISION,%d\n", ESP.getChipRevision());
  Serial.printf("INFO,RESET_REASON,%s\n", resetReasonName(esp_reset_reason()));
  Serial.printf("TEST,CHIP,%s,%s\n", chipPass ? "PASS" : "FAIL", ESP.getChipModel());
  printTest("FLASH", flashPass, flashBytes);
  printTest("PSRAM", psramPass, psramBytes);
  Serial.printf("TEST,AMP_SAFE,%s,%d\n",
                digitalRead(MINI_SYNTH_PIN_AMP_CTRL) == MINI_SYNTH_AMP_DISABLE_LEVEL
                    ? "PASS"
                    : "FAIL",
                digitalRead(MINI_SYNTH_PIN_AMP_CTRL));
  Serial.printf("SELFTEST_END,%s\n", allPass ? "PASS" : "FAIL");
}

void loop() {
  static uint32_t heartbeat = 0;
  Serial.printf("HEARTBEAT,%lu,FREE_HEAP,%lu,FREE_PSRAM,%lu\n",
                static_cast<unsigned long>(heartbeat++),
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getFreePsram()));
  delay(1000);
}
