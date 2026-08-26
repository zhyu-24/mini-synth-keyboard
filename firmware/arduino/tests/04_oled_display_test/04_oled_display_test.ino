#include <MiniSynthPins.h>
#include <U8g2lib.h>
#include <Wire.h>

namespace {
constexpr uint8_t OLED_ADDRESS_7BIT = 0x3C;
constexpr uint32_t PAGE_INTERVAL_MS = 3000;

// The physical module is sold as SSD1315. Its vendor-supported Arduino path
// uses the SSD1306-compatible 128x64 U8g2 constructor. The physical display
// test is the final compatibility check.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE,
    MINI_SYNTH_PIN_I2C_SCL,
    MINI_SYNTH_PIN_I2C_SDA);

uint8_t currentPage = 0;
uint32_t pageChangedAt = 0;

void drawGeometryPage() {
  display.clearBuffer();
  display.drawFrame(0, 0, 128, 64);

  // Unique corner markers make rotation and mirroring easy to identify.
  display.drawBox(2, 2, 4, 4);                  // top-left: solid square
  display.drawCircle(123, 4, 3, U8G2_DRAW_ALL); // top-right: circle
  display.drawTriangle(2, 61, 8, 61, 2, 55);    // bottom-left: triangle
  display.drawLine(119, 56, 125, 62);           // bottom-right: X
  display.drawLine(125, 56, 119, 62);

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(18, 17, "OLED 128x64");
  display.drawStr(24, 31, "ADDR 0x3C");
  display.drawStr(17, 45, "TOP / BOTTOM");
  display.drawStr(30, 58, "PAGE 1/3");
  display.sendBuffer();
}

void drawVerticalPage() {
  display.clearBuffer();
  for (uint8_t x = 0; x < 128; x += 8) {
    display.drawBox(x, 0, 4, 64);
  }
  display.setDrawColor(2); // XOR text so it remains visible on both stripe colors.
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(34, 34, "VERTICAL");
  display.setDrawColor(1);
  display.sendBuffer();
}

void drawHorizontalPage() {
  display.clearBuffer();
  for (uint8_t y = 0; y < 64; y += 8) {
    display.drawBox(0, y, 128, 4);
  }
  display.setDrawColor(2);
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(31, 34, "HORIZONTAL");
  display.setDrawColor(1);
  display.sendBuffer();
}

void drawPage(uint8_t page) {
  switch (page) {
    case 0:
      drawGeometryPage();
      Serial.println("OLED_PAGE,1,GEOMETRY");
      break;
    case 1:
      drawVerticalPage();
      Serial.println("OLED_PAGE,2,VERTICAL_STRIPES");
      break;
    default:
      drawHorizontalPage();
      Serial.println("OLED_PAGE,3,HORIZONTAL_STRIPES");
      break;
  }
}
} // namespace

void setup() {
  // Safety first: the speaker is not part of this test.
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  Serial.begin(115200);
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }

  Wire.begin(MINI_SYNTH_PIN_I2C_SDA, MINI_SYNTH_PIN_I2C_SCL);
  Wire.setClock(100000);

  // U8g2 stores I2C addresses in the 8-bit form.
  display.setI2CAddress(OLED_ADDRESS_7BIT << 1);
  display.setBusClock(100000);
  display.begin();
  display.setContrast(128); // Moderate brightness for the first physical test.

  Serial.println();
  Serial.println("OLEDTEST_BEGIN");
  Serial.printf("INFO,I2C_ADDRESS,0x%02X\n", OLED_ADDRESS_7BIT);
  Serial.printf("INFO,I2C_PINS,SDA,%d,SCL,%d\n",
                MINI_SYNTH_PIN_I2C_SDA,
                MINI_SYNTH_PIN_I2C_SCL);
  Serial.printf("INFO,DISPLAY,%u,%u\n",
                display.getDisplayWidth(),
                display.getDisplayHeight());
  Serial.println("INFO,AMP,DISABLED");

  drawPage(currentPage);
  pageChangedAt = millis();
}

void loop() {
  const uint32_t now = millis();
  if (now - pageChangedAt >= PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % 3;
    drawPage(currentPage);
    pageChangedAt = now;
  }
}
