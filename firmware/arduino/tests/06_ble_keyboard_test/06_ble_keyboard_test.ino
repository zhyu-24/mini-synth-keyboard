#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <MiniSynthPins.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

namespace {

constexpr char DEVICE_NAME[] = "MiniSynth GamePad";
constexpr char MANUFACTURER_NAME[] = "Tsinghua GRID";
constexpr uint8_t HID_REPORT_ID = 1;
constexpr uint8_t KEY_COUNT = 9;
constexpr uint8_t DEBOUNCE_TICKS = 5;       // 5 stable samples at 1 kHz.
constexpr uint16_t INPUT_PERIOD_MS = 1;
constexpr uint16_t HOME_HOLD_MS = 1000;
constexpr uint16_t UI_MIN_REFRESH_MS = 20;
constexpr uint16_t UI_PERIODIC_REFRESH_MS = 250;
constexpr UBaseType_t REPORT_QUEUE_DEPTH = 16;
constexpr uint8_t OLED_ADDRESS_7BIT = 0x3C;
constexpr uint8_t OLED_YELLOW_ROWS = 16;
constexpr uint8_t LEFT_ALT_MODIFIER = 0x04;
constexpr uint16_t VALID_PHYSICAL_MASK = 0x01FF;

// Physical mask order follows ALL_KEY_PINS below:
// bit 0..6 = SW1..SW7, bit 7 = SW8, bit 8 = SW9.
constexpr uint16_t SW8_MASK = 1U << 7;
constexpr uint16_t SW9_MASK = 1U << 8;
constexpr uint16_t HOME_CHORD_MASK = SW8_MASK | SW9_MASK;

const uint8_t ALL_KEY_PINS[KEY_COUNT] = {
    MINI_SYNTH_PIN_KEY_DO,         // SW1 -> Q
    MINI_SYNTH_PIN_KEY_RE,         // SW2 -> W
    MINI_SYNTH_PIN_KEY_MI,         // SW3 -> E
    MINI_SYNTH_PIN_KEY_FA,         // SW4 -> A
    MINI_SYNTH_PIN_KEY_SOL,        // SW5 -> S
    MINI_SYNTH_PIN_KEY_LA,         // SW6 -> D
    MINI_SYNTH_PIN_KEY_TI,         // SW7 -> F
    MINI_SYNTH_PIN_KEY_PLAY_STOP,  // SW8 -> R
    MINI_SYNTH_PIN_KEY_FN,         // SW9 -> Left Alt
};

// This is a deliberately small NKRO keyboard descriptor. Instead of the
// standard six-key array, it publishes one independent bit for each of the
// eight game keys, plus the standard eight modifier bits. All eight letters
// and Left Alt can therefore be held at the same time.
const uint8_t HID_REPORT_DESCRIPTOR[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, HID_REPORT_ID,  //   Report ID (1)

    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8 modifiers)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // Eight variable bits, in this exact order:
    // Q, W, E, R, A, S, D, F.
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x09, 0x14,        //   Usage (Q)
    0x09, 0x1A,        //   Usage (W)
    0x09, 0x08,        //   Usage (E)
    0x09, 0x15,        //   Usage (R)
    0x09, 0x04,        //   Usage (A)
    0x09, 0x16,        //   Usage (S)
    0x09, 0x07,        //   Usage (D)
    0x09, 0x09,        //   Usage (F)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8 keys)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0,              // End Collection
};

struct KeyboardNkroReport {
  uint8_t modifiers;
  uint8_t gameKeys;
} __attribute__((packed));
static_assert(sizeof(KeyboardNkroReport) == 2,
              "Unexpected BLE keyboard report size");

struct ReportRequest {
  uint32_t epoch;
  uint16_t physicalMask;
};

struct TransportSnapshot {
  bool connected;
  bool releaseGate;
  bool needsInitialZero;
  bool restartAdvertising;
  uint32_t epoch;
};

enum class BleUiState : uint8_t {
  Starting,
  Advertising,
  Connected,
  Lost,
  Error,
};

U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE,
    MINI_SYNTH_PIN_I2C_SCL,
    MINI_SYNTH_PIN_I2C_SDA);

BLEHIDDevice *hidDevice = nullptr;
BLECharacteristic *inputReport = nullptr;
QueueHandle_t reportQueue = nullptr;
portMUX_TYPE sharedMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool sharedConnected = false;
volatile bool sharedReleaseGate = true;
volatile bool sharedNeedsInitialZero = false;
volatile bool sharedRestartAdvertising = false;
volatile uint32_t sharedTransportEpoch = 1;
volatile uint16_t sharedPhysicalMask = 0;
volatile BleUiState sharedBleUiState = BleUiState::Starting;

uint8_t debounceCounters[KEY_COUNT] = {};
bool debouncedPressed[KEY_COUNT] = {};
bool homeChordTracking = false;
bool homeChordTriggered = false;
uint32_t homeChordStartedAt = 0;

bool displayReady = false;
uint32_t lastUiRefreshAt = 0;
uint16_t lastDisplayedPhysicalMask = 0xFFFF;
BleUiState lastDisplayedBleUiState = BleUiState::Error;
bool lastDisplayedReleaseGate = false;

KeyboardNkroReport buildKeyboardReport(uint16_t physicalMask) {
  physicalMask &= VALID_PHYSICAL_MASK;

  KeyboardNkroReport report = {};
  const bool leftAltPressed = (physicalMask & SW9_MASK) != 0;
  if (leftAltPressed) {
    report.modifiers |= LEFT_ALT_MODIFIER;
    // SW8+SW9 is the global Home gesture. Suppress R for the full time that
    // Left Alt is held, including the first second before Home triggers, so
    // entering Home cannot accidentally activate the game's R action.
    physicalMask &= static_cast<uint16_t>(~SW8_MASK);
  }

  // Report bit order is Q, W, E, R, A, S, D, F.
  if ((physicalMask & (1U << 0)) != 0) report.gameKeys |= 1U << 0;
  if ((physicalMask & (1U << 1)) != 0) report.gameKeys |= 1U << 1;
  if ((physicalMask & (1U << 2)) != 0) report.gameKeys |= 1U << 2;
  if ((physicalMask & (1U << 7)) != 0) report.gameKeys |= 1U << 3;
  if ((physicalMask & (1U << 3)) != 0) report.gameKeys |= 1U << 4;
  if ((physicalMask & (1U << 4)) != 0) report.gameKeys |= 1U << 5;
  if ((physicalMask & (1U << 5)) != 0) report.gameKeys |= 1U << 6;
  if ((physicalMask & (1U << 6)) != 0) report.gameKeys |= 1U << 7;
  return report;
}

TransportSnapshot readTransportSnapshot() {
  TransportSnapshot snapshot;
  portENTER_CRITICAL(&sharedMux);
  snapshot.connected = sharedConnected;
  snapshot.releaseGate = sharedReleaseGate;
  snapshot.needsInitialZero = sharedNeedsInitialZero;
  snapshot.restartAdvertising = sharedRestartAdvertising;
  snapshot.epoch = sharedTransportEpoch;
  portEXIT_CRITICAL(&sharedMux);
  return snapshot;
}

void setBleUiState(BleUiState state) {
  portENTER_CRITICAL(&sharedMux);
  sharedBleUiState = state;
  portEXIT_CRITICAL(&sharedMux);
}

void publishPhysicalMask(uint16_t physicalMask) {
  portENTER_CRITICAL(&sharedMux);
  sharedPhysicalMask = physicalMask & VALID_PHYSICAL_MASK;
  portEXIT_CRITICAL(&sharedMux);
}

void queueMaskTransition(uint16_t physicalMask, bool flushFirst = false) {
  if (reportQueue == nullptr) {
    return;
  }

  ReportRequest request;
  portENTER_CRITICAL(&sharedMux);
  request.epoch = sharedTransportEpoch;
  portEXIT_CRITICAL(&sharedMux);
  request.physicalMask = physicalMask & VALID_PHYSICAL_MASK;

  if (flushFirst) {
    xQueueReset(reportQueue);
  }

  // Preserve ordinary press/release transitions so a short tap is not erased
  // by a newer state before BLE sends it. If the transport is badly stalled,
  // drop the backlog and resynchronize to the newest complete state.
  if (xQueueSend(reportQueue, &request, 0) != pdTRUE) {
    xQueueReset(reportQueue);
    xQueueSend(reportQueue, &request, 0);
  }
}

void engageReleaseGate() {
  portENTER_CRITICAL(&sharedMux);
  sharedReleaseGate = true;
  portEXIT_CRITICAL(&sharedMux);
  queueMaskTransition(0, true);
}

void clearInitialZeroFlag(uint32_t epoch) {
  portENTER_CRITICAL(&sharedMux);
  if (sharedTransportEpoch == epoch) {
    sharedNeedsInitialZero = false;
  }
  portEXIT_CRITICAL(&sharedMux);
}

void clearRestartAdvertisingFlag(uint32_t epoch) {
  portENTER_CRITICAL(&sharedMux);
  if (sharedTransportEpoch == epoch) {
    sharedRestartAdvertising = false;
  }
  portEXIT_CRITICAL(&sharedMux);
}

void clearReleaseGateIfAllReleased(uint16_t physicalMask) {
  if (physicalMask != 0) {
    return;
  }

  bool wasGated = false;
  portENTER_CRITICAL(&sharedMux);
  wasGated = sharedReleaseGate;
  sharedReleaseGate = false;
  portEXIT_CRITICAL(&sharedMux);

  if (wasGated) {
    queueMaskTransition(0);
  }
}

void sendReportNow(uint16_t physicalMask) {
  if (inputReport == nullptr) {
    return;
  }
  const KeyboardNkroReport report = buildKeyboardReport(physicalMask);
  inputReport->setValue(reinterpret_cast<const uint8_t *>(&report),
                        sizeof(report));
  inputReport->notify();
}

class KeyboardServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer *) override {
    portENTER_CRITICAL(&sharedMux);
    sharedTransportEpoch = sharedTransportEpoch + 1U;
    sharedConnected = true;
    sharedReleaseGate = true;
    sharedNeedsInitialZero = true;
    sharedRestartAdvertising = false;
    sharedBleUiState = BleUiState::Connected;
    portEXIT_CRITICAL(&sharedMux);
  }

  void onDisconnect(BLEServer *) override {
    portENTER_CRITICAL(&sharedMux);
    sharedTransportEpoch = sharedTransportEpoch + 1U;
    sharedConnected = false;
    sharedReleaseGate = true;
    sharedNeedsInitialZero = false;
    sharedRestartAdvertising = true;
    sharedBleUiState = BleUiState::Lost;
    portEXIT_CRITICAL(&sharedMux);
  }
};

void inputScanTask(void *) {
  TickType_t lastWake = xTaskGetTickCount();
  uint16_t previousQueuedMask = 0xFFFF;

  for (;;) {
    uint16_t physicalMask = 0;
    for (uint8_t i = 0; i < KEY_COUNT; ++i) {
      const bool rawPressed =
          digitalRead(ALL_KEY_PINS[i]) == MINI_SYNTH_KEY_ACTIVE_LEVEL;

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

      if (debouncedPressed[i]) {
        physicalMask |= static_cast<uint16_t>(1U << i);
      }
    }

    publishPhysicalMask(physicalMask);
    const uint32_t nowMs = millis();
    const bool homeChordPressed =
        (physicalMask & HOME_CHORD_MASK) == HOME_CHORD_MASK;

    if (homeChordPressed) {
      if (!homeChordTracking) {
        homeChordTracking = true;
        homeChordTriggered = false;
        homeChordStartedAt = nowMs;
      } else if (!homeChordTriggered &&
                 nowMs - homeChordStartedAt >= HOME_HOLD_MS) {
        // Standalone-test equivalent of leaving Keyboard: publish an all-up
        // report, then wait until every physical key is released.
        engageReleaseGate();
        homeChordTriggered = true;
        previousQueuedMask = 0;
      }
    } else {
      homeChordTracking = false;
      homeChordTriggered = false;
    }

    clearReleaseGateIfAllReleased(physicalMask);
    const TransportSnapshot transport = readTransportSnapshot();
    const uint16_t desiredMask = transport.releaseGate ? 0 : physicalMask;

    if (transport.connected && desiredMask != previousQueuedMask) {
      queueMaskTransition(desiredMask);
      previousQueuedMask = desiredMask;
    } else if (!transport.connected) {
      previousQueuedMask = 0xFFFF;
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(INPUT_PERIOD_MS));
  }
}

void hidSendTask(void *) {
  ReportRequest request = {};
  uint32_t lastHandledEpoch = 0;

  for (;;) {
    TransportSnapshot transport = readTransportSnapshot();

    if (transport.restartAdvertising && !transport.connected) {
      BLEDevice::startAdvertising();
      clearRestartAdvertisingFlag(transport.epoch);
      setBleUiState(BleUiState::Advertising);
    }

    transport = readTransportSnapshot();
    if (transport.connected && transport.needsInitialZero) {
      sendReportNow(0);
      clearInitialZeroFlag(transport.epoch);
      lastHandledEpoch = transport.epoch;
    }

    if (xQueueReceive(reportQueue, &request, pdMS_TO_TICKS(10)) == pdTRUE) {
      transport = readTransportSnapshot();
      if (!transport.connected || request.epoch != transport.epoch) {
        continue;
      }

      uint16_t maskToSend = request.physicalMask;
      if (transport.releaseGate && maskToSend != 0) {
        maskToSend = 0;
      }
      sendReportNow(maskToSend);
      lastHandledEpoch = transport.epoch;
    }

    // Keep the variable used so warnings remain useful in strict builds.
    (void)lastHandledEpoch;
  }
}

const char *bleStateName(BleUiState state) {
  switch (state) {
    case BleUiState::Starting:
      return "START";
    case BleUiState::Advertising:
      return "PAIR";
    case BleUiState::Connected:
      return "BT OK";
    case BleUiState::Lost:
      return "LOST";
    case BleUiState::Error:
      return "ERROR";
  }
  return "BLE";
}

void drawKeyCell(int16_t x, int16_t y, int16_t width, int16_t height,
                 const char *label, bool pressed) {
  if (pressed) {
    display.drawBox(x, y, width, height);
    display.setDrawColor(0);
  } else {
    display.drawFrame(x, y, width, height);
    display.setDrawColor(1);
  }

  const int16_t labelWidth = display.getStrWidth(label);
  display.drawStr(x + (width - labelWidth) / 2, y + height - 4, label);
  display.setDrawColor(1);
}

void updateDisplay() {
  if (!displayReady) {
    return;
  }

  uint16_t physicalMask;
  BleUiState bleState;
  bool releaseGate;
  portENTER_CRITICAL(&sharedMux);
  physicalMask = sharedPhysicalMask;
  bleState = sharedBleUiState;
  releaseGate = sharedReleaseGate;
  portEXIT_CRITICAL(&sharedMux);

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, "KEYBOARD");
  const char *stateText = bleStateName(bleState);
  const int16_t stateWidth = display.getStrWidth(stateText);
  display.drawStr(126 - stateWidth, 11, stateText);
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  display.setFont(u8g2_font_5x7_tf);
  constexpr int16_t keyWidth = 25;
  constexpr int16_t keyHeight = 15;
  constexpr int16_t gap = 4;
  constexpr int16_t rowX = 3;
  constexpr int16_t topY = 18;
  constexpr int16_t bottomY = 35;

  const char *const topLabels[4] = {"Q", "W", "E", "R"};
  const uint8_t topBits[4] = {0, 1, 2, 7};
  const char *const bottomLabels[4] = {"A", "S", "D", "F"};
  const uint8_t bottomBits[4] = {3, 4, 5, 6};

  for (uint8_t i = 0; i < 4; ++i) {
    const int16_t x = rowX + i * (keyWidth + gap);
    drawKeyCell(x, topY, keyWidth, keyHeight, topLabels[i],
                (physicalMask & (1U << topBits[i])) != 0);
    drawKeyCell(x, bottomY, keyWidth, keyHeight, bottomLabels[i],
                (physicalMask & (1U << bottomBits[i])) != 0);
  }

  drawKeyCell(3, 52, 36, 11, "ALT",
              (physicalMask & SW9_MASK) != 0);
  display.drawStr(44, 61,
                  releaseGate ? "RELEASE ALL" : "SW8+9: RELEASE");
  display.sendBuffer();
}

bool initializeBleKeyboard() {
  if (!BLEDevice::init(DEVICE_NAME)) {
    return false;
  }

  BLESecurity *security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);
  security->setAuthenticationMode(true, false, true);

  BLEServer *server = BLEDevice::createServer();
  if (server == nullptr) {
    return false;
  }
  server->setCallbacks(new KeyboardServerCallbacks());

  hidDevice = new BLEHIDDevice(server);
  if (hidDevice == nullptr) {
    return false;
  }

  inputReport = hidDevice->inputReport(HID_REPORT_ID);
  if (inputReport == nullptr) {
    return false;
  }

  hidDevice->manufacturer()->setValue(MANUFACTURER_NAME);
  hidDevice->pnp(0x02, 0x303A, 0x1001, 0x0100);
  hidDevice->hidInfo(0x00, 0x01);
  hidDevice->reportMap(const_cast<uint8_t *>(HID_REPORT_DESCRIPTOR),
                       sizeof(HID_REPORT_DESCRIPTOR));
  hidDevice->setBatteryLevel(100);
  hidDevice->startServices();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising == nullptr) {
    return false;
  }
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hidDevice->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  setBleUiState(BleUiState::Advertising);
  return true;
}

}  // namespace

void setup() {
  // This isolated BLE test never drives the speaker. Keep the amplifier in
  // hardware shutdown from the first executable lines onward.
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  Serial.begin(115200);

  for (uint8_t i = 0; i < KEY_COUNT; ++i) {
    pinMode(ALL_KEY_PINS[i], MINI_SYNTH_KEY_PIN_MODE);
  }

  Wire.begin(MINI_SYNTH_PIN_I2C_SDA, MINI_SYNTH_PIN_I2C_SCL);
  Wire.setClock(400000);
  display.setI2CAddress(OLED_ADDRESS_7BIT << 1);
  display.setBusClock(400000);
  display.begin();
  display.setContrast(96);
  displayReady = true;

  reportQueue = xQueueCreate(REPORT_QUEUE_DEPTH, sizeof(ReportRequest));
  if (reportQueue == nullptr) {
    setBleUiState(BleUiState::Error);
    updateDisplay();
    return;
  }

  if (!initializeBleKeyboard()) {
    setBleUiState(BleUiState::Error);
    updateDisplay();
    return;
  }

  const BaseType_t inputResult =
      xTaskCreate(inputScanTask, "ble-key-input", 3072, nullptr, 4, nullptr);
  const BaseType_t hidResult =
      xTaskCreate(hidSendTask, "ble-hid-send", 4096, nullptr, 1, nullptr);
  if (inputResult != pdPASS || hidResult != pdPASS) {
    setBleUiState(BleUiState::Error);
  }

  updateDisplay();
  lastUiRefreshAt = millis();
}

void loop() {
  uint16_t physicalMask;
  BleUiState bleState;
  bool releaseGate;
  portENTER_CRITICAL(&sharedMux);
  physicalMask = sharedPhysicalMask;
  bleState = sharedBleUiState;
  releaseGate = sharedReleaseGate;
  portEXIT_CRITICAL(&sharedMux);

  const uint32_t now = millis();
  const bool changed =
      physicalMask != lastDisplayedPhysicalMask ||
      bleState != lastDisplayedBleUiState ||
      releaseGate != lastDisplayedReleaseGate;
  const bool changeRefreshDue =
      changed && now - lastUiRefreshAt >= UI_MIN_REFRESH_MS;
  const bool periodicRefreshDue =
      now - lastUiRefreshAt >= UI_PERIODIC_REFRESH_MS;

  if (changeRefreshDue || periodicRefreshDue) {
    updateDisplay();
    lastUiRefreshAt = now;
    lastDisplayedPhysicalMask = physicalMask;
    lastDisplayedBleUiState = bleState;
    lastDisplayedReleaseGate = releaseGate;
  }

  // Safety invariant: the BLE test must never enable the NS4168.
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  delay(1);
}
