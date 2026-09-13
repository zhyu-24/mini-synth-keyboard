// Intentionally no include guard: isolated current/baseline production contexts.
#define portENTER_CRITICAL(mux) ((void)0)
#define portEXIT_CRITICAL(mux) ((void)0)
std::vector<uint8_t> nvs;
bool nvsAvailable = true, nvsShortRead = false, nvsWriteFails = false;
unsigned nvsAccesses = 0;
struct Preferences {
  bool begin(const char *ns, bool) {
    assert(std::string(ns) == "msynth-set");
    ++nvsAccesses;
    return nvsAvailable;
  }
  size_t getBytesLength(const char *) { return nvs.size(); }
  size_t getBytes(const char *, void *destination, size_t size) {
    assert(size <= nvs.size());
    if (nvsShortRead) return 0;
    memcpy(destination, nvs.data(), size);
    return size;
  }
  size_t putBytes(const char *, const void *data, size_t size) {
    if (nvsWriteFails) return 0;
    const auto *bytes = static_cast<const uint8_t *>(data);
    nvs.assign(bytes, bytes + size);
    return size;
  }
  void end() {}
};

constexpr int u8g2_font_6x10_tf = 0, u8g2_font_5x7_tf = 1;
bool displayReady = true;
unsigned badgeDraws = 0;
struct Display {
  std::map<int, std::string> rows;
  void clearBuffer() { rows.clear(); }
  void setFont(int) {}
  void drawStr(int, int y, const char *value) { rows[y] = value; }
  void drawLine(int, int, int, int) {}
  void sendBuffer() {}
} display;
uint32_t millis() { return 0; }
void drawGlobalBleBadge(uint32_t) { ++badgeDraws; }

bool sharedCatalogRefreshGate = false, sharedAudioCatalogQuiescent = false;
CatalogSong testSong = {};
const CatalogSong AUDIO_SAFE_EMBEDDED_SONG = {};
const CatalogSong &catalogSong(uint8_t) { return testSong; }
AppId readActiveApp() { return sharedActiveApp; }
int testBlock = 0, testBlocks = 0, startupWrites = 0;
std::function<void(int)> blockSetup;
std::vector<int16_t> captured;
std::vector<std::array<float, 7>> capturedGains, capturedWeights;
SharedPerformanceState readPerformanceState() {
  if (blockSetup) blockSetup(testBlock);
  return {sharedNoteMask, sharedOctaveOffset};
}
bool writeAudioFrames() {
  if (startupWrites++ < STARTUP_SILENCE_BLOCKS) {
    for (const auto &f : audioFrames) assert(f.left == 0 && f.right == 0);
    return true;
  }
  for (const auto &f : audioFrames) {
    assert(f.left == 0 && std::abs(static_cast<int>(f.right)) <= MASTER_PEAK);
    captured.push_back(f.right);
  }
  std::array<float, 7> gains, weights;
  std::copy(currentPitchGains, currentPitchGains + NOTE_COUNT, gains.begin());
  std::copy(currentNormalizationPitchGains,
            currentNormalizationPitchGains + NOTE_COUNT, weights.begin());
  capturedGains.push_back(gains);
  capturedWeights.push_back(weights);
  return ++testBlock < testBlocks;
}
constexpr int MINI_SYNTH_PIN_AMP_CTRL = 21, MINI_SYNTH_AMP_ENABLE_LEVEL = 1;
void digitalWrite(int, int) { assert(startupWrites == STARTUP_SILENCE_BLOCKS); }
void vTaskDelete(void *) {}
