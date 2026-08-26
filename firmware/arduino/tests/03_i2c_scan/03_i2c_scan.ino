#include <MiniSynthPins.h>
#include <Wire.h>

void scanI2c() {
  uint8_t found = 0;
  Serial.println("I2C_SCAN_BEGIN");

  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();

    if (error == 0) {
      Serial.printf("I2C_DEVICE,0x%02X\n", address);
      ++found;
    } else if (error == 4) {
      Serial.printf("I2C_ERROR,UNKNOWN,0x%02X\n", address);
    }
  }

  Serial.printf("I2C_SCAN_END,%s,COUNT,%u\n", found > 0 ? "PASS" : "FAIL", found);
}

void setup() {
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  Serial.begin(115200);
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }

  Wire.begin(MINI_SYNTH_PIN_I2C_SDA, MINI_SYNTH_PIN_I2C_SCL);
  Wire.setClock(100000);

  Serial.println();
  Serial.printf("INFO,I2C_PINS,SDA,%d,SCL,%d\n",
                MINI_SYNTH_PIN_I2C_SDA, MINI_SYNTH_PIN_I2C_SCL);
  scanI2c();
}

void loop() {
  delay(5000);
  scanI2c();
}
