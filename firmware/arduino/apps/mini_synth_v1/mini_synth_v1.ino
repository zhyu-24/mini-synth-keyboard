#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <ESP_I2S.h>
#include <FFat.h>
#include <FS.h>
#include <MiniSynthPins.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr size_t AUDIO_FRAMES_PER_BLOCK = 64;  // 4 ms at 16 kHz.
constexpr uint8_t NOTE_COUNT = 7;
constexpr uint8_t INPUT_COUNT = 9;
constexpr uint8_t DEBOUNCE_TICKS = 5;          // 5 stable samples at 1 kHz.
constexpr uint16_t INPUT_SCAN_PERIOD_MS = 1;
constexpr uint16_t UI_MIN_REFRESH_MS = 20;       // Coalesce rapid edges into one OLED frame.
constexpr uint16_t UI_PERIODIC_REFRESH_MS = 250; // Refresh health/status even when idle.
constexpr float ATTACK_SECONDS = 0.010f;
constexpr float RELEASE_SECONDS = 0.025f;
constexpr uint16_t SONG_NOTE_FADE_MS = 12;
constexpr uint16_t SONG_NOTE_FADE_SAMPLES =
    static_cast<uint16_t>((SAMPLE_RATE_HZ * SONG_NOTE_FADE_MS) / 1000U);
static_assert(SONG_NOTE_FADE_SAMPLES > 0,
              "Song note fade must span at least one sample");
// 8BIT is a square-like timbre: it has far more upper-harmonic energy than a
// sine and the installed speaker emphasizes that region. Use a long edge ramp
// plus an independently calibrated timbre trim; neither changes the confirmed
// global ceiling. User-confirmed equal-loudness value: 0.1585 (-16 dB).
constexpr uint16_t EIGHT_BIT_EDGE_SAMPLES = 12;
constexpr float EIGHT_BIT_TIMBRE_GAIN = 0.1585f;  // User-confirmed.
// Start PIANO_SYNTH conservatively: its upper harmonics overlap the speaker's
// sensitive band. This is an initial value for real-board equal-loudness tuning.
constexpr float PIANO_TIMBRE_GAIN = 0.34f;  // User tuning: about +2.33 dB.
constexpr float PIANO_ATTACK_SECONDS = 0.006f;
constexpr float PIANO_DECAY_SECONDS = 1.20f;
constexpr float PIANO_SUSTAIN_LEVEL = 0.16f;
// Real piano brightness is concentrated near the hammer strike rather than
// held for the full note. This short envelope drives only the extra harmonics.
constexpr float PIANO_HAMMER_DECAY_SECONDS = 0.080f;  // User-confirmed.
// ORGAN uses a normalized additive drawbar-like blend. Keep this independent
// so its real-speaker equal-loudness value can be calibrated without touching
// SINE, 8BIT, the 21-note pitch table, or MASTER_PEAK.
constexpr float ORGAN_TIMBRE_GAIN = 0.2223f;  // User-confirmed.
constexpr uint16_t OCTAVE_GLIDE_MS = 12;
constexpr uint16_t OCTAVE_GLIDE_SAMPLES =
    static_cast<uint16_t>((SAMPLE_RATE_HZ * OCTAVE_GLIDE_MS) / 1000U);
static_assert(OCTAVE_GLIDE_SAMPLES > 0, "Octave glide must span at least one sample");
constexpr int16_t MASTER_PEAK = 18000;  // User-confirmed production setting; preserve in future edits.
constexpr float NOTE_ATTENUATION_DB[3][7] = {
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {-1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -5.0f, -6.0f},
    {-8.0f, -11.0f, -14.0f, -14.0f, -15.0f, -17.0f, -18.0f},
};
constexpr uint8_t OLED_ADDRESS_7BIT = 0x3C;
constexpr uint8_t OLED_YELLOW_ROWS = 16;
constexpr uint16_t SINE_TABLE_SIZE = 256;
constexpr double PHASE_SCALE = 4294967296.0;   // 2^32.
constexpr uint16_t STARTUP_SILENCE_BLOCKS = 25; // 100 ms before amplifier enable.
constexpr uint16_t GLOBAL_HOME_HOLD_MS = 1000;
constexpr uint16_t GLOBAL_BLE_PAIR_HOLD_MS = 3000;
constexpr uint32_t GLOBAL_BLE_PAIR_WINDOW_MS = 30000;
// Requested relative octave compensation for non-SINE timbres only.
constexpr float NON_SINE_C4_GAIN = 1.41253754f;  // +3 dB.
constexpr float NON_SINE_C5_GAIN = 1.99526231f;  // +6 dB.
constexpr uint8_t DEFAULT_GLOBAL_VOLUME_PERCENT = 100;
constexpr uint8_t GLOBAL_VOLUME_STEP_PERCENT = 10;

// BLE game keyboard: one modifier byte plus an eight-key NKRO bitmap.
constexpr char KEYBOARD_DEVICE_NAME[] = "MiniSynth GamePad";
constexpr char KEYBOARD_MANUFACTURER_NAME[] = "Tsinghua GRID";
constexpr uint8_t KEYBOARD_HID_REPORT_ID = 1;
constexpr uint8_t KEYBOARD_LEFT_ALT_MODIFIER = 0x04;
constexpr uint16_t KEYBOARD_VALID_PHYSICAL_MASK = 0x01FF;
constexpr uint16_t KEYBOARD_SW8_MASK = 1U << 7;
constexpr uint16_t KEYBOARD_SW9_MASK = 1U << 8;
constexpr UBaseType_t KEYBOARD_REPORT_QUEUE_DEPTH = 16;

// MSPKG v1 / FFat runtime catalog boundaries. Keep limits explicit so a
// malformed package cannot exhaust PSRAM or make the audio scheduler unsafe.
constexpr uint8_t EMBEDDED_SONG_COUNT = 4;
constexpr uint8_t MAX_FILE_SONGS = 30;
constexpr uint8_t MAX_RUNTIME_SONGS = MAX_FILE_SONGS;
constexpr uint8_t MAX_CATALOG_PATH_BYTES = 96;
static_assert(MAX_FILE_SONGS == 30, "Unexpected file-song capacity");
static_assert(MAX_RUNTIME_SONGS >= EMBEDDED_SONG_COUNT,
              "Runtime catalog must hold embedded fallback songs");
constexpr uint32_t MAX_MSPKG_METADATA_BYTES = 4096;
constexpr uint32_t MAX_MSPKG_PAYLOAD_BYTES = 512UL * 1024UL;
constexpr uint32_t MAX_MSPKG_NOTE_EVENTS = 2000;
constexpr uint8_t MAX_MSPKG_ACTIVE_POLYPHONY = 4;
constexpr uint8_t MAX_SEQUENCE_VOICES = MAX_MSPKG_ACTIVE_POLYPHONY + 2;
constexpr uint8_t MAX_MSPKG_TEMPO_RECORDS = 64;
constexpr uint8_t MSPKG_TITLE_BYTES = 31;
constexpr uint8_t MSPKG_TIMBRE_BYTES = 15;

// MUSB v1 runs over the ESP32-S3 native Hardware CDC/JTAG Serial selected by
// the pinned FQBN. Keep these values wire-compatible with tools/usb-import/.
constexpr uint8_t USB_PROTOCOL_VERSION = 1;
constexpr char USB_FRAME_MAGIC[4] = {'M', 'U', 'S', 'B'};
constexpr uint32_t MAX_USB_FILE_BYTES =
    32UL + MAX_MSPKG_METADATA_BYTES + MAX_MSPKG_PAYLOAD_BYTES;
constexpr uint16_t MAX_USB_FRAME_PAYLOAD = 1152;
constexpr uint16_t MAX_USB_CHUNK_BYTES = 1024;
constexpr uint8_t MAX_USB_FILENAME_BYTES = 48;
constexpr uint16_t USB_RX_BUFFER_BYTES = 2048;
constexpr uint16_t USB_LOOP_BYTE_BUDGET = 2048;
constexpr uint32_t USB_UPLOAD_IDLE_TIMEOUT_MS = 30000;
constexpr uint32_t USB_PARSER_IDLE_TIMEOUT_MS = 1000;
constexpr uint32_t USB_RESULT_DISPLAY_MS = 2500;
constexpr char USB_TEMP_PATH[] = "/.~usb-upload.tmp";
constexpr char USB_INIT_STORAGE_CONFIRMATION[] = "FORMAT_FFAT_V1";
static_assert(MAX_USB_FILE_BYTES == 528416UL,
              "Unexpected MSPKG upload boundary");

enum UsbCommand : uint8_t {
  USB_CMD_LIST = 0x01,
  USB_CMD_STATUS = 0x02,
  USB_CMD_BEGIN = 0x03,
  USB_CMD_CHUNK = 0x04,
  USB_CMD_FINISH = 0x05,
  USB_CMD_ABORT = 0x06,
  USB_CMD_DELETE = 0x07,
  USB_CMD_INIT_STORAGE = 0x08,
  USB_RSP_ACK = 0x80,
  USB_RSP_ERROR = 0x81,
  USB_RSP_LIST_ENTRY = 0x82,
  USB_RSP_LIST_DONE = 0x83,
  USB_RSP_STATUS = 0x84,
};

enum UsbDeviceState : uint8_t {
  USB_STATE_IDLE = 0,
  USB_STATE_RECEIVING = 1,
  USB_STATE_VALIDATING = 2,
  USB_STATE_COMMITTING = 3,
  USB_STATE_ERROR = 4,
};

enum UsbErrorCode : uint16_t {
  USB_ERR_OK = 0,
  USB_ERR_BAD_FRAME_CRC = 1,
  USB_ERR_PROTOCOL_VERSION = 2,
  USB_ERR_UNKNOWN_COMMAND = 3,
  USB_ERR_BAD_PAYLOAD = 4,
  USB_ERR_UNSAFE_FILENAME = 5,
  USB_ERR_FILE_TOO_LARGE = 6,
  USB_ERR_FS_NOT_MOUNTED = 7,
  USB_ERR_BUSY = 8,
  USB_ERR_NO_UPLOAD = 9,
  USB_ERR_OFFSET_MISMATCH = 10,
  USB_ERR_CHUNK_LENGTH = 11,
  USB_ERR_FILE_IO = 12,
  USB_ERR_FILE_LENGTH = 13,
  USB_ERR_FILE_CRC = 14,
  USB_ERR_MSPKG_MAGIC = 15,
  USB_ERR_MSPKG_VERSION = 16,
  USB_ERR_MSPKG_TYPE = 17,
  USB_ERR_MSPKG_DECLARED_SIZE = 18,
  USB_ERR_METADATA_CRC = 19,
  USB_ERR_PAYLOAD_CRC = 20,
  USB_ERR_PAYLOAD_FORMAT = 21,
  USB_ERR_COMMIT_FAILED = 22,
  USB_ERR_ROLLBACK_FAILED = 23,
  USB_ERR_NO_SPACE = 24,
  USB_ERR_FRAME_TOO_LARGE = 25,
  USB_ERR_DELETE_FAILED = 26,
  USB_ERR_FORMAT_FAILED = 27,
};

enum class UsbOverlayState : uint8_t {
  Hidden,
  Receiving,
  Validating,
  Committing,
  Refreshing,
  Complete,
  Failed,
};

struct UsbFrameHeader {
  char magic[4];
  uint8_t version;
  uint8_t command;
  uint16_t flags;
  uint32_t sequence;
  uint32_t payloadLength;
  uint32_t payloadCrc32;
} __attribute__((packed));
static_assert(sizeof(UsbFrameHeader) == 20,
              "Unexpected USB protocol header size");

struct UsbAckPayload {
  uint8_t requestCommand;
  uint8_t state;
  uint16_t code;
  uint32_t nextOffset;
  uint32_t totalLength;
  uint32_t fileCrc32;
} __attribute__((packed));
static_assert(sizeof(UsbAckPayload) == 16,
              "Unexpected USB ACK payload size");

struct UsbStatusPayload {
  uint8_t state;
  uint8_t mounted;
  uint16_t code;
  uint32_t nextOffset;
  uint32_t totalLength;
  uint32_t expectedCrc32;
  uint32_t runningCrc32;
} __attribute__((packed));
static_assert(sizeof(UsbStatusPayload) == 20,
              "Unexpected USB status payload size");

enum class AppId : uint8_t {
  Play,
  Home,
  Song,
  Loop,
  Beat,
  Pet,
  Keyboard,
  Game,
  Settings,
};

enum class SongTimbre : uint8_t {
  Sine,
  EightBit,
  Organ,
  PianoSynth,
};

enum class SongStatus : uint8_t {
  Stopped,
  Playing,
  Paused,
  Finished,
};

enum class SongLoadState : uint8_t {
  Ready,
  Loading,
  Failed,
};

enum class PlayOctaveMode : uint8_t {
  Hold,
  Latch,
};

enum class SettingRow : uint8_t {
  Volume,
  OctaveMode,
  PlaySound,
  PitchEq,
};
constexpr uint8_t SETTING_ROW_COUNT = 4;
constexpr uint8_t SETTINGS_VISIBLE_ROWS = 3;
constexpr bool DEFAULT_PITCH_EQ_ENABLED = false;

struct PersistentSettingsRecordV1 {
  uint32_t magic;
  uint8_t version;
  uint8_t volumePercent;
  uint8_t octaveMode;
  uint8_t playTimbre;
  uint32_t checksum;
} __attribute__((packed));
static_assert(sizeof(PersistentSettingsRecordV1) == 12,
              "Unexpected v1 settings record size");
struct PersistentSettingsRecord {
  uint32_t magic;
  uint8_t version;
  uint8_t volumePercent;
  uint8_t octaveMode;
  uint8_t playTimbre;
  uint8_t pitchEqEnabled;
  uint32_t checksum;
} __attribute__((packed));
static_assert(sizeof(PersistentSettingsRecord) == 13,
              "Unexpected settings record size");
constexpr uint32_t SETTINGS_MAGIC = 0x4D535332UL;  // MSS2.
constexpr uint8_t SETTINGS_VERSION = 2;
constexpr char SETTINGS_NVS_NAMESPACE[] = "msynth-set";
constexpr char SETTINGS_NVS_KEY[] = "record";

enum class KeyboardBleState : uint8_t {
  Off,
  Starting,
  Advertising,
  Connected,
  Lost,
  Error,
};

enum class GlobalBleCommand : uint8_t {
  None,
  OpenPairing,
  SwitchPeer,
};

// SW8/SW9 retain historical macro names in the installed board library.
// Play interprets them as HOLD or LATCH octave controls; every app reserves
// the two-button hold for the global Home/BLE gesture.
constexpr uint8_t OCTAVE_DOWN_PIN = MINI_SYNTH_PIN_KEY_PLAY_STOP;  // SW8 / GPIO18
constexpr uint8_t OCTAVE_UP_PIN = MINI_SYNTH_PIN_KEY_FN;           // SW9 / GPIO8

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
    OCTAVE_DOWN_PIN,
    OCTAVE_UP_PIN,
};

const char *const NOTE_NAMES[NOTE_COUNT] = {"C", "D", "E", "F", "G", "A", "B"};
const float NOTE_FREQUENCIES_C4[NOTE_COUNT] = {
    261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f};

struct StereoFrame {
  int16_t left;
  int16_t right;
};
static_assert(sizeof(StereoFrame) == 4, "Unexpected stereo frame packing");

struct SharedPerformanceState {
  uint8_t noteMask;
  int8_t octaveOffset;
};

struct KeyboardNkroReport {
  uint8_t modifiers;
  uint8_t gameKeys;
} __attribute__((packed));
static_assert(sizeof(KeyboardNkroReport) == 2,
              "Unexpected BLE keyboard report size");

struct KeyboardReportRequest {
  uint32_t epoch;
  uint16_t physicalMask;
};

struct KeyboardTransportSnapshot {
  bool connected;
  bool appActive;
  bool releaseGate;
  bool needsInitialZero;
  bool restartAdvertising;
  uint32_t epoch;
};

const uint8_t KEYBOARD_HID_REPORT_DESCRIPTOR[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, KEYBOARD_HID_REPORT_ID,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x07,
    0x09, 0x14, 0x09, 0x1A, 0x09, 0x08, 0x09, 0x15,
    0x09, 0x04, 0x09, 0x16, 0x09, 0x07, 0x09, 0x09,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0xC0,
};

struct SongEvent {
  uint32_t startSample;
  uint32_t durationSamples;
  uint8_t midiPitch;
};

struct SequenceVoice {
  uint32_t phase;
  uint32_t endSample;
  uint32_t phaseStep;
  float pitchGain;
  float normalizationPitchGain;
  float envelope;
  float smoothedEightBit;
  float pianoEnvelope;
  float pianoHammerEnvelope;
  uint8_t midiPitch;
  SongTimbre timbre;
  bool active;
  bool pianoAttackPhase;
};
// Runtime packages are accepted only up to four active notes. Two extra voices
// preserve 12 ms release tails when a new chord starts at the same sample.
constexpr uint8_t SEQUENCE_VOICE_COUNT = MAX_SEQUENCE_VOICES;

constexpr SongEvent SONG_VOYAGERFAREWELL_EVENTS[] = {
    {45714U, 17143U, 63},
    {62857U, 2857U, 61},
    {65714U, 8572U, 63},
    {77143U, 2857U, 61},
    {80000U, 5714U, 63},
    {85714U, 5715U, 66},
    {91429U, 17142U, 61},
    {108571U, 5715U, 59},
    {125714U, 5715U, 66},
    {131429U, 5714U, 66},
    {137143U, 17143U, 58},
    {154286U, 5714U, 59},
    {171429U, 5714U, 61},
    {177143U, 2857U, 61},
    {180000U, 2857U, 59},
    {182857U, 45715U, 59},
    {228572U, 17142U, 63},
    {245714U, 2858U, 61},
    {248572U, 2857U, 63},
    {251429U, 5714U, 63},
    {260000U, 2857U, 61},
    {262857U, 5715U, 63},
    {268572U, 5714U, 68},
    {274286U, 5714U, 61},
    {285714U, 5715U, 61},
    {291429U, 2857U, 66},
    {294286U, 2857U, 61},
    {305714U, 2858U, 59},
    {308572U, 5714U, 58},
    {314286U, 5714U, 59},
    {320000U, 17143U, 58},
    {337143U, 5714U, 59},
    {342857U, 5715U, 61},
    {348572U, 2857U, 61},
    {351429U, 2857U, 63},
    {360000U, 5714U, 61},
    {365714U, 11429U, 59},
    {377143U, 11429U, 58},
    {388572U, 22857U, 56},
    {411429U, 17143U, 63},
    {428572U, 2857U, 61},
    {431429U, 8571U, 63},
    {442857U, 2857U, 61},
    {445714U, 5715U, 63},
    {451429U, 5714U, 66},
    {457143U, 17143U, 61},
    {474286U, 2857U, 59},
    {477143U, 2857U, 61},
    {488572U, 2857U, 59},
    {491429U, 5714U, 58},
    {497143U, 5714U, 59},
    {502857U, 11429U, 58},
    {514286U, 5714U, 58},
    {520000U, 5714U, 59},
    {525714U, 17143U, 61},
    {542857U, 5715U, 61},
    {548572U, 2857U, 59},
    {551429U, 5714U, 58},
    {557143U, 2857U, 59},
    {560000U, 5715U, 58},
    {565715U, 2857U, 59},
    {568572U, 2857U, 58},
    {571429U, 2857U, 58},
    {574286U, 5714U, 59},
    {580000U, 2857U, 58},
    {582857U, 5715U, 59},
    {588572U, 5714U, 61},
    {594286U, 17143U, 63},
    {611429U, 2857U, 61},
    {614286U, 8571U, 63},
    {625715U, 2857U, 61},
    {628572U, 5714U, 63},
    {634286U, 5714U, 68},
    {640000U, 17143U, 61},
    {657143U, 2857U, 66},
    {660000U, 2857U, 61},
    {671429U, 2857U, 59},
    {674286U, 5714U, 58},
    {680000U, 5715U, 59},
    {685715U, 17142U, 58},
    {702857U, 2858U, 58},
    {705715U, 2857U, 59},
    {708572U, 11428U, 61},
    {720000U, 11429U, 63},
    {731429U, 5714U, 61},
    {737143U, 5714U, 59},
    {742857U, 11429U, 59},
    {754286U, 11429U, 71},
    {765715U, 11428U, 73},
    {777143U, 22857U, 75},
    {800000U, 5715U, 75},
    {805715U, 2857U, 73},
    {808572U, 5714U, 73},
    {814286U, 8571U, 71},
    {822857U, 22858U, 68},
    {845715U, 5714U, 75},
    {851429U, 2857U, 73},
    {854286U, 5714U, 73},
    {860000U, 8572U, 71},
    {868572U, 11428U, 66},
    {880000U, 5715U, 68},
    {885715U, 2857U, 63},
    {888572U, 2857U, 66},
    {891429U, 11429U, 66},
    {902858U, 5714U, 73},
    {908572U, 5714U, 71},
    {914286U, 17143U, 70},
    {931429U, 2857U, 71},
    {934286U, 14286U, 71},
    {960000U, 22858U, 75},
    {982858U, 5714U, 75},
    {988572U, 2857U, 73},
    {991429U, 5714U, 73},
    {997143U, 8572U, 71},
    {1005715U, 22857U, 68},
    {1028572U, 5714U, 75},
    {1034286U, 2857U, 73},
    {1037143U, 5715U, 73},
    {1042858U, 8571U, 71},
    {1051429U, 17143U, 66},
    {1068572U, 5714U, 66},
    {1074286U, 8572U, 70},
    {1082858U, 8571U, 70},
    {1091429U, 5714U, 70},
    {1097143U, 8572U, 70},
    {1105715U, 8571U, 71},
    {1114286U, 5714U, 70},
    {1120000U, 11429U, 73},
    {1131429U, 11429U, 70},
    {1142858U, 17142U, 63},
    {1160000U, 2858U, 61},
    {1162858U, 8571U, 63},
    {1174286U, 2857U, 61},
    {1177143U, 5715U, 63},
    {1182858U, 5714U, 66},
    {1188572U, 17143U, 61},
    {1205715U, 5714U, 59},
    {1222858U, 5714U, 66},
    {1228572U, 5714U, 66},
    {1234286U, 17143U, 58},
    {1251429U, 5714U, 59},
    {1268572U, 5714U, 61},
    {1274286U, 5715U, 61},
    {1280001U, 45714U, 59},
    {1325715U, 17143U, 63},
    {1342858U, 2857U, 61},
    {1345715U, 2857U, 63},
    {1348572U, 5714U, 63},
    {1357143U, 2858U, 61},
    {1360001U, 5714U, 63},
    {1365715U, 5714U, 68},
    {1371429U, 5714U, 68},
    {1377143U, 11429U, 70},
    {1388572U, 17143U, 66},
    {1405715U, 2857U, 63},
    {1408572U, 2857U, 61},
    {1411429U, 5714U, 59},
    {1417143U, 34286U, 58},
    {1451429U, 2857U, 63},
    {1454286U, 2857U, 61},
    {1457143U, 5715U, 59},
    {1462858U, 11428U, 59},
    {1474286U, 5715U, 61},
    {1480001U, 5714U, 63},
    {1485715U, 11428U, 63},
    {1497143U, 5715U, 66},
    {1502858U, 5714U, 68},
    {1508572U, 11429U, 75},
    {1520001U, 11428U, 68},
    {1531429U, 5714U, 68},
    {1537143U, 5715U, 70},
    {1542858U, 5714U, 71},
    {1548572U, 5714U, 66},
    {1554286U, 5715U, 68},
    {1560001U, 5714U, 70},
    {1565715U, 5714U, 71},
    {1571429U, 5714U, 63},
    {1577143U, 5715U, 64},
    {1582858U, 5714U, 61},
    {1588572U, 5714U, 58},
    {1594286U, 5715U, 59},
    {1600001U, 5714U, 61},
    {1605715U, 5714U, 58},
    {1611429U, 5715U, 59},
    {1617144U, 5714U, 56},
    {1622858U, 5714U, 58},
    {1628572U, 5714U, 54},
    {1634286U, 5715U, 56},
    {1640001U, 5714U, 52},
    {1645715U, 5714U, 54},
    {1651429U, 5715U, 51},
    {1657144U, 5714U, 52},
    {1662858U, 5714U, 49},
    {1668572U, 11429U, 71},
    {1680001U, 11428U, 73},
    {1691429U, 22857U, 75},
    {1714286U, 5715U, 75},
    {1720001U, 2857U, 73},
    {1722858U, 5714U, 73},
    {1728572U, 8572U, 71},
    {1737144U, 22857U, 68},
    {1760001U, 5714U, 75},
    {1765715U, 2857U, 73},
    {1768572U, 5714U, 73},
    {1774286U, 8572U, 71},
    {1782858U, 11428U, 66},
    {1794286U, 5715U, 68},
    {1800001U, 2857U, 63},
    {1802858U, 2857U, 66},
    {1805715U, 11429U, 66},
    {1817144U, 5714U, 73},
    {1822858U, 5714U, 71},
    {1828572U, 17143U, 70},
    {1845715U, 2857U, 71},
    {1848572U, 2857U, 71},
    {1851429U, 11429U, 71},
    {1874286U, 22858U, 75},
    {1897144U, 5714U, 75},
    {1902858U, 2857U, 73},
    {1905715U, 5714U, 73},
    {1911429U, 8572U, 71},
    {1920001U, 22857U, 68},
    {1942858U, 5714U, 75},
    {1948572U, 2857U, 73},
    {1951429U, 5715U, 73},
    {1957144U, 8571U, 71},
    {1965715U, 17143U, 66},
    {1982858U, 5714U, 66},
    {1988572U, 8572U, 70},
    {1997144U, 8571U, 70},
    {2005715U, 5714U, 70},
    {2011429U, 8572U, 70},
    {2020001U, 8571U, 71},
    {2028572U, 5715U, 70},
    {2034287U, 11428U, 73},
    {2045715U, 11429U, 70},
    {2057144U, 17143U, 75},
    {2074287U, 5714U, 68},
    {2080001U, 5714U, 68},
    {2085715U, 5714U, 75},
    {2091429U, 5715U, 75},
    {2097144U, 5714U, 75},
    {2102858U, 8571U, 75},
    {2111429U, 8572U, 78},
    {2120001U, 5714U, 71},
    {2125715U, 22857U, 73},
    {2148572U, 17143U, 73},
    {2165715U, 5714U, 66},
    {2171429U, 5715U, 66},
    {2177144U, 5714U, 73},
    {2182858U, 5714U, 73},
    {2188572U, 5715U, 73},
    {2194287U, 2857U, 73},
    {2197144U, 5714U, 71},
    {2202858U, 8571U, 70},
    {2211429U, 5715U, 71},
    {2217144U, 11428U, 71},
    {2228572U, 11429U, 72},
    {2240001U, 17143U, 75},
    {2257144U, 5714U, 68},
    {2262858U, 5714U, 68},
    {2268572U, 5715U, 75},
    {2274287U, 5714U, 75},
    {2280001U, 5714U, 75},
    {2285715U, 2857U, 73},
    {2288572U, 5715U, 71},
    {2294287U, 8571U, 70},
    {2302858U, 5714U, 73},
    {2308572U, 22858U, 73},
    {2331430U, 2857U, 71},
    {2334287U, 5714U, 73},
    {2340001U, 8571U, 73},
    {2348572U, 5715U, 73},
    {2354287U, 11428U, 73},
    {2365715U, 5715U, 68},
    {2371430U, 5714U, 71},
    {2377144U, 8571U, 71},
    {2385715U, 8572U, 71},
    {2394287U, 2857U, 72},
    {2397144U, 2857U, 72},
    {2400001U, 2857U, 73},
    {2402858U, 5714U, 72},
    {2408572U, 5715U, 68},
    {2414287U, 8571U, 66},
    {2422858U, 17143U, 75},
    {2440001U, 11429U, 68},
    {2451430U, 5714U, 75},
    {2457144U, 5714U, 75},
    {2462858U, 5714U, 75},
    {2468572U, 2858U, 73},
    {2471430U, 5714U, 71},
    {2477144U, 8571U, 70},
    {2485715U, 5715U, 73},
    {2491430U, 22857U, 73},
    {2514287U, 17143U, 73},
    {2531430U, 11428U, 66},
    {2542858U, 5714U, 73},
    {2548572U, 5715U, 73},
    {2554287U, 5714U, 73},
    {2560001U, 2857U, 73},
    {2562858U, 5714U, 71},
    {2568572U, 8572U, 70},
    {2577144U, 5714U, 71},
    {2582858U, 11429U, 71},
    {2594287U, 11428U, 72},
    {2605715U, 17143U, 76},
    {2622858U, 5714U, 68},
    {2628572U, 5715U, 68},
    {2634287U, 5714U, 75},
    {2640001U, 5714U, 76},
    {2645715U, 5715U, 75},
    {2651430U, 2857U, 73},
    {2654287U, 5714U, 71},
    {2660001U, 8571U, 70},
    {2668572U, 5715U, 73},
    {2674287U, 22857U, 73},
    {2697144U, 17143U, 71},
    {2714287U, 11428U, 70},
    {2725715U, 5715U, 71},
    {2731430U, 2857U, 73},
    {2734287U, 8571U, 73},
    {2742858U, 8572U, 68},
    {2751430U, 8571U, 70},
    {2760001U, 5714U, 72},
    {2765715U, 5715U, 73},
    {2771430U, 5714U, 75},
    {2777144U, 5714U, 76},
    {2782858U, 51429U, 75},
    {2834287U, 45714U, 73},
    {2880001U, 45714U, 82},
    {2925715U, 45715U, 78},
    {2971430U, 45714U, 75},
    {3017144U, 45714U, 82},
    {3062858U, 45715U, 85},
    {3108573U, 45714U, 85},
};
constexpr size_t SONG_VOYAGERFAREWELL_EVENT_COUNT = sizeof(SONG_VOYAGERFAREWELL_EVENTS) / sizeof(SONG_VOYAGERFAREWELL_EVENTS[0]);
constexpr uint32_t SONG_VOYAGERFAREWELL_DURATION_SAMPLES = 3154287U;

constexpr SongEvent SONG_SPRING_EVENTS[] = {
    {0U, 9897U, 75},
    {9897U, 4948U, 73},
    {14845U, 9897U, 71},
    {24742U, 4949U, 73},
    {29691U, 7422U, 75},
    {37113U, 2475U, 76},
    {39588U, 4948U, 75},
    {44536U, 14845U, 73},
    {59381U, 9897U, 75},
    {69278U, 4949U, 73},
    {74227U, 9897U, 71},
    {84124U, 4948U, 73},
    {89072U, 7423U, 75},
    {96495U, 2474U, 76},
    {98969U, 4949U, 75},
    {103918U, 14845U, 73},
    {118763U, 9897U, 75},
    {128660U, 4948U, 73},
    {133608U, 9897U, 71},
    {143505U, 4949U, 73},
    {148454U, 7422U, 75},
    {155876U, 2475U, 76},
    {158351U, 4948U, 75},
    {163299U, 14845U, 73},
    {178144U, 9897U, 75},
    {188041U, 4949U, 73},
    {192990U, 9897U, 71},
    {202887U, 4948U, 73},
    {207835U, 7423U, 75},
    {215258U, 2474U, 76},
    {217732U, 4949U, 75},
    {222681U, 9896U, 73},
    {232577U, 2475U, 59},
    {235052U, 2474U, 61},
    {237526U, 4948U, 63},
    {242474U, 4949U, 63},
    {247423U, 4948U, 61},
    {252371U, 4949U, 64},
    {257320U, 4948U, 63},
    {262268U, 4949U, 61},
    {267217U, 4948U, 61},
    {272165U, 4949U, 61},
    {277114U, 2474U, 59},
    {279588U, 2474U, 59},
    {282062U, 4948U, 64},
    {287010U, 4949U, 63},
    {291959U, 4948U, 61},
    {296907U, 9897U, 61},
    {306804U, 2474U, 59},
    {309278U, 2475U, 61},
    {311753U, 14845U, 63},
    {341443U, 4949U, 63},
    {346392U, 4948U, 66},
    {351340U, 2475U, 71},
    {353815U, 2474U, 70},
    {356289U, 9897U, 70},
    {366186U, 4948U, 71},
    {371134U, 9897U, 70},
    {381031U, 4949U, 71},
    {385980U, 2474U, 70},
    {388454U, 2474U, 68},
    {390928U, 9897U, 66},
    {400825U, 4948U, 66},
    {405773U, 4949U, 61},
    {410722U, 4948U, 64},
    {415670U, 9897U, 64},
    {425567U, 4949U, 63},
    {430516U, 9897U, 63},
    {440413U, 4948U, 54},
    {445361U, 4948U, 64},
    {450309U, 4949U, 63},
    {455258U, 4948U, 61},
    {460206U, 9897U, 63},
    {470103U, 4949U, 66},
    {475052U, 14845U, 59},
    {499794U, 4949U, 59},
    {504743U, 4948U, 61},
    {509691U, 7423U, 59},
    {517114U, 2474U, 59},
    {519588U, 4948U, 59},
    {524536U, 4949U, 66},
    {529485U, 4948U, 59},
    {534433U, 9897U, 64},
    {544330U, 4949U, 63},
    {549279U, 4948U, 61},
    {554227U, 4949U, 59},
    {559176U, 4948U, 59},
    {564124U, 14845U, 59},
    {588866U, 2474U, 59},
    {591340U, 2475U, 61},
    {593815U, 4948U, 63},
    {598763U, 4949U, 63},
    {603712U, 4948U, 61},
    {608660U, 4949U, 64},
    {613609U, 4948U, 63},
    {618557U, 4948U, 61},
    {623505U, 4949U, 61},
    {628454U, 4948U, 61},
    {633402U, 4949U, 59},
    {638351U, 4948U, 64},
    {643299U, 4949U, 63},
    {648248U, 4948U, 61},
    {653196U, 9897U, 61},
    {663093U, 2474U, 59},
    {665567U, 2475U, 61},
    {668042U, 14845U, 63},
    {697732U, 4949U, 63},
    {702681U, 4948U, 66},
    {707629U, 4949U, 71},
    {712578U, 9897U, 70},
    {722475U, 4948U, 71},
    {727423U, 9897U, 70},
    {737320U, 4948U, 71},
    {742268U, 2475U, 70},
    {744743U, 2474U, 68},
    {747217U, 9897U, 66},
    {757114U, 4948U, 66},
    {762062U, 4949U, 61},
    {767011U, 4948U, 64},
    {771959U, 4949U, 64},
    {776908U, 4948U, 63},
    {781856U, 4949U, 63},
    {786805U, 9896U, 63},
    {796701U, 4949U, 54},
    {801650U, 4948U, 64},
    {806598U, 4949U, 63},
    {811547U, 4948U, 61},
    {816495U, 9897U, 63},
    {826392U, 4949U, 66},
    {831341U, 14845U, 59},
    {856083U, 2474U, 59},
    {858557U, 2474U, 59},
    {861031U, 4949U, 61},
    {865980U, 9897U, 59},
    {875877U, 4948U, 59},
    {880825U, 4949U, 66},
    {885774U, 4948U, 59},
    {890722U, 4949U, 64},
    {895671U, 2474U, 64},
    {898145U, 2474U, 64},
    {900619U, 2474U, 63},
    {903093U, 2474U, 61},
    {905567U, 4949U, 61},
    {910516U, 4948U, 59},
    {915464U, 4949U, 58},
    {920413U, 14845U, 59},
    {950104U, 9896U, 63},
    {960000U, 19794U, 76},
    {979794U, 4949U, 68},
    {984743U, 4948U, 66},
    {989691U, 4949U, 66},
    {994640U, 4948U, 66},
    {999588U, 4949U, 64},
    {1004537U, 4948U, 64},
    {1009485U, 4948U, 63},
    {1014433U, 4949U, 61},
    {1019382U, 4948U, 61},
    {1024330U, 9897U, 61},
    {1034227U, 4949U, 66},
    {1039176U, 4948U, 66},
    {1044124U, 2474U, 64},
    {1046598U, 2475U, 64},
    {1049073U, 4948U, 64},
    {1054021U, 4949U, 64},
    {1058970U, 4948U, 63},
    {1063918U, 4948U, 61},
    {1068866U, 9897U, 61},
    {1078763U, 2475U, 59},
    {1081238U, 2474U, 58},
    {1083712U, 14845U, 59},
    {1098557U, 4949U, 68},
    {1103506U, 4948U, 66},
    {1108454U, 4949U, 66},
    {1113403U, 4948U, 66},
    {1118351U, 4949U, 64},
    {1123300U, 4948U, 64},
    {1128248U, 4948U, 63},
    {1133196U, 4949U, 61},
    {1138145U, 4948U, 61},
    {1143093U, 9897U, 61},
    {1152990U, 4949U, 63},
    {1157939U, 4948U, 64},
    {1162887U, 2474U, 63},
    {1165361U, 2475U, 63},
    {1167836U, 2474U, 63},
    {1170310U, 2474U, 63},
    {1172784U, 4949U, 63},
    {1177733U, 4948U, 61},
    {1182681U, 4948U, 63},
    {1187629U, 9897U, 73},
    {1197526U, 4949U, 71},
    {1202475U, 9897U, 71},
    {1212372U, 4948U, 71},
    {1217320U, 9897U, 70},
    {1227217U, 4949U, 68},
    {1232166U, 14845U, 68},
    {1256908U, 4948U, 68},
    {1261856U, 4949U, 68},
    {1266805U, 4948U, 66},
    {1271753U, 2474U, 64},
    {1274227U, 2475U, 64},
    {1276702U, 14845U, 64},
    {1291547U, 2474U, 63},
    {1294021U, 2474U, 64},
    {1296495U, 24743U, 66},
    {1336083U, 2474U, 61},
    {1338557U, 2475U, 63},
    {1341032U, 2474U, 61},
    {1343506U, 2474U, 63},
    {1345980U, 2474U, 64},
    {1348454U, 9897U, 66},
    {1358351U, 2474U, 64},
    {1360825U, 2475U, 66},
    {1365774U, 9897U, 68},
    {1375671U, 2474U, 68},
    {1378145U, 2474U, 70},
    {1380619U, 9897U, 71},
    {1390516U, 2474U, 73},
    {1392990U, 2475U, 71},
    {1395465U, 9897U, 66},
    {1405362U, 4948U, 66},
    {1410310U, 4948U, 66},
    {1415258U, 4949U, 64},
    {1420207U, 4948U, 64},
    {1425155U, 9897U, 63},
    {1435052U, 2474U, 63},
    {1437526U, 2475U, 64},
    {1440001U, 14845U, 66},
    {1454846U, 2474U, 63},
    {1457320U, 2475U, 61},
    {1459795U, 2474U, 63},
    {1462269U, 2474U, 61},
    {1464743U, 2474U, 63},
    {1467217U, 2474U, 64},
    {1469691U, 9897U, 66},
    {1479588U, 2475U, 64},
    {1482063U, 2474U, 66},
    {1484537U, 9897U, 68},
    {1494434U, 2474U, 67},
    {1496908U, 2474U, 68},
    {1499382U, 9897U, 70},
    {1511753U, 2475U, 67},
    {1514228U, 4948U, 75},
    {1519176U, 4948U, 75},
    {1526599U, 2474U, 68},
    {1529073U, 4948U, 76},
    {1534021U, 4949U, 75},
    {1538970U, 4948U, 73},
    {1543918U, 7423U, 73},
    {1551341U, 2474U, 71},
    {1553815U, 2474U, 71},
    {1556289U, 2475U, 70},
    {1558764U, 9897U, 71},
    {1568661U, 2474U, 66},
    {1571135U, 2474U, 71},
    {1573609U, 4948U, 73},
    {1578557U, 4949U, 71},
    {1583506U, 4948U, 71},
    {1588454U, 9897U, 71},
    {1598351U, 4949U, 66},
    {1603300U, 4948U, 73},
    {1608248U, 4949U, 71},
    {1613197U, 4948U, 71},
    {1618145U, 9897U, 71},
    {1628042U, 2474U, 66},
    {1630516U, 2474U, 71},
    {1632990U, 4949U, 73},
    {1637939U, 4948U, 71},
    {1642887U, 4949U, 71},
    {1647836U, 9897U, 71},
    {1657733U, 2474U, 66},
    {1660207U, 2474U, 71},
    {1662681U, 4949U, 73},
    {1667630U, 4948U, 71},
    {1672578U, 4949U, 71},
    {1677527U, 9896U, 71},
    {1687423U, 2475U, 66},
    {1689898U, 2474U, 71},
    {1692372U, 7423U, 73},
    {1699795U, 2474U, 75},
    {1702269U, 4948U, 73},
    {1707217U, 9897U, 71},
    {1717114U, 4949U, 71},
    {1722063U, 4948U, 70},
    {1727011U, 4949U, 68},
    {1731960U, 4948U, 68},
    {1736908U, 9897U, 68},
    {1746805U, 4948U, 66},
    {1751753U, 9897U, 66},
    {1761650U, 4949U, 64},
    {1766599U, 4948U, 64},
    {1771547U, 4949U, 63},
    {1776496U, 4948U, 61},
    {1781444U, 14846U, 63},
    {1811135U, 4948U, 63},
    {1816083U, 4949U, 64},
    {1821032U, 4948U, 63},
    {1825980U, 4949U, 64},
    {1830929U, 4948U, 63},
    {1835877U, 2474U, 61},
    {1838351U, 2475U, 59},
    {1840826U, 14845U, 59},
    {1855671U, 14845U, 59},
    {1870516U, 7423U, 75},
    {1877939U, 2474U, 76},
    {1880413U, 4949U, 75},
    {1885362U, 9897U, 73},
    {1895259U, 2474U, 59},
    {1897733U, 2474U, 61},
    {1900207U, 4949U, 63},
    {1905156U, 4948U, 63},
    {1910104U, 4948U, 61},
    {1915052U, 4949U, 64},
    {1920001U, 4948U, 63},
    {1924949U, 4949U, 61},
    {1929898U, 4948U, 61},
    {1934846U, 4949U, 61},
    {1939795U, 4948U, 59},
    {1944743U, 4949U, 64},
    {1949692U, 4948U, 63},
    {1954640U, 4949U, 61},
    {1959589U, 9896U, 61},
    {1969485U, 2475U, 59},
    {1971960U, 2474U, 61},
    {1974434U, 14845U, 63},
    {2004125U, 4948U, 63},
    {2009073U, 4949U, 66},
    {2014022U, 4948U, 71},
    {2018970U, 9897U, 70},
    {2028867U, 4948U, 71},
    {2033815U, 9897U, 70},
    {2043712U, 4949U, 71},
    {2048661U, 2474U, 70},
    {2051135U, 2474U, 68},
    {2053609U, 9897U, 66},
    {2063506U, 4949U, 66},
    {2068455U, 4948U, 61},
    {2073403U, 4949U, 64},
    {2078352U, 4948U, 64},
    {2083300U, 4948U, 63},
    {2088248U, 4949U, 63},
    {2093197U, 9897U, 63},
    {2103094U, 4948U, 54},
    {2108042U, 4949U, 64},
    {2112991U, 4948U, 63},
    {2117939U, 4949U, 61},
    {2122888U, 9897U, 63},
    {2132785U, 4948U, 66},
    {2137733U, 14845U, 59},
    {2162475U, 2475U, 59},
    {2164950U, 2474U, 59},
    {2167424U, 4948U, 61},
    {2172372U, 9897U, 59},
    {2182269U, 4949U, 59},
    {2187218U, 4948U, 66},
    {2192166U, 4948U, 59},
    {2197114U, 4949U, 64},
    {2202063U, 2474U, 64},
    {2204537U, 2474U, 64},
    {2207011U, 2475U, 63},
    {2209486U, 2474U, 61},
    {2211960U, 4948U, 61},
    {2216908U, 4949U, 59},
    {2221857U, 4948U, 58},
    {2226805U, 14846U, 59},
    {2256496U, 9897U, 63},
    {2266393U, 19794U, 76},
    {2286187U, 4948U, 68},
    {2291135U, 4949U, 66},
    {2296084U, 4948U, 66},
    {2301032U, 4948U, 66},
    {2305980U, 4949U, 64},
    {2310929U, 4948U, 64},
    {2315877U, 4949U, 63},
    {2320826U, 4948U, 61},
    {2325774U, 4949U, 61},
    {2330723U, 9897U, 61},
    {2340620U, 4948U, 66},
    {2345568U, 4949U, 66},
    {2350517U, 2474U, 64},
    {2352991U, 2474U, 64},
    {2355465U, 4949U, 64},
    {2360414U, 4948U, 64},
    {2365362U, 4948U, 63},
    {2370310U, 4949U, 61},
    {2375259U, 9897U, 61},
    {2385156U, 2474U, 59},
    {2387630U, 2474U, 58},
    {2390104U, 14846U, 59},
    {2404950U, 4948U, 68},
    {2409898U, 4949U, 66},
    {2414847U, 4948U, 66},
    {2419795U, 4948U, 66},
    {2424743U, 4949U, 64},
    {2429692U, 4948U, 64},
    {2434640U, 4949U, 63},
    {2439589U, 4948U, 61},
    {2444537U, 4949U, 61},
    {2449486U, 9897U, 61},
    {2459383U, 4948U, 63},
    {2464331U, 4949U, 64},
    {2469280U, 2474U, 63},
    {2471754U, 2474U, 63},
    {2474228U, 2474U, 63},
    {2476702U, 2474U, 63},
    {2479176U, 4949U, 63},
    {2484125U, 4948U, 61},
    {2489073U, 4949U, 63},
    {2494022U, 9897U, 73},
    {2503919U, 4948U, 71},
    {2508867U, 9897U, 71},
    {2518764U, 4949U, 71},
    {2523713U, 9896U, 70},
    {2533609U, 4949U, 68},
    {2538558U, 14845U, 68},
    {2563300U, 4949U, 68},
    {2568249U, 4948U, 68},
    {2573197U, 4949U, 66},
    {2578146U, 2474U, 64},
    {2580620U, 2474U, 64},
    {2583094U, 14845U, 64},
    {2597939U, 2475U, 63},
    {2600414U, 2474U, 64},
    {2602888U, 24742U, 66},
    {2642476U, 2474U, 61},
    {2644950U, 2474U, 63},
    {2647424U, 2474U, 61},
    {2649898U, 2474U, 63},
    {2652372U, 2475U, 64},
    {2654847U, 9897U, 66},
    {2664744U, 2474U, 64},
    {2667218U, 2474U, 66},
    {2672166U, 9897U, 68},
    {2682063U, 2474U, 68},
    {2684537U, 2475U, 70},
    {2687012U, 9897U, 71},
    {2696909U, 2474U, 73},
    {2699383U, 2474U, 71},
    {2701857U, 9897U, 66},
    {2711754U, 4948U, 66},
    {2716702U, 4949U, 66},
    {2721651U, 4948U, 64},
    {2726599U, 4949U, 64},
    {2731548U, 9897U, 63},
    {2741445U, 2474U, 63},
    {2743919U, 2474U, 64},
    {2746393U, 14845U, 66},
    {2761238U, 2475U, 63},
    {2763713U, 2474U, 61},
    {2766187U, 2474U, 63},
    {2768661U, 2474U, 61},
    {2771135U, 2475U, 63},
    {2773610U, 2474U, 64},
    {2776084U, 9897U, 66},
    {2785981U, 2474U, 64},
    {2788455U, 2474U, 66},
    {2790929U, 9897U, 68},
    {2800826U, 2474U, 67},
    {2803300U, 2475U, 68},
    {2805775U, 9896U, 70},
    {2818146U, 2474U, 67},
    {2820620U, 4948U, 75},
    {2825568U, 4949U, 75},
    {2832991U, 2474U, 68},
    {2835465U, 4949U, 76},
    {2840414U, 4948U, 75},
    {2845362U, 4949U, 73},
    {2850311U, 7422U, 73},
    {2857733U, 2475U, 71},
    {2860208U, 2474U, 71},
    {2862682U, 2474U, 70},
    {2865156U, 9897U, 71},
    {2875053U, 2474U, 66},
    {2877527U, 2474U, 71},
    {2880001U, 4949U, 73},
    {2884950U, 4948U, 71},
    {2889898U, 4949U, 71},
    {2894847U, 9897U, 71},
    {2904744U, 4948U, 66},
    {2909692U, 4949U, 73},
    {2914641U, 4948U, 71},
    {2919589U, 4948U, 71},
    {2924537U, 9897U, 71},
    {2934434U, 2475U, 66},
    {2936909U, 2474U, 71},
    {2939383U, 4948U, 73},
    {2944331U, 4949U, 71},
    {2949280U, 4948U, 71},
    {2954228U, 9897U, 71},
    {2964125U, 2474U, 66},
    {2966599U, 2475U, 71},
    {2969074U, 4948U, 73},
    {2974022U, 4949U, 71},
    {2978971U, 4948U, 71},
    {2983919U, 9897U, 71},
    {2993816U, 2474U, 66},
    {2996290U, 2474U, 71},
    {2998764U, 7423U, 73},
    {3006187U, 2474U, 75},
    {3008661U, 4949U, 73},
    {3013610U, 9897U, 71},
    {3023507U, 4948U, 71},
    {3028455U, 4949U, 70},
    {3033404U, 4948U, 68},
    {3038352U, 4948U, 68},
    {3043300U, 9897U, 68},
    {3053197U, 4949U, 66},
    {3058146U, 9897U, 66},
    {3068043U, 4948U, 64},
    {3072991U, 4949U, 64},
    {3077940U, 4948U, 63},
    {3082888U, 4949U, 61},
    {3087837U, 14845U, 63},
    {3117527U, 4949U, 63},
    {3122476U, 4948U, 64},
    {3127424U, 4949U, 63},
    {3132373U, 4948U, 64},
    {3137321U, 4949U, 63},
    {3142270U, 2474U, 61},
    {3144744U, 2474U, 59},
    {3147218U, 14845U, 59},
    {3162063U, 14846U, 59},
    {3176909U, 7422U, 75},
    {3184331U, 2475U, 76},
    {3186806U, 4948U, 75},
    {3191754U, 9897U, 73},
    {3201651U, 2474U, 59},
    {3204125U, 2474U, 61},
    {3206599U, 14846U, 71},
    {3221445U, 9897U, 71},
    {3231342U, 4948U, 74},
    {3236290U, 14846U, 71},
    {3251136U, 14845U, 73},
    {3265981U, 29691U, 75},
    {3300620U, 4949U, 74},
    {3305569U, 4948U, 75},
    {3310517U, 9897U, 78},
    {3320414U, 4948U, 73},
    {3325362U, 24743U, 71},
    {3350105U, 4948U, 74},
    {3355053U, 14846U, 71},
    {3369899U, 14845U, 73},
    {3384744U, 4948U, 75},
    {3389692U, 9897U, 74},
    {3404538U, 4948U, 81},
    {3409486U, 4949U, 80},
    {3414435U, 2474U, 74},
    {3416909U, 2474U, 73},
    {3419383U, 9897U, 71},
    {3429280U, 2474U, 59},
    {3431754U, 2474U, 61},
    {3434228U, 2475U, 59},
    {3436703U, 2474U, 61},
    {3439177U, 2474U, 59},
    {3441651U, 2474U, 61},
    {3444125U, 2475U, 73},
    {3446600U, 2474U, 71},
    {3449074U, 2474U, 71},
    {3451548U, 2474U, 71},
    {3454022U, 2475U, 71},
    {3456497U, 2474U, 71},
    {3458971U, 4948U, 74},
    {3463919U, 9897U, 71},
    {3473816U, 2474U, 73},
    {3476290U, 2475U, 71},
    {3478765U, 2474U, 71},
    {3481239U, 2474U, 71},
    {3483713U, 2474U, 71},
    {3486187U, 2474U, 71},
    {3488661U, 4949U, 73},
    {3493610U, 4948U, 71},
    {3498558U, 2475U, 71},
    {3501033U, 2474U, 68},
    {3503507U, 4948U, 66},
    {3508455U, 24743U, 75},
    {3562888U, 2475U, 73},
    {3565363U, 2474U, 71},
    {3567837U, 2474U, 71},
    {3570311U, 2474U, 71},
    {3572785U, 2474U, 71},
    {3575259U, 2475U, 71},
    {3577734U, 4948U, 74},
    {3582682U, 9897U, 71},
    {3592579U, 2474U, 73},
    {3595053U, 2475U, 71},
    {3597528U, 2474U, 71},
    {3600002U, 2474U, 71},
    {3602476U, 2474U, 71},
    {3604950U, 2474U, 71},
    {3607424U, 4949U, 73},
    {3612373U, 4948U, 71},
    {3617321U, 2475U, 71},
    {3619796U, 2474U, 68},
    {3622270U, 4948U, 66},
    {3627218U, 4949U, 75},
    {3632167U, 4948U, 73},
    {3637115U, 14846U, 73},
    {3651961U, 2474U, 73},
    {3654435U, 2474U, 73},
    {3656909U, 4948U, 73},
    {3661857U, 2475U, 66},
    {3664332U, 2474U, 66},
    {3666806U, 4948U, 73},
    {3671754U, 4949U, 71},
    {3676703U, 4948U, 71},
    {3681651U, 14846U, 71},
    {3696497U, 4948U, 71},
    {3701445U, 2474U, 71},
    {3703919U, 2475U, 68},
    {3706394U, 4948U, 71},
    {3711342U, 2474U, 71},
    {3713816U, 2474U, 71},
    {3716290U, 4949U, 73},
    {3721239U, 4948U, 71},
    {3726187U, 2475U, 73},
    {3728662U, 2474U, 73},
    {3731136U, 9897U, 71},
    {3755878U, 4949U, 73},
    {3760827U, 2474U, 73},
    {3763301U, 2474U, 71},
    {3765775U, 4948U, 73},
    {3770723U, 2475U, 73},
    {3773198U, 2474U, 73},
    {3775672U, 4948U, 75},
    {3780620U, 4949U, 73},
    {3785569U, 9897U, 75},
    {3795466U, 4948U, 76},
    {3800414U, 14846U, 75},
    {3815260U, 29690U, 75},
    {3844950U, 14846U, 75},
    {3859796U, 9897U, 75},
    {3869693U, 4948U, 73},
    {3874641U, 9897U, 71},
    {3884538U, 4948U, 73},
    {3889486U, 9897U, 75},
    {3899383U, 2475U, 76},
    {3901858U, 2474U, 75},
    {3904332U, 4948U, 73},
    {3909280U, 2474U, 75},
    {3911754U, 2475U, 73},
    {3914229U, 4948U, 71},
    {3919177U, 9897U, 75},
    {3929074U, 4949U, 73},
    {3934023U, 9896U, 71},
    {3943919U, 4949U, 73},
    {3948868U, 9897U, 75},
    {3958765U, 4948U, 76},
    {3963713U, 4949U, 79},
    {3968662U, 4948U, 75},
    {3973610U, 4949U, 73},
    {3978559U, 14845U, 73},
    {3993404U, 9897U, 79},
    {4003301U, 4948U, 82},
    {4008249U, 14846U, 80},
    {4023095U, 14845U, 80},
    {4037940U, 2474U, 63},
    {4040414U, 2475U, 61},
    {4042889U, 2474U, 63},
    {4045363U, 2474U, 61},
    {4047837U, 2474U, 63},
    {4050311U, 2474U, 64},
    {4052785U, 9897U, 66},
    {4062682U, 2475U, 64},
    {4065157U, 2474U, 66},
    {4067631U, 9897U, 68},
    {4077528U, 2474U, 68},
    {4080002U, 2474U, 70},
    {4082476U, 9897U, 71},
    {4092373U, 2474U, 73},
    {4094847U, 2475U, 71},
    {4097322U, 9896U, 66},
    {4107218U, 4949U, 66},
    {4112167U, 4948U, 66},
    {4117115U, 4949U, 64},
    {4122064U, 4948U, 64},
    {4127012U, 9897U, 63},
    {4136909U, 2474U, 63},
    {4139383U, 2475U, 64},
    {4141858U, 14845U, 66},
    {4156703U, 2474U, 63},
    {4159177U, 2474U, 61},
    {4161651U, 2475U, 63},
    {4164126U, 2474U, 61},
    {4166600U, 2474U, 63},
    {4169074U, 2474U, 64},
    {4171548U, 9897U, 66},
    {4181445U, 2475U, 64},
    {4183920U, 2474U, 66},
    {4186394U, 9897U, 68},
    {4196291U, 2474U, 67},
    {4198765U, 2474U, 68},
    {4201239U, 9897U, 70},
    {4213610U, 2475U, 67},
    {4216085U, 4948U, 75},
    {4221033U, 4948U, 75},
    {4228456U, 2474U, 68},
    {4230930U, 4948U, 76},
    {4235878U, 4949U, 75},
    {4240827U, 4948U, 73},
    {4245775U, 7423U, 73},
    {4253198U, 2474U, 71},
    {4255672U, 2474U, 71},
    {4258146U, 2475U, 70},
    {4260621U, 9897U, 71},
    {4270518U, 2474U, 66},
    {4272992U, 2474U, 71},
    {4275466U, 4948U, 73},
    {4280414U, 4949U, 71},
    {4285363U, 4948U, 71},
    {4290311U, 9897U, 71},
    {4300208U, 4949U, 66},
    {4305157U, 4948U, 73},
    {4310105U, 4949U, 71},
    {4315054U, 4948U, 71},
    {4320002U, 9897U, 71},
    {4329899U, 4948U, 71},
    {4334847U, 4949U, 73},
    {4339796U, 4948U, 71},
    {4344744U, 4949U, 71},
    {4349693U, 9897U, 71},
    {4359590U, 2474U, 71},
    {4362064U, 2474U, 71},
    {4364538U, 7423U, 73},
    {4371961U, 2474U, 75},
    {4374435U, 4949U, 73},
    {4379384U, 9896U, 71},
    {4389280U, 4949U, 71},
    {4394229U, 4948U, 70},
    {4399177U, 4949U, 68},
    {4404126U, 4948U, 68},
    {4409074U, 9897U, 68},
    {4418971U, 4949U, 66},
    {4423920U, 9897U, 66},
    {4433817U, 4948U, 64},
    {4438765U, 4948U, 64},
    {4443713U, 4949U, 63},
    {4448662U, 4948U, 61},
    {4453610U, 14846U, 63},
    {4483301U, 4949U, 63},
    {4488250U, 4948U, 64},
    {4493198U, 4949U, 63},
    {4498147U, 4948U, 64},
    {4503095U, 4948U, 63},
    {4508043U, 2475U, 61},
    {4510518U, 2474U, 59},
    {4512992U, 14845U, 59},
    {4542683U, 4948U, 64},
    {4547631U, 9897U, 64},
    {4557528U, 9897U, 67},
    {4567425U, 4948U, 71},
    {4572373U, 29691U, 71},
    {4602064U, 4949U, 71},
    {4607013U, 4948U, 73},
    {4611961U, 4948U, 71},
    {4616909U, 4949U, 73},
    {4621858U, 4948U, 71},
    {4626806U, 2475U, 73},
    {4629281U, 2474U, 73},
    {4631755U, 29691U, 71},
};
constexpr size_t SONG_SPRING_EVENT_COUNT = sizeof(SONG_SPRING_EVENTS) / sizeof(SONG_SPRING_EVENTS[0]);
constexpr uint32_t SONG_SPRING_DURATION_SAMPLES = 4661446U;

constexpr SongEvent SONG_GLORY_EVENTS[] = {
    {0U, 9000U, 65},
    {9000U, 9000U, 72},
    {18000U, 6000U, 72},
    {24000U, 6000U, 68},
    {30000U, 6000U, 67},
    {36000U, 9000U, 65},
    {45000U, 9000U, 72},
    {54000U, 6000U, 72},
    {60000U, 6000U, 68},
    {66000U, 6000U, 67},
    {72000U, 9000U, 65},
    {81000U, 9000U, 72},
    {90000U, 6000U, 72},
    {96000U, 6000U, 68},
    {102000U, 6000U, 67},
    {108000U, 18000U, 65},
    {126000U, 18000U, 64},
    {144000U, 6000U, 48},
    {150000U, 3000U, 52},
    {153000U, 6000U, 58},
    {159000U, 3000U, 52},
    {162000U, 6000U, 58},
    {168000U, 3000U, 52},
    {171000U, 6000U, 61},
    {177000U, 3000U, 58},
    {180000U, 6000U, 48},
    {186000U, 3000U, 52},
    {189000U, 6000U, 58},
    {195000U, 3000U, 52},
    {198000U, 6000U, 58},
    {204000U, 3000U, 52},
    {207000U, 6000U, 61},
    {213000U, 3000U, 58},
    {216000U, 6000U, 53},
    {222000U, 3000U, 53},
    {225000U, 6000U, 60},
    {231000U, 3000U, 53},
    {234000U, 6000U, 60},
    {240000U, 3000U, 53},
    {243000U, 6000U, 65},
    {249000U, 3000U, 60},
    {252000U, 6000U, 53},
    {258000U, 3000U, 53},
    {261000U, 6000U, 60},
    {267000U, 3000U, 53},
    {270000U, 6000U, 60},
    {276000U, 3000U, 53},
    {279000U, 6000U, 65},
    {285000U, 3000U, 60},
    {288000U, 6000U, 67},
    {294000U, 3000U, 67},
    {297000U, 6000U, 67},
    {303000U, 3000U, 67},
    {306000U, 6000U, 67},
    {312000U, 12000U, 65},
    {324000U, 6000U, 67},
    {330000U, 3000U, 67},
    {333000U, 9000U, 67},
    {342000U, 6000U, 65},
    {348000U, 12000U, 63},
    {360000U, 6000U, 65},
    {366000U, 3000U, 65},
    {369000U, 6000U, 65},
    {375000U, 3000U, 65},
    {378000U, 6000U, 65},
    {384000U, 6000U, 60},
    {390000U, 6000U, 63},
    {396000U, 18000U, 65},
    {414000U, 18000U, 63},
    {432000U, 6000U, 67},
    {438000U, 3000U, 67},
    {441000U, 6000U, 67},
    {447000U, 3000U, 67},
    {450000U, 6000U, 67},
    {456000U, 12000U, 65},
    {468000U, 6000U, 67},
    {474000U, 3000U, 67},
    {477000U, 9000U, 67},
    {486000U, 6000U, 65},
    {492000U, 12000U, 63},
    {504000U, 6000U, 65},
    {510000U, 6000U, 65},
    {516000U, 6000U, 65},
    {522000U, 6000U, 60},
    {528000U, 6000U, 60},
    {534000U, 6000U, 63},
    {540000U, 18000U, 65},
    {558000U, 18000U, 60},
    {576000U, 9000U, 72},
    {585000U, 9000U, 73},
    {594000U, 9000U, 72},
    {603000U, 9000U, 68},
    {612000U, 9000U, 70},
    {621000U, 9000U, 66},
    {630000U, 9000U, 65},
    {639000U, 9000U, 63},
    {648000U, 9000U, 65},
    {657000U, 9000U, 67},
    {666000U, 9000U, 68},
    {675000U, 9000U, 70},
    {684000U, 6000U, 70},
    {690000U, 3000U, 63},
    {693000U, 9000U, 67},
    {702000U, 9000U, 68},
    {711000U, 3000U, 65},
    {714000U, 3000U, 67},
    {717000U, 3000U, 68},
    {720000U, 15000U, 72},
    {735000U, 3000U, 72},
    {738000U, 6000U, 70},
    {744000U, 6000U, 68},
    {750000U, 6000U, 67},
    {756000U, 12000U, 65},
    {768000U, 6000U, 63},
    {774000U, 12000U, 60},
    {786000U, 6000U, 63},
    {792000U, 9000U, 65},
    {801000U, 3000U, 67},
    {804000U, 6000U, 68},
    {810000U, 6000U, 70},
    {816000U, 6000U, 63},
    {822000U, 6000U, 70},
    {828000U, 33000U, 72},
    {861000U, 3000U, 70},
    {864000U, 12000U, 72},
    {876000U, 6000U, 73},
    {882000U, 6000U, 75},
    {888000U, 6000U, 70},
    {894000U, 6000U, 68},
    {900000U, 6000U, 67},
    {906000U, 6000U, 68},
    {912000U, 6000U, 70},
    {918000U, 12000U, 72},
    {930000U, 3000U, 63},
    {933000U, 3000U, 63},
    {936000U, 12000U, 65},
    {948000U, 3000U, 68},
    {951000U, 3000U, 67},
    {954000U, 12000U, 65},
    {966000U, 6000U, 67},
    {972000U, 18000U, 65},
    {990000U, 6000U, 63},
    {996000U, 6000U, 61},
    {1002000U, 6000U, 63},
    {1008000U, 18000U, 63},
    {1026000U, 12000U, 70},
    {1038000U, 3000U, 68},
    {1041000U, 3000U, 67},
    {1044000U, 18000U, 65},
    {1062000U, 12000U, 63},
    {1074000U, 6000U, 67},
    {1080000U, 12000U, 72},
    {1092000U, 6000U, 70},
    {1098000U, 6000U, 67},
    {1104000U, 12000U, 65},
    {1116000U, 12000U, 67},
    {1128000U, 6000U, 65},
    {1134000U, 12000U, 64},
    {1146000U, 3000U, 72},
    {1149000U, 3000U, 74},
    {1152000U, 12000U, 75},
    {1164000U, 6000U, 74},
    {1170000U, 6000U, 67},
    {1176000U, 6000U, 65},
    {1182000U, 6000U, 63},
    {1188000U, 12000U, 65},
    {1200000U, 6000U, 62},
    {1206000U, 12000U, 67},
    {1218000U, 3000U, 60},
    {1221000U, 3000U, 63},
    {1224000U, 9000U, 63},
    {1233000U, 3000U, 60},
    {1236000U, 6000U, 63},
    {1242000U, 12000U, 65},
    {1254000U, 6000U, 70},
    {1260000U, 24000U, 72},
    {1284000U, 6000U, 74},
    {1290000U, 3000U, 75},
    {1293000U, 3000U, 75},
    {1296000U, 9000U, 65},
    {1296000U, 9000U, 72},
    {1305000U, 9000U, 72},
    {1314000U, 6000U, 72},
    {1320000U, 6000U, 68},
    {1326000U, 6000U, 67},
    {1332000U, 9000U, 65},
    {1341000U, 9000U, 72},
    {1350000U, 6000U, 72},
    {1356000U, 6000U, 68},
    {1362000U, 6000U, 67},
    {1368000U, 9000U, 65},
    {1377000U, 9000U, 72},
    {1386000U, 6000U, 72},
    {1392000U, 6000U, 68},
    {1398000U, 6000U, 67},
    {1404000U, 18000U, 65},
    {1422000U, 18000U, 64},
    {1440000U, 6000U, 67},
    {1446000U, 3000U, 67},
    {1449000U, 6000U, 67},
    {1455000U, 3000U, 67},
    {1458000U, 6000U, 67},
    {1464000U, 12000U, 65},
    {1476000U, 6000U, 67},
    {1482000U, 3000U, 67},
    {1485000U, 9000U, 67},
    {1494000U, 6000U, 65},
    {1500000U, 12000U, 63},
    {1512000U, 6000U, 65},
    {1518000U, 3000U, 65},
    {1521000U, 6000U, 65},
    {1527000U, 3000U, 65},
    {1530000U, 6000U, 65},
    {1536000U, 6000U, 60},
    {1542000U, 6000U, 63},
    {1548000U, 18000U, 65},
    {1566000U, 18000U, 63},
    {1584000U, 6000U, 67},
    {1590000U, 3000U, 67},
    {1593000U, 6000U, 67},
    {1599000U, 3000U, 67},
    {1602000U, 6000U, 67},
    {1608000U, 12000U, 65},
    {1620000U, 6000U, 67},
    {1626000U, 3000U, 67},
    {1629000U, 9000U, 67},
    {1638000U, 6000U, 65},
    {1644000U, 12000U, 63},
    {1656000U, 6000U, 65},
    {1662000U, 6000U, 65},
    {1668000U, 6000U, 65},
    {1674000U, 6000U, 60},
    {1680000U, 6000U, 60},
    {1686000U, 6000U, 63},
    {1692000U, 18000U, 65},
    {1710000U, 18000U, 60},
    {1728000U, 9000U, 72},
    {1737000U, 9000U, 73},
    {1746000U, 9000U, 72},
    {1755000U, 9000U, 68},
    {1764000U, 9000U, 70},
    {1773000U, 9000U, 66},
    {1782000U, 9000U, 65},
    {1791000U, 9000U, 63},
    {1800000U, 9000U, 65},
    {1809000U, 9000U, 67},
    {1818000U, 9000U, 68},
    {1827000U, 9000U, 70},
    {1836000U, 6000U, 70},
    {1842000U, 3000U, 63},
    {1845000U, 9000U, 67},
    {1854000U, 9000U, 68},
    {1863000U, 3000U, 65},
    {1866000U, 3000U, 67},
    {1869000U, 3000U, 68},
    {1872000U, 15000U, 72},
    {1887000U, 3000U, 72},
    {1890000U, 6000U, 70},
    {1896000U, 6000U, 68},
    {1902000U, 6000U, 67},
    {1908000U, 12000U, 65},
    {1920000U, 6000U, 63},
    {1926000U, 12000U, 60},
    {1938000U, 6000U, 63},
    {1944000U, 9000U, 65},
    {1953000U, 3000U, 67},
    {1956000U, 6000U, 68},
    {1962000U, 6000U, 70},
    {1968000U, 6000U, 63},
    {1974000U, 6000U, 70},
    {1980000U, 33000U, 72},
    {2013000U, 3000U, 70},
    {2016000U, 12000U, 72},
    {2028000U, 6000U, 73},
    {2034000U, 6000U, 75},
    {2040000U, 6000U, 70},
    {2046000U, 6000U, 68},
    {2052000U, 6000U, 67},
    {2058000U, 6000U, 68},
    {2064000U, 6000U, 70},
    {2070000U, 12000U, 72},
    {2082000U, 3000U, 63},
    {2085000U, 3000U, 63},
    {2088000U, 12000U, 65},
    {2100000U, 3000U, 68},
    {2103000U, 3000U, 67},
    {2106000U, 12000U, 65},
    {2118000U, 6000U, 67},
    {2124000U, 18000U, 65},
    {2142000U, 6000U, 63},
    {2148000U, 6000U, 61},
    {2154000U, 6000U, 63},
    {2160000U, 18000U, 63},
    {2178000U, 12000U, 70},
    {2190000U, 3000U, 68},
    {2193000U, 3000U, 67},
    {2196000U, 18000U, 65},
    {2214000U, 12000U, 63},
    {2226000U, 6000U, 67},
    {2232000U, 12000U, 72},
    {2244000U, 6000U, 70},
    {2250000U, 6000U, 67},
    {2256000U, 12000U, 65},
    {2268000U, 12000U, 67},
    {2280000U, 6000U, 65},
    {2286000U, 12000U, 64},
    {2298000U, 3000U, 72},
    {2301000U, 3000U, 74},
    {2304000U, 12000U, 75},
    {2316000U, 6000U, 74},
    {2322000U, 6000U, 67},
    {2328000U, 6000U, 65},
    {2334000U, 6000U, 63},
    {2340000U, 12000U, 65},
    {2352000U, 6000U, 62},
    {2358000U, 12000U, 67},
    {2370000U, 3000U, 60},
    {2373000U, 3000U, 63},
    {2376000U, 9000U, 63},
    {2385000U, 3000U, 60},
    {2388000U, 6000U, 63},
    {2394000U, 12000U, 65},
    {2406000U, 6000U, 70},
    {2412000U, 24000U, 72},
    {2436000U, 6000U, 74},
    {2442000U, 3000U, 75},
    {2445000U, 3000U, 75},
    {2448000U, 9000U, 65},
    {2448000U, 9000U, 72},
    {2457000U, 9000U, 72},
    {2466000U, 6000U, 72},
    {2472000U, 6000U, 68},
    {2478000U, 6000U, 67},
    {2484000U, 9000U, 65},
    {2493000U, 9000U, 72},
    {2502000U, 6000U, 72},
    {2508000U, 6000U, 68},
    {2514000U, 6000U, 67},
    {2520000U, 9000U, 65},
    {2529000U, 9000U, 72},
    {2538000U, 6000U, 72},
    {2544000U, 6000U, 68},
    {2550000U, 6000U, 67},
    {2556000U, 18000U, 65},
    {2574000U, 18000U, 64},
    {2592000U, 6000U, 65},
    {2592000U, 6000U, 72},
    {2598000U, 6000U, 72},
    {2604000U, 6000U, 73},
    {2610000U, 12000U, 72},
    {2622000U, 6000U, 68},
    {2628000U, 6000U, 67},
    {2634000U, 6000U, 67},
    {2640000U, 6000U, 68},
    {2646000U, 12000U, 67},
    {2658000U, 6000U, 63},
    {2664000U, 6000U, 72},
    {2670000U, 6000U, 72},
    {2676000U, 6000U, 73},
    {2682000U, 12000U, 72},
    {2694000U, 6000U, 65},
    {2700000U, 6000U, 68},
    {2706000U, 6000U, 68},
    {2712000U, 6000U, 70},
    {2718000U, 12000U, 67},
    {2730000U, 6000U, 67},
    {2736000U, 6000U, 68},
    {2742000U, 6000U, 68},
    {2748000U, 6000U, 70},
    {2754000U, 12000U, 72},
    {2766000U, 3000U, 68},
    {2769000U, 3000U, 70},
    {2772000U, 12000U, 70},
    {2784000U, 3000U, 68},
    {2787000U, 3000U, 70},
    {2790000U, 9000U, 72},
    {2799000U, 3000U, 73},
    {2802000U, 6000U, 73},
    {2808000U, 6000U, 72},
    {2814000U, 6000U, 77},
    {2820000U, 33000U, 65},
    {2853000U, 3000U, 77},
    {2856000U, 3000U, 79},
    {2859000U, 3000U, 80},
    {2862000U, 9000U, 84},
    {2871000U, 6000U, 84},
    {2877000U, 3000U, 84},
    {2880000U, 6000U, 82},
    {2886000U, 6000U, 80},
    {2892000U, 6000U, 79},
    {2898000U, 6000U, 77},
    {2904000U, 6000U, 77},
    {2910000U, 6000U, 75},
    {2916000U, 12000U, 72},
    {2928000U, 6000U, 75},
    {2934000U, 9000U, 77},
    {2943000U, 3000U, 79},
    {2946000U, 6000U, 80},
    {2952000U, 6000U, 82},
    {2958000U, 6000U, 75},
    {2964000U, 6000U, 82},
    {2970000U, 30000U, 84},
    {3000000U, 3000U, 84},
    {3003000U, 3000U, 82},
    {3006000U, 12000U, 84},
    {3018000U, 6000U, 85},
    {3024000U, 6000U, 87},
    {3030000U, 6000U, 82},
    {3036000U, 6000U, 80},
    {3042000U, 6000U, 79},
    {3048000U, 6000U, 80},
    {3054000U, 6000U, 82},
    {3060000U, 12000U, 84},
    {3072000U, 3000U, 75},
    {3075000U, 3000U, 75},
    {3078000U, 12000U, 77},
    {3090000U, 3000U, 80},
    {3093000U, 3000U, 79},
    {3096000U, 12000U, 77},
    {3108000U, 6000U, 79},
    {3114000U, 18000U, 77},
    {3132000U, 12000U, 75},
    {3144000U, 3000U, 72},
    {3147000U, 3000U, 75},
    {3150000U, 18000U, 75},
    {3168000U, 12000U, 82},
    {3180000U, 3000U, 80},
    {3183000U, 3000U, 79},
    {3186000U, 18000U, 77},
    {3204000U, 6000U, 75},
    {3210000U, 6000U, 80},
    {3216000U, 6000U, 82},
    {3222000U, 12000U, 84},
    {3234000U, 6000U, 82},
    {3240000U, 6000U, 79},
    {3246000U, 12000U, 77},
    {3258000U, 12000U, 79},
    {3270000U, 6000U, 77},
    {3276000U, 12000U, 76},
    {3288000U, 3000U, 72},
    {3291000U, 3000U, 74},
    {3294000U, 12000U, 75},
    {3306000U, 6000U, 75},
    {3312000U, 6000U, 67},
    {3318000U, 6000U, 65},
    {3324000U, 6000U, 63},
    {3330000U, 6000U, 65},
    {3336000U, 6000U, 63},
    {3342000U, 6000U, 62},
    {3348000U, 12000U, 67},
    {3360000U, 3000U, 60},
    {3363000U, 3000U, 62},
    {3366000U, 9000U, 63},
    {3375000U, 3000U, 60},
    {3378000U, 6000U, 63},
    {3384000U, 12000U, 65},
    {3396000U, 6000U, 70},
    {3402000U, 24000U, 72},
    {3426000U, 6000U, 74},
    {3432000U, 3000U, 75},
    {3435000U, 3000U, 75},
    {3438000U, 9000U, 65},
    {3438000U, 9000U, 72},
    {3447000U, 9000U, 72},
    {3456000U, 6000U, 72},
    {3462000U, 6000U, 68},
    {3468000U, 6000U, 67},
    {3474000U, 9000U, 65},
    {3483000U, 9000U, 72},
    {3492000U, 6000U, 72},
    {3498000U, 6000U, 68},
    {3504000U, 6000U, 67},
    {3510000U, 9000U, 65},
    {3519000U, 9000U, 72},
    {3528000U, 6000U, 72},
    {3534000U, 6000U, 68},
    {3540000U, 54000U, 67},
    {3600000U, 36000U, 77},
    {3636000U, 24000U, 77},
};
constexpr size_t SONG_GLORY_EVENT_COUNT = sizeof(SONG_GLORY_EVENTS) / sizeof(SONG_GLORY_EVENTS[0]);
constexpr uint32_t SONG_GLORY_DURATION_SAMPLES = 3660000U;

constexpr SongEvent SONG_REVERSALSISTERS_EVENTS[] = {
    {32308U, 2307U, 76},
    {34615U, 2308U, 77},
    {36923U, 18462U, 52},
    {36923U, 6923U, 71},
    {36923U, 6923U, 79},
    {43846U, 6923U, 79},
    {50769U, 9231U, 69},
    {50769U, 9231U, 72},
    {55385U, 18461U, 53},
    {60000U, 4615U, 76},
    {64615U, 4616U, 74},
    {69231U, 4615U, 72},
    {73846U, 18462U, 55},
    {73846U, 4616U, 69},
    {73846U, 4616U, 71},
    {78462U, 4615U, 72},
    {83077U, 4615U, 74},
    {87692U, 9231U, 67},
    {87692U, 9231U, 72},
    {92308U, 18461U, 57},
    {96923U, 4615U, 64},
    {106154U, 2308U, 76},
    {108462U, 2307U, 77},
    {110769U, 18462U, 52},
    {110769U, 6923U, 71},
    {110769U, 6923U, 79},
    {117692U, 6923U, 79},
    {124615U, 9231U, 69},
    {124615U, 9231U, 72},
    {129231U, 18461U, 53},
    {133846U, 4616U, 76},
    {138462U, 4615U, 74},
    {143077U, 4615U, 72},
    {147692U, 6923U, 55},
    {147692U, 6923U, 69},
    {147692U, 6923U, 74},
    {154615U, 6923U, 72},
    {154615U, 6923U, 79},
    {161538U, 18462U, 57},
    {161538U, 9231U, 72},
    {161538U, 9231U, 76},
    {170769U, 4616U, 67},
    {180000U, 2308U, 76},
    {182308U, 2307U, 77},
    {184615U, 18462U, 52},
    {184615U, 6923U, 71},
    {184615U, 6923U, 79},
    {191538U, 6924U, 79},
    {198462U, 9230U, 69},
    {198462U, 9230U, 72},
    {203077U, 18461U, 53},
    {207692U, 4616U, 76},
    {212308U, 4615U, 74},
    {216923U, 4615U, 72},
    {221538U, 4616U, 69},
    {221538U, 4616U, 71},
    {226154U, 4615U, 72},
    {230769U, 4616U, 74},
    {235385U, 9230U, 67},
    {235385U, 9230U, 72},
    {244615U, 4616U, 64},
    {253846U, 2308U, 76},
    {256154U, 2308U, 77},
    {258462U, 18461U, 52},
    {258462U, 6923U, 71},
    {258462U, 6923U, 79},
    {265385U, 6923U, 79},
    {272308U, 9230U, 79},
    {272308U, 9230U, 84},
    {281538U, 13847U, 72},
    {295385U, 18461U, 55},
    {295385U, 4615U, 67},
    {300000U, 4615U, 72},
    {304615U, 4616U, 71},
    {309231U, 18461U, 67},
    {309231U, 18461U, 72},
    {313846U, 4615U, 59},
    {318461U, 4616U, 62},
    {323077U, 4615U, 67},
    {327692U, 2308U, 76},
    {330000U, 2308U, 77},
    {332308U, 18461U, 52},
    {332308U, 6923U, 71},
    {332308U, 6923U, 79},
    {339231U, 6923U, 79},
    {346154U, 9231U, 69},
    {346154U, 9231U, 72},
    {350769U, 18462U, 53},
    {355385U, 4615U, 76},
    {360000U, 4615U, 74},
    {364615U, 4616U, 72},
    {369231U, 18461U, 55},
    {369231U, 4615U, 69},
    {369231U, 4615U, 71},
    {373846U, 4615U, 72},
    {378461U, 4616U, 74},
    {383077U, 9231U, 67},
    {383077U, 9231U, 72},
    {387692U, 18462U, 57},
    {392308U, 4615U, 64},
    {401538U, 2308U, 76},
    {403846U, 2308U, 77},
    {406154U, 18461U, 52},
    {406154U, 6923U, 71},
    {406154U, 6923U, 79},
    {413077U, 6923U, 79},
    {420000U, 9231U, 69},
    {420000U, 9231U, 72},
    {429231U, 4615U, 76},
    {433846U, 4615U, 74},
    {438461U, 4616U, 72},
    {443077U, 6923U, 55},
    {443077U, 6923U, 69},
    {443077U, 6923U, 74},
    {450000U, 6923U, 72},
    {450000U, 6923U, 79},
    {456923U, 9231U, 57},
    {456923U, 9231U, 72},
    {456923U, 9231U, 76},
    {466154U, 4615U, 67},
    {475385U, 2307U, 76},
    {477692U, 2308U, 77},
    {480000U, 18461U, 52},
    {480000U, 6923U, 71},
    {480000U, 6923U, 79},
    {486923U, 6923U, 79},
    {493846U, 9231U, 69},
    {493846U, 9231U, 72},
    {498461U, 18462U, 53},
    {503077U, 4615U, 76},
    {507692U, 4616U, 74},
    {512308U, 4615U, 72},
    {516923U, 4615U, 69},
    {516923U, 4615U, 71},
    {521538U, 4616U, 72},
    {526154U, 4615U, 74},
    {530769U, 9231U, 67},
    {530769U, 9231U, 72},
    {535385U, 18461U, 57},
    {540000U, 4615U, 64},
    {549231U, 2307U, 76},
    {551538U, 2308U, 77},
    {553846U, 18462U, 52},
    {553846U, 6923U, 71},
    {553846U, 6923U, 79},
    {560769U, 6923U, 79},
    {567692U, 9231U, 79},
    {567692U, 9231U, 84},
    {576923U, 13846U, 72},
    {590769U, 18462U, 55},
    {590769U, 4616U, 67},
    {595385U, 4615U, 72},
    {600000U, 4615U, 71},
    {604615U, 18462U, 67},
    {604615U, 18462U, 72},
    {609231U, 18461U, 59},
    {623077U, 4615U, 67},
    {627692U, 18462U, 52},
    {627692U, 6923U, 74},
    {627692U, 6923U, 79},
    {634615U, 6923U, 79},
    {641538U, 9231U, 72},
    {641538U, 9231U, 79},
    {646154U, 18461U, 57},
    {650769U, 4616U, 77},
    {655385U, 4615U, 76},
    {660000U, 2308U, 74},
    {662308U, 2307U, 72},
    {664615U, 18462U, 55},
    {664615U, 6923U, 67},
    {664615U, 6923U, 74},
    {671538U, 6923U, 76},
    {678461U, 4616U, 67},
    {678461U, 4616U, 72},
    {683077U, 4615U, 55},
    {683077U, 9231U, 74},
    {687692U, 9231U, 55},
    {692308U, 9230U, 76},
    {701538U, 18462U, 52},
    {701538U, 6923U, 71},
    {701538U, 6923U, 79},
    {708461U, 6924U, 79},
    {715385U, 9230U, 79},
    {715385U, 9230U, 84},
    {724615U, 9231U, 72},
    {733846U, 4615U, 72},
    {733846U, 4615U, 77},
    {738461U, 18462U, 55},
    {738461U, 4616U, 77},
    {743077U, 9231U, 76},
    {752308U, 9230U, 72},
    {761538U, 9231U, 74},
    {770769U, 2308U, 76},
    {773077U, 2308U, 77},
    {775385U, 6923U, 74},
    {775385U, 6923U, 79},
    {782308U, 9230U, 57},
    {782308U, 6923U, 79},
    {789231U, 9230U, 72},
    {789231U, 9230U, 79},
    {798461U, 4616U, 77},
    {803077U, 4615U, 76},
    {807692U, 2308U, 74},
    {810000U, 2308U, 72},
    {812308U, 9230U, 55},
    {812308U, 6923U, 67},
    {812308U, 6923U, 74},
    {819231U, 6923U, 76},
    {826154U, 4615U, 67},
    {826154U, 4615U, 72},
    {830769U, 9231U, 74},
    {835385U, 4615U, 55},
    {840000U, 9231U, 62},
    {840000U, 9231U, 76},
    {849231U, 6923U, 71},
    {849231U, 6923U, 79},
    {853846U, 18462U, 52},
    {856154U, 6923U, 79},
    {863077U, 9231U, 79},
    {863077U, 9231U, 84},
    {872308U, 9230U, 72},
    {881538U, 4616U, 67},
    {881538U, 4616U, 72},
    {881538U, 4616U, 77},
    {886154U, 18461U, 55},
    {886154U, 4615U, 77},
    {890769U, 9231U, 76},
    {900000U, 9231U, 72},
    {904615U, 18462U, 55},
    {909231U, 9230U, 74},
    {918461U, 2308U, 72},
    {920769U, 2308U, 74},
    {923077U, 13846U, 55},
    {923077U, 36923U, 72},
    {936923U, 13846U, 57},
    {950769U, 9231U, 57},
    {960000U, 36923U, 59},
    {996923U, 4615U, 55},
    {996923U, 36923U, 67},
    {996923U, 36923U, 72},
    {1001538U, 9231U, 57},
    {1010769U, 18462U, 57},
    {1029231U, 4615U, 57},
    {1033846U, 18462U, 57},
    {1033846U, 36923U, 70},
    {1033846U, 36923U, 74},
    {1052308U, 13846U, 57},
    {1070769U, 9231U, 55},
    {1070769U, 36923U, 67},
    {1070769U, 36923U, 72},
    {1070769U, 36923U, 76},
    {1080000U, 9231U, 57},
    {1089231U, 18461U, 57},
    {1107692U, 36923U, 73},
    {1107692U, 36923U, 77},
    {1126154U, 18461U, 55},
    {1144615U, 18462U, 59},
    {1144615U, 36923U, 74},
    {1144615U, 36923U, 77},
    {1163077U, 18461U, 55},
    {1186154U, 4615U, 72},
    {1190769U, 2308U, 74},
    {1193077U, 4615U, 72},
    {1197692U, 4616U, 74},
    {1202308U, 4615U, 74},
    {1206923U, 2308U, 72},
    {1209231U, 4615U, 74},
    {1213846U, 4615U, 72},
    {1223077U, 4615U, 72},
    {1227692U, 2308U, 74},
    {1230000U, 4615U, 72},
    {1234615U, 4616U, 74},
    {1239231U, 4615U, 74},
    {1243846U, 2308U, 72},
    {1246154U, 4615U, 74},
    {1250769U, 2308U, 76},
    {1253077U, 2307U, 77},
    {1255384U, 6924U, 67},
    {1255384U, 6924U, 71},
    {1255384U, 6924U, 74},
    {1255384U, 6924U, 79},
    {1262308U, 6923U, 76},
    {1262308U, 6923U, 79},
    {1269231U, 9230U, 72},
    {1269231U, 9230U, 79},
    {1278461U, 4616U, 77},
    {1283077U, 4615U, 76},
    {1287692U, 2308U, 74},
    {1290000U, 2308U, 72},
    {1292308U, 9230U, 55},
    {1292308U, 6923U, 67},
    {1292308U, 6923U, 74},
    {1299231U, 6923U, 76},
    {1306154U, 4615U, 67},
    {1306154U, 4615U, 72},
    {1310769U, 9231U, 74},
    {1315384U, 4616U, 55},
    {1320000U, 9231U, 62},
    {1320000U, 9231U, 76},
    {1329231U, 18461U, 52},
    {1329231U, 6923U, 71},
    {1329231U, 6923U, 79},
    {1336154U, 6923U, 79},
    {1343077U, 9231U, 79},
    {1343077U, 9231U, 84},
    {1352308U, 9230U, 72},
    {1361538U, 4616U, 72},
    {1361538U, 4616U, 77},
    {1366154U, 4615U, 77},
    {1370769U, 9231U, 76},
    {1380000U, 9231U, 72},
    {1389231U, 9230U, 74},
    {1398461U, 2308U, 76},
    {1400769U, 2308U, 77},
    {1403077U, 18461U, 52},
    {1403077U, 6923U, 74},
    {1403077U, 6923U, 79},
    {1410000U, 6923U, 79},
    {1416923U, 9231U, 72},
    {1416923U, 9231U, 79},
    {1421538U, 18462U, 57},
    {1426154U, 4615U, 77},
    {1430769U, 4615U, 76},
    {1435384U, 2308U, 74},
    {1437692U, 2308U, 72},
    {1440000U, 18461U, 55},
    {1440000U, 6923U, 67},
    {1440000U, 6923U, 74},
    {1446923U, 6923U, 76},
    {1453846U, 4615U, 67},
    {1453846U, 4615U, 72},
    {1458461U, 9231U, 74},
    {1467692U, 4615U, 55},
    {1467692U, 9231U, 76},
    {1472307U, 4616U, 62},
    {1476923U, 18461U, 52},
    {1476923U, 6923U, 71},
    {1476923U, 6923U, 79},
    {1483846U, 6923U, 79},
    {1490769U, 9231U, 79},
    {1490769U, 9231U, 84},
    {1495384U, 18462U, 53},
    {1500000U, 9231U, 72},
    {1509231U, 4615U, 67},
    {1509231U, 4615U, 72},
    {1509231U, 4615U, 77},
    {1513846U, 18461U, 55},
    {1513846U, 4615U, 77},
    {1518461U, 9231U, 76},
    {1527692U, 9231U, 72},
    {1532307U, 18462U, 57},
    {1536923U, 9231U, 74},
    {1546154U, 2307U, 72},
    {1548461U, 2308U, 74},
    {1550769U, 18462U, 55},
    {1550769U, 18462U, 72},
    {1569231U, 9230U, 55},
    {1569231U, 18461U, 67},
    {1569231U, 18461U, 74},
    {1578461U, 9231U, 55},
    {1587692U, 36923U, 60},
    {1587692U, 36923U, 67},
    {1587692U, 36923U, 76},
};
constexpr size_t SONG_REVERSALSISTERS_EVENT_COUNT = sizeof(SONG_REVERSALSISTERS_EVENTS) / sizeof(SONG_REVERSALSISTERS_EVENTS[0]);
constexpr uint32_t SONG_REVERSALSISTERS_DURATION_SAMPLES = 1624615U;

struct CatalogSong {
  char displayName[MSPKG_TITLE_BYTES + 1];
  char path[MAX_CATALOG_PATH_BYTES];
  const SongEvent *events;
  size_t eventCount;
  uint32_t durationSamples;
  SongEvent *ownedEvents;
  bool fromFile;
  uint32_t packageSourceHash;
};

const CatalogSong AUDIO_SAFE_EMBEDDED_SONG = {
    "VOYAGER FAREWELL", "", SONG_VOYAGERFAREWELL_EVENTS,
    SONG_VOYAGERFAREWELL_EVENT_COUNT, SONG_VOYAGERFAREWELL_DURATION_SAMPLES,
    nullptr, false, 0};

CatalogSong songCatalog[MAX_RUNTIME_SONGS] = {
    {"VOYAGER FAREWELL", "", SONG_VOYAGERFAREWELL_EVENTS,
     SONG_VOYAGERFAREWELL_EVENT_COUNT, SONG_VOYAGERFAREWELL_DURATION_SAMPLES,
     nullptr, false, 0},
    {"SPRING", "", SONG_SPRING_EVENTS,
     SONG_SPRING_EVENT_COUNT, SONG_SPRING_DURATION_SAMPLES, nullptr, false, 0},
    {"GLORY SKY", "", SONG_GLORY_EVENTS,
     SONG_GLORY_EVENT_COUNT, SONG_GLORY_DURATION_SAMPLES, nullptr, false, 0},
    {"REVERSAL SISTERS", "", SONG_REVERSALSISTERS_EVENTS,
     SONG_REVERSALSISTERS_EVENT_COUNT, SONG_REVERSALSISTERS_DURATION_SAMPLES,
     nullptr, false, 0},
};
volatile uint8_t songCatalogCount = EMBEDDED_SONG_COUNT;
static_assert(EMBEDDED_SONG_COUNT == 4, "Unexpected embedded song count");
static_assert(SONG_VOYAGERFAREWELL_EVENT_COUNT == 335,
              "Unexpected Voyager event count");
static_assert(SONG_SPRING_EVENT_COUNT == 759,
              "Unexpected Spring event count");
static_assert(SONG_GLORY_EVENT_COUNT == 479,
              "Unexpected Glory event count");
static_assert(SONG_REVERSALSISTERS_EVENT_COUNT == 363,
              "Unexpected Reversal Sisters event count");

I2SClass i2s;
BLEServer *globalBleServer = nullptr;
BLEHIDDevice *keyboardHidDevice = nullptr;
BLECharacteristic *keyboardInputReport = nullptr;
QueueHandle_t keyboardReportQueue = nullptr;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE,
    MINI_SYNTH_PIN_I2C_SCL,
    MINI_SYNTH_PIN_I2C_SDA);

StereoFrame audioFrames[AUDIO_FRAMES_PER_BLOCK];
int16_t sineTable[SINE_TABLE_SIZE];
uint32_t basePhaseSteps[NOTE_COUNT];
float noteGainByOctave[3][NOTE_COUNT];  // rows: down/base/up octave.
uint32_t phases[NOTE_COUNT] = {};
float envelopes[NOTE_COUNT] = {};
float currentPhaseSteps[NOTE_COUNT] = {};
float glideStartPhaseSteps[NOTE_COUNT] = {};
float glideTargetPhaseSteps[NOTE_COUNT] = {};
float currentPitchGains[NOTE_COUNT] = {};
float glideStartPitchGains[NOTE_COUNT] = {};
float glideTargetPitchGains[NOTE_COUNT] = {};
float currentNormalizationPitchGains[NOTE_COUNT] = {};
float glideStartNormalizationPitchGains[NOTE_COUNT] = {};
float glideTargetNormalizationPitchGains[NOTE_COUNT] = {};
uint16_t glideSamplesRemaining = 0;
uint8_t debounceCounters[INPUT_COUNT] = {};
bool debouncedPressed[INPUT_COUNT] = {};
uint8_t previousInputNoteMask = 0;
bool previousOctaveDownPressed = false;
bool previousOctaveUpPressed = false;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t sharedNoteMask = 0;
volatile int8_t sharedOctaveOffset = 0;
volatile AppId sharedActiveApp = AppId::Play;
volatile AppId sharedLastResumableApp = AppId::Play;
volatile SongTimbre sharedPlayTimbre = SongTimbre::Sine;
volatile PlayOctaveMode sharedPlayOctaveMode = PlayOctaveMode::Hold;
volatile int8_t sharedLatchedPlayOctave = 0;
volatile uint8_t sharedGlobalVolumePercent = DEFAULT_GLOBAL_VOLUME_PERCENT;
volatile bool sharedPitchEqEnabled = DEFAULT_PITCH_EQ_ENABLED;
volatile SettingRow sharedSettingRow = SettingRow::Volume;
volatile uint32_t sharedSettingsRevision = 1;
volatile uint32_t sharedSettingsSaveRevision = 0;
volatile bool sharedSettingsSavePending = false;
volatile SongStatus sharedSongStatus = SongStatus::Stopped;
volatile SongTimbre sharedSongTimbre = SongTimbre::Sine;
volatile uint8_t sharedSongIndex = 0;
volatile bool sharedSongRestartRequested = false;
volatile uint32_t sharedSongPositionSamples = 0;
volatile uint8_t sharedSongPitch = 0;
volatile SongLoadState sharedSongLoadState = SongLoadState::Ready;
volatile bool audioTaskHealthy = false;
volatile bool audioWriteFailed = false;
volatile bool sharedKeyboardConnected = false;
volatile bool sharedKeyboardAppActive = false;
volatile bool sharedKeyboardReleaseGate = true;
volatile bool sharedKeyboardNeedsInitialZero = false;
volatile bool sharedKeyboardRestartAdvertising = false;
volatile GlobalBleCommand sharedGlobalBleCommand = GlobalBleCommand::None;
volatile bool sharedGlobalBlePairingActive = false;
volatile uint32_t sharedGlobalBlePairingDeadlineMs = 0;
volatile uint32_t sharedGlobalBlePairingGeneration = 0;
volatile bool sharedGlobalBleCurrentPeerValid = false;
volatile uint8_t sharedGlobalBleCurrentPeerAddressType = 0;
volatile uint8_t sharedGlobalBleCurrentPeerAddress[6] = {};
volatile bool sharedGlobalBleBlockedPeerActive = false;
volatile uint8_t sharedGlobalBleBlockedPeerAddressType = 0;
volatile uint8_t sharedGlobalBleBlockedPeerAddress[6] = {};
volatile uint32_t sharedKeyboardTransportEpoch = 1;
volatile uint16_t sharedKeyboardPhysicalMask = 0;
volatile KeyboardBleState sharedKeyboardBleState = KeyboardBleState::Off;

bool i2sReady = false;
bool displayReady = false;
uint32_t lastUiRefreshAt = 0;
uint8_t lastDisplayedNoteMask = 0;
int8_t lastDisplayedOctaveOffset = 0;
AppId lastDisplayedApp = AppId::Play;
bool globalHomeChordTracking = false;
bool globalHomeChordTriggered = false;
bool globalBlePairTriggered = false;
uint32_t globalHomeChordStartedAt = 0;
bool suppressInputsUntilAllReleased = false;
SongStatus lastDisplayedSongStatus = SongStatus::Stopped;
SongTimbre lastDisplayedSongTimbre = SongTimbre::Sine;
SongTimbre lastDisplayedPlayTimbre = SongTimbre::Sine;
uint8_t lastDisplayedSongIndex = 0;
uint32_t lastDisplayedSongSecond = 0;
bool ffatMounted = false;
volatile bool sharedKeyboardBleReady = false;
volatile bool sharedKeyboardBleInitAttempted = false;
uint8_t ffatAcceptedPackages = 0;
uint8_t ffatRejectedPackages = 0;
int8_t pendingLatchedOctaveDirection = 0;
uint32_t lastDisplayedSettingsRevision = 0;
SongLoadState lastDisplayedSongLoadState = SongLoadState::Ready;
uint16_t lastDisplayedKeyboardPhysicalMask = 0xFFFF;
KeyboardBleState lastDisplayedKeyboardBleState = KeyboardBleState::Off;
bool lastDisplayedKeyboardReleaseGate = true;
bool lastDisplayedGlobalBlePairingActive = false;
uint8_t lastDisplayedGlobalBlePairingSecond = 0xFF;

// USB protocol state is serviced only by loopTask. The real-time tasks see
// only the catalog refresh gate/ack under stateMux; they never touch Serial or
// FFat and never wait for a refresh.
UsbDeviceState usbDeviceState = USB_STATE_IDLE;
UsbErrorCode usbLastResult = USB_ERR_OK;
char usbUploadFilename[MAX_USB_FILENAME_BYTES + 1] = {};
uint32_t usbUploadLength = 0;
uint32_t usbUploadExpectedCrc = 0;
uint32_t usbUploadRunningCrc = 0;
uint32_t usbUploadOffset = 0;
uint32_t usbLastUploadActivityMs = 0;
File usbUploadFile;
uint8_t usbFramePayload[MAX_USB_FRAME_PAYLOAD];
UsbFrameHeader usbIncomingHeader = {};
uint8_t usbIncomingHeaderBytes = 0;
uint32_t usbIncomingPayloadBytes = 0;
uint8_t usbMagicMatchBytes = 0;
uint32_t usbLastParserByteMs = 0;
bool usbResponseCacheValid = false;
uint32_t usbCachedSequence = 0;
uint8_t usbCachedRequestCommand = 0;
uint8_t usbCachedResponseCommand = 0;
UsbAckPayload usbCachedAck = {};
UsbOverlayState usbOverlayState = UsbOverlayState::Hidden;
uint32_t usbOverlayExpiresAt = 0;
uint32_t usbOverlayRevision = 0;
uint32_t lastDisplayedUsbOverlayRevision = UINT32_MAX;

volatile bool sharedCatalogRefreshGate = false;
volatile bool sharedAudioCatalogQuiescent = false;
bool runtimeCatalogRefreshPending = false;
bool runtimeCatalogRefreshStarted = false;
volatile bool selectedSongLoadPending = false;
bool selectedSongLoadStarted = false;
char runtimeCatalogPreferredPath[MAX_USB_FILENAME_BYTES + 2] = {};

KeyboardNkroReport buildKeyboardReport(uint16_t physicalMask) {
  physicalMask &= KEYBOARD_VALID_PHYSICAL_MASK;
  KeyboardNkroReport report = {};
  if ((physicalMask & KEYBOARD_SW9_MASK) != 0) {
    report.modifiers |= KEYBOARD_LEFT_ALT_MODIFIER;
    // The global Home gesture is SW8+SW9. Suppress R for the entire time Left
    // Alt is held so entering Home cannot leak an R action to the game.
    physicalMask &= static_cast<uint16_t>(~KEYBOARD_SW8_MASK);
  }
  if (physicalMask & (1U << 0)) report.gameKeys |= 1U << 0;
  if (physicalMask & (1U << 1)) report.gameKeys |= 1U << 1;
  if (physicalMask & (1U << 2)) report.gameKeys |= 1U << 2;
  if (physicalMask & (1U << 7)) report.gameKeys |= 1U << 3;
  if (physicalMask & (1U << 3)) report.gameKeys |= 1U << 4;
  if (physicalMask & (1U << 4)) report.gameKeys |= 1U << 5;
  if (physicalMask & (1U << 5)) report.gameKeys |= 1U << 6;
  if (physicalMask & (1U << 6)) report.gameKeys |= 1U << 7;
  return report;
}

KeyboardTransportSnapshot readKeyboardTransportSnapshot() {
  KeyboardTransportSnapshot snapshot;
  portENTER_CRITICAL(&stateMux);
  snapshot.connected = sharedKeyboardConnected;
  snapshot.appActive = sharedKeyboardAppActive;
  snapshot.releaseGate = sharedKeyboardReleaseGate;
  snapshot.needsInitialZero = sharedKeyboardNeedsInitialZero;
  snapshot.restartAdvertising = sharedKeyboardRestartAdvertising;
  snapshot.epoch = sharedKeyboardTransportEpoch;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

void queueKeyboardMaskTransition(uint16_t physicalMask, bool flushFirst = false) {
  if (keyboardReportQueue == nullptr) return;
  KeyboardReportRequest request;
  portENTER_CRITICAL(&stateMux);
  request.epoch = sharedKeyboardTransportEpoch;
  portEXIT_CRITICAL(&stateMux);
  request.physicalMask = physicalMask & KEYBOARD_VALID_PHYSICAL_MASK;
  if (flushFirst) xQueueReset(keyboardReportQueue);
  if (xQueueSend(keyboardReportQueue, &request, 0) != pdTRUE) {
    xQueueReset(keyboardReportQueue);
    xQueueSend(keyboardReportQueue, &request, 0);
  }
}

void sendKeyboardReportNow(uint16_t physicalMask) {
  if (keyboardInputReport == nullptr) return;
  const KeyboardNkroReport report = buildKeyboardReport(physicalMask);
  keyboardInputReport->setValue(reinterpret_cast<const uint8_t *>(&report),
                                sizeof(report));
  keyboardInputReport->notify();
}

void requestKeyboardReleaseAll() {
  // Input/app transitions only publish intent. BLE notify and advertising APIs
  // stay in the low-priority HID task, never in the 1 kHz input task.
  portENTER_CRITICAL(&stateMux);
  sharedKeyboardReleaseGate = true;
  portEXIT_CRITICAL(&stateMux);
  queueKeyboardMaskTransition(0, true);
}

void clearKeyboardReleaseGateIfAllReleased(uint16_t physicalMask) {
  if (physicalMask != 0) return;
  bool wasGated;
  portENTER_CRITICAL(&stateMux);
  wasGated = sharedKeyboardReleaseGate;
  sharedKeyboardReleaseGate = false;
  portEXIT_CRITICAL(&stateMux);
  if (wasGated) queueKeyboardMaskTransition(0);
}

void startKeyboardApp() {
  portENTER_CRITICAL(&stateMux);
  sharedKeyboardAppActive = true;
  sharedKeyboardReleaseGate = true;
  portEXIT_CRITICAL(&stateMux);
  queueKeyboardMaskTransition(0, true);
}

void stopKeyboardApp() {
  requestKeyboardReleaseAll();
  portENTER_CRITICAL(&stateMux);
  sharedKeyboardAppActive = false;
  portEXIT_CRITICAL(&stateMux);
}

void requestGlobalBlePairing() {
  portENTER_CRITICAL(&stateMux);
  sharedGlobalBleCommand = sharedKeyboardConnected
      ? GlobalBleCommand::SwitchPeer
      : GlobalBleCommand::OpenPairing;
  portEXIT_CRITICAL(&stateMux);
}

uint32_t armGlobalBlePairingWindow(uint32_t nowMs) {
  const uint32_t deadline = nowMs + GLOBAL_BLE_PAIR_WINDOW_MS;
  portENTER_CRITICAL(&stateMux);
  sharedGlobalBlePairingActive = true;
  sharedGlobalBlePairingDeadlineMs = deadline;
  sharedGlobalBlePairingGeneration = sharedGlobalBlePairingGeneration + 1U;
  sharedKeyboardBleState = KeyboardBleState::Advertising;
  portEXIT_CRITICAL(&stateMux);
  return deadline;
}

void startGlobalBlePairingWindow(uint32_t nowMs) {
  armGlobalBlePairingWindow(nowMs);
  BLEDevice::startAdvertising();
}

uint8_t globalBlePairingSecondsRemaining(uint32_t nowMs) {
  uint32_t deadline;
  bool active;
  portENTER_CRITICAL(&stateMux);
  deadline = sharedGlobalBlePairingDeadlineMs;
  active = sharedGlobalBlePairingActive;
  portEXIT_CRITICAL(&stateMux);
  if (!active || static_cast<int32_t>(deadline - nowMs) <= 0) return 0;
  const uint32_t remainingMs = deadline - nowMs;
  return static_cast<uint8_t>((remainingMs + 999U) / 1000U);
}

class KeyboardServerCallbacks : public BLEServerCallbacks {
 public:
#if defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer *server, ble_gap_conn_desc *desc) override {
    bool rejectBlockedPeer = false;
    portENTER_CRITICAL(&stateMux);
    if (sharedGlobalBlePairingActive && sharedGlobalBleBlockedPeerActive &&
        desc->peer_id_addr.type == sharedGlobalBleBlockedPeerAddressType) {
      rejectBlockedPeer = true;
      for (uint8_t i = 0; i < sizeof(sharedGlobalBleBlockedPeerAddress); ++i) {
        if (desc->peer_id_addr.val[i] != sharedGlobalBleBlockedPeerAddress[i]) {
          rejectBlockedPeer = false;
          break;
        }
      }
    }
    if (!rejectBlockedPeer) {
      sharedKeyboardTransportEpoch = sharedKeyboardTransportEpoch + 1U;
      sharedKeyboardConnected = true;
      sharedKeyboardReleaseGate = true;
      sharedKeyboardNeedsInitialZero = true;
      sharedKeyboardRestartAdvertising = false;
      sharedGlobalBlePairingActive = false;
      sharedGlobalBlePairingDeadlineMs = 0;
      sharedGlobalBleBlockedPeerActive = false;
      sharedGlobalBleCurrentPeerValid = true;
      sharedGlobalBleCurrentPeerAddressType = desc->peer_id_addr.type;
      for (uint8_t i = 0; i < sizeof(sharedGlobalBleCurrentPeerAddress); ++i) {
        sharedGlobalBleCurrentPeerAddress[i] = desc->peer_id_addr.val[i];
      }
      sharedKeyboardBleState = KeyboardBleState::Connected;
    }
    portEXIT_CRITICAL(&stateMux);
    if (rejectBlockedPeer) {
      // Keep the bond, but refuse only the peer that the user explicitly left.
      // The disconnect callback asks the HID task to resume advertising.
      server->disconnect(desc->conn_handle);
    }
  }

  void onDisconnect(BLEServer *, ble_gap_conn_desc *) override {
    handleDisconnect();
  }
#else
  void onConnect(BLEServer *) override {
    portENTER_CRITICAL(&stateMux);
    sharedKeyboardTransportEpoch = sharedKeyboardTransportEpoch + 1U;
    sharedKeyboardConnected = true;
    sharedKeyboardReleaseGate = true;
    sharedKeyboardNeedsInitialZero = true;
    sharedKeyboardRestartAdvertising = false;
    sharedGlobalBlePairingActive = false;
    sharedGlobalBlePairingDeadlineMs = 0;
    sharedGlobalBleBlockedPeerActive = false;
    sharedKeyboardBleState = KeyboardBleState::Connected;
    portEXIT_CRITICAL(&stateMux);
  }

  void onDisconnect(BLEServer *) override {
    handleDisconnect();
  }
#endif

 private:
  static void handleDisconnect() {
    portENTER_CRITICAL(&stateMux);
    sharedKeyboardTransportEpoch = sharedKeyboardTransportEpoch + 1U;
    sharedKeyboardConnected = false;
    sharedKeyboardReleaseGate = true;
    sharedKeyboardNeedsInitialZero = false;
    sharedKeyboardRestartAdvertising = sharedGlobalBlePairingActive;
    sharedKeyboardBleState = sharedGlobalBlePairingActive
        ? KeyboardBleState::Advertising : KeyboardBleState::Off;
    portEXIT_CRITICAL(&stateMux);
  }
};

bool initializeBleKeyboard() {
  portENTER_CRITICAL(&stateMux);
  sharedKeyboardBleState = KeyboardBleState::Starting;
  portEXIT_CRITICAL(&stateMux);
  if (!BLEDevice::init(KEYBOARD_DEVICE_NAME)) return false;
  BLESecurity *security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);
  security->setAuthenticationMode(true, false, true);
  globalBleServer = BLEDevice::createServer();
  if (globalBleServer == nullptr) return false;
  globalBleServer->setCallbacks(new KeyboardServerCallbacks());
  keyboardHidDevice = new BLEHIDDevice(globalBleServer);
  if (keyboardHidDevice == nullptr) return false;
  keyboardInputReport = keyboardHidDevice->inputReport(KEYBOARD_HID_REPORT_ID);
  if (keyboardInputReport == nullptr) return false;
  keyboardHidDevice->manufacturer()->setValue(KEYBOARD_MANUFACTURER_NAME);
  keyboardHidDevice->pnp(0x02, 0x303A, 0x1001, 0x0100);
  keyboardHidDevice->hidInfo(0x00, 0x01);
  keyboardHidDevice->reportMap(
      const_cast<uint8_t *>(KEYBOARD_HID_REPORT_DESCRIPTOR),
      sizeof(KEYBOARD_HID_REPORT_DESCRIPTOR));
  keyboardHidDevice->setBatteryLevel(100);
  keyboardHidDevice->startServices();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising == nullptr) return false;
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(keyboardHidDevice->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  return true;
}

void keyboardHidSendTask(void *) {
  KeyboardReportRequest request = {};
  for (;;) {
    GlobalBleCommand command;
    bool pairingActive;
    uint32_t pairingDeadline;
    portENTER_CRITICAL(&stateMux);
    command = sharedGlobalBleCommand;
    if (command != GlobalBleCommand::None) {
      sharedGlobalBleCommand = GlobalBleCommand::None;
    }
    pairingActive = sharedGlobalBlePairingActive;
    pairingDeadline = sharedGlobalBlePairingDeadlineMs;
    portEXIT_CRITICAL(&stateMux);

    if (command != GlobalBleCommand::None &&
        !sharedKeyboardBleInitAttempted) {
      portENTER_CRITICAL(&stateMux);
      sharedKeyboardBleInitAttempted = true;
      sharedKeyboardBleState = KeyboardBleState::Starting;
      portEXIT_CRITICAL(&stateMux);
      const bool initialized = initializeBleKeyboard();
      portENTER_CRITICAL(&stateMux);
      sharedKeyboardBleReady = initialized;
      sharedKeyboardBleState = initialized
          ? KeyboardBleState::Off : KeyboardBleState::Error;
      portEXIT_CRITICAL(&stateMux);
    }

    if (command != GlobalBleCommand::None && sharedKeyboardBleReady) {
      requestKeyboardReleaseAll();
      if (command == GlobalBleCommand::SwitchPeer) {
        // Snapshot the connected peer before disconnecting. During the next
        // 30 seconds only this peer is rejected; all bond keys stay intact.
        portENTER_CRITICAL(&stateMux);
        sharedGlobalBleBlockedPeerActive = sharedGlobalBleCurrentPeerValid;
        sharedGlobalBleBlockedPeerAddressType =
            sharedGlobalBleCurrentPeerAddressType;
        for (uint8_t i = 0; i < sizeof(sharedGlobalBleBlockedPeerAddress); ++i) {
          sharedGlobalBleBlockedPeerAddress[i] =
              sharedGlobalBleCurrentPeerAddress[i];
        }
        portEXIT_CRITICAL(&stateMux);
      } else {
        portENTER_CRITICAL(&stateMux);
        sharedGlobalBleBlockedPeerActive = false;
        portEXIT_CRITICAL(&stateMux);
      }
      if (sharedKeyboardConnected && globalBleServer != nullptr) {
        // Arm the switch window before terminating the link. Otherwise an old
        // computer can reconnect in the few milliseconds before advertising
        // starts and slip through the peer filter.
        pairingDeadline = armGlobalBlePairingWindow(millis());
        pairingActive = true;
        sendKeyboardReportNow(0);
        globalBleServer->disconnect(globalBleServer->getConnId());
        const uint32_t disconnectStart = millis();
        while (sharedKeyboardConnected &&
               millis() - disconnectStart < 500U) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        BLEDevice::startAdvertising();
      } else {
        const uint32_t pairingStart = millis();
        startGlobalBlePairingWindow(pairingStart);
        pairingActive = true;
        pairingDeadline = pairingStart + GLOBAL_BLE_PAIR_WINDOW_MS;
      }
    }

    if (pairingActive && !sharedKeyboardConnected &&
        static_cast<int32_t>(millis() - pairingDeadline) >= 0) {
      bool restorePreviousPeer;
      portENTER_CRITICAL(&stateMux);
      restorePreviousPeer = sharedGlobalBleBlockedPeerActive;
      sharedGlobalBleBlockedPeerActive = false;
      sharedGlobalBlePairingActive = false;
      sharedGlobalBlePairingDeadlineMs = 0;
      sharedKeyboardBleState = restorePreviousPeer
          ? KeyboardBleState::Advertising : KeyboardBleState::Off;
      portEXIT_CRITICAL(&stateMux);
      if (restorePreviousPeer) {
        // The switch timed out: keep advertising, now allowing the old bonded
        // computer to resume its ordinary automatic reconnection.
        BLEDevice::startAdvertising();
      } else {
        BLEDevice::stopAdvertising();
      }
      pairingActive = false;
    }

    bool restartAdvertising = false;
    portENTER_CRITICAL(&stateMux);
    if (sharedKeyboardRestartAdvertising && sharedGlobalBlePairingActive &&
        !sharedKeyboardConnected) {
      sharedKeyboardRestartAdvertising = false;
      restartAdvertising = true;
    }
    portEXIT_CRITICAL(&stateMux);
    if (restartAdvertising) BLEDevice::startAdvertising();

    KeyboardTransportSnapshot transport = readKeyboardTransportSnapshot();
    if (transport.connected && transport.needsInitialZero) {
      sendKeyboardReportNow(0);
      portENTER_CRITICAL(&stateMux);
      if (sharedKeyboardTransportEpoch == transport.epoch) {
        sharedKeyboardNeedsInitialZero = false;
      }
      portEXIT_CRITICAL(&stateMux);
    }
    if (xQueueReceive(keyboardReportQueue, &request,
                      pdMS_TO_TICKS(10)) == pdTRUE) {
      transport = readKeyboardTransportSnapshot();
      if (!transport.connected || !transport.appActive ||
          request.epoch != transport.epoch) continue;
      sendKeyboardReportNow(transport.releaseGate ? 0 : request.physicalMask);
    }
  }
}

void initializeSynthesisTables() {
  for (uint16_t i = 0; i < SINE_TABLE_SIZE; ++i) {
    const float radians = 6.28318530718f * static_cast<float>(i) /
                          static_cast<float>(SINE_TABLE_SIZE);
    sineTable[i] = static_cast<int16_t>(sinf(radians) * 32767.0f);
  }

  for (uint8_t i = 0; i < NOTE_COUNT; ++i) {
    basePhaseSteps[i] = static_cast<uint32_t>(
        NOTE_FREQUENCIES_C4[i] * PHASE_SCALE / SAMPLE_RATE_HZ);

    for (uint8_t octaveIndex = 0; octaveIndex < 3; ++octaveIndex) {
      noteGainByOctave[octaveIndex][i] =
          powf(10.0f, NOTE_ATTENUATION_DB[octaveIndex][i] / 20.0f);
    }
  }
}

SharedPerformanceState readPerformanceState() {
  SharedPerformanceState snapshot;
  portENTER_CRITICAL(&stateMux);
  snapshot.noteMask = sharedNoteMask;
  snapshot.octaveOffset = sharedOctaveOffset;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

void publishPerformanceState(uint8_t noteMask, int8_t octaveOffset) {
  portENTER_CRITICAL(&stateMux);
  sharedNoteMask = noteMask;
  sharedOctaveOffset = octaveOffset;
  portEXIT_CRITICAL(&stateMux);
}

AppId readActiveApp() {
  AppId app;
  portENTER_CRITICAL(&stateMux);
  app = sharedActiveApp;
  portEXIT_CRITICAL(&stateMux);
  return app;
}

void publishActiveApp(AppId app) {
  const AppId previousApp = readActiveApp();
  if (previousApp == AppId::Keyboard && app != AppId::Keyboard) {
    stopKeyboardApp();
  }
  portENTER_CRITICAL(&stateMux);
  sharedActiveApp = app;
  if (app != AppId::Home && app != AppId::Settings) {
    sharedLastResumableApp = app;
  }
  portEXIT_CRITICAL(&stateMux);
  if (app == AppId::Keyboard && previousApp != AppId::Keyboard) {
    startKeyboardApp();
  }
}

AppId readLastResumableApp() {
  AppId app;
  portENTER_CRITICAL(&stateMux);
  app = sharedLastResumableApp;
  portEXIT_CRITICAL(&stateMux);
  return app;
}

struct SongSnapshot {
  SongStatus status;
  SongTimbre timbre;
  uint8_t songIndex;
  bool restartRequested;
  uint32_t positionSamples;
  uint8_t midiPitch;
  SongLoadState loadState;
};

SongSnapshot readSongSnapshot() {
  SongSnapshot snapshot;
  portENTER_CRITICAL(&stateMux);
  snapshot.status = sharedSongStatus;
  snapshot.timbre = sharedSongTimbre;
  snapshot.songIndex = sharedSongIndex;
  snapshot.restartRequested = sharedSongRestartRequested;
  snapshot.positionSamples = sharedSongPositionSamples;
  snapshot.midiPitch = sharedSongPitch;
  snapshot.loadState = sharedSongLoadState;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

void setSongStatus(SongStatus status) {
  portENTER_CRITICAL(&stateMux);
  sharedSongStatus = status;
  if (status == SongStatus::Stopped) {
    sharedSongPositionSamples = 0;
    sharedSongPitch = 0;
  }
  portEXIT_CRITICAL(&stateMux);
}

void requestSongRestart() {
  portENTER_CRITICAL(&stateMux);
  if (sharedSongLoadState == SongLoadState::Ready) {
    sharedSongRestartRequested = true;
    sharedSongStatus = SongStatus::Playing;
  }
  portEXIT_CRITICAL(&stateMux);
}

SongTimbre cycleTimbreValue(SongTimbre timbre, int8_t direction) {
  int8_t index = static_cast<int8_t>(timbre) + direction;
  if (index < 0) {
    index = 3;
  } else if (index > 3) {
    index = 0;
  }
  return static_cast<SongTimbre>(index);
}

struct SettingsSnapshot {
  uint8_t volumePercent;
  PlayOctaveMode octaveMode;
  SongTimbre playTimbre;
  bool pitchEqEnabled;
  int8_t latchedOctave;
  SettingRow selectedRow;
  uint32_t revision;
};

SettingsSnapshot readSettingsSnapshot() {
  SettingsSnapshot snapshot;
  portENTER_CRITICAL(&stateMux);
  snapshot.volumePercent = sharedGlobalVolumePercent;
  snapshot.octaveMode = sharedPlayOctaveMode;
  snapshot.playTimbre = sharedPlayTimbre;
  snapshot.pitchEqEnabled = sharedPitchEqEnabled;
  snapshot.latchedOctave = sharedLatchedPlayOctave;
  snapshot.selectedRow = sharedSettingRow;
  snapshot.revision = sharedSettingsRevision;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

SongTimbre readPlayTimbre() {
  return readSettingsSnapshot().playTimbre;
}

uint32_t settingsBytesChecksum(const void *data, size_t length) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t settingsChecksum(const PersistentSettingsRecord &record) {
  return settingsBytesChecksum(&record,
      offsetof(PersistentSettingsRecord, checksum));
}

PersistentSettingsRecord makeSettingsRecord(const SettingsSnapshot &settings) {
  PersistentSettingsRecord record = {};
  record.magic = SETTINGS_MAGIC;
  record.version = SETTINGS_VERSION;
  record.volumePercent = settings.volumePercent;
  record.octaveMode = static_cast<uint8_t>(settings.octaveMode);
  record.playTimbre = static_cast<uint8_t>(settings.playTimbre);
  record.pitchEqEnabled = settings.pitchEqEnabled ? 1 : 0;
  record.checksum = settingsChecksum(record);
  return record;
}

bool validSettingsRecord(const PersistentSettingsRecord &record) {
  return record.magic == SETTINGS_MAGIC &&
         record.version == SETTINGS_VERSION &&
         record.volumePercent <= 100 &&
         record.volumePercent % GLOBAL_VOLUME_STEP_PERCENT == 0 &&
         record.octaveMode <= static_cast<uint8_t>(PlayOctaveMode::Latch) &&
         record.playTimbre <= static_cast<uint8_t>(SongTimbre::PianoSynth) &&
         record.pitchEqEnabled <= 1 &&
         record.checksum == settingsChecksum(record);
}

// Decode without touching NVS. A valid legacy record keeps all three settings;
// only the new switch receives its default. Subsequent saves always write v2.
bool decodeSettingsRecord(const void *data, size_t length,
                          PersistentSettingsRecord &record) {
  if (length == sizeof(record)) {
    memcpy(&record, data, sizeof(record));
    return validSettingsRecord(record);
  }
  if (length != sizeof(PersistentSettingsRecordV1)) return false;
  PersistentSettingsRecordV1 legacy;
  memcpy(&legacy, data, sizeof(legacy));
  if (legacy.magic != SETTINGS_MAGIC || legacy.version != 1 ||
      legacy.checksum != settingsBytesChecksum(&legacy,
          offsetof(PersistentSettingsRecordV1, checksum))) return false;
  record = {};
  record.magic = SETTINGS_MAGIC;
  record.version = SETTINGS_VERSION;
  record.volumePercent = legacy.volumePercent;
  record.octaveMode = legacy.octaveMode;
  record.playTimbre = legacy.playTimbre;
  record.pitchEqEnabled = DEFAULT_PITCH_EQ_ENABLED ? 1 : 0;
  record.checksum = settingsChecksum(record);
  return validSettingsRecord(record);
}

void loadPersistentSettings() {
  PersistentSettingsRecord record = {};
  Preferences preferences;
  bool valid = false;
  if (preferences.begin(SETTINGS_NVS_NAMESPACE, true)) {
    uint8_t bytes[sizeof(PersistentSettingsRecord)] = {};
    const size_t length = preferences.getBytesLength(SETTINGS_NVS_KEY);
    if ((length == sizeof(PersistentSettingsRecordV1) ||
         length == sizeof(PersistentSettingsRecord)) &&
        preferences.getBytes(SETTINGS_NVS_KEY, bytes, length) == length) {
      valid = decodeSettingsRecord(bytes, length, record);
    }
    preferences.end();
  }
  portENTER_CRITICAL(&stateMux);
  sharedGlobalVolumePercent = valid
      ? record.volumePercent : DEFAULT_GLOBAL_VOLUME_PERCENT;
  sharedPlayOctaveMode = valid
      ? static_cast<PlayOctaveMode>(record.octaveMode)
      : PlayOctaveMode::Hold;
  sharedPlayTimbre = valid
      ? static_cast<SongTimbre>(record.playTimbre) : SongTimbre::Sine;
  sharedPitchEqEnabled = valid
      ? record.pitchEqEnabled != 0 : DEFAULT_PITCH_EQ_ENABLED;
  sharedLatchedPlayOctave = 0;  // Deliberately runtime-only.
  sharedSettingsRevision = sharedSettingsRevision + 1U;
  portEXIT_CRITICAL(&stateMux);
}

void markSettingsChanged() {
  sharedSettingsRevision = sharedSettingsRevision + 1U;
  sharedSettingsSaveRevision = sharedSettingsRevision;
  sharedSettingsSavePending = true;
}

void serviceSettingsPersistence() {
  bool pending;
  uint32_t revision;
  portENTER_CRITICAL(&stateMux);
  pending = sharedSettingsSavePending;
  revision = sharedSettingsSaveRevision;
  portEXIT_CRITICAL(&stateMux);
  if (!pending) return;
  const PersistentSettingsRecord record = makeSettingsRecord(
      readSettingsSnapshot());
  Preferences preferences;
  bool written = false;
  if (preferences.begin(SETTINGS_NVS_NAMESPACE, false)) {
    written = preferences.putBytes(SETTINGS_NVS_KEY, &record, sizeof(record)) ==
        sizeof(record);
    preferences.end();
  }
  if (!written) return;
  portENTER_CRITICAL(&stateMux);
  if (sharedSettingsSaveRevision == revision) {
    sharedSettingsSavePending = false;
  }
  portEXIT_CRITICAL(&stateMux);
}

void selectSettingRow(int8_t direction) {
  portENTER_CRITICAL(&stateMux);
  int8_t row = static_cast<int8_t>(sharedSettingRow) + direction;
  if (row < 0) row = SETTING_ROW_COUNT - 1;
  if (row >= SETTING_ROW_COUNT) row = 0;
  sharedSettingRow = static_cast<SettingRow>(row);
  sharedSettingsRevision = sharedSettingsRevision + 1U;
  portEXIT_CRITICAL(&stateMux);
}

void changeSelectedSetting(int8_t direction, bool resetToDefault) {
  portENTER_CRITICAL(&stateMux);
  if (sharedSettingRow == SettingRow::Volume) {
    int16_t value = resetToDefault ? DEFAULT_GLOBAL_VOLUME_PERCENT
        : static_cast<int16_t>(sharedGlobalVolumePercent) +
              direction * GLOBAL_VOLUME_STEP_PERCENT;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    sharedGlobalVolumePercent = static_cast<uint8_t>(value);
  } else if (sharedSettingRow == SettingRow::OctaveMode) {
    if (resetToDefault) {
      sharedPlayOctaveMode = PlayOctaveMode::Hold;
    } else if (direction < 0) {
      sharedPlayOctaveMode = PlayOctaveMode::Hold;
    } else if (direction > 0) {
      sharedPlayOctaveMode = PlayOctaveMode::Latch;
    }
  } else if (sharedSettingRow == SettingRow::PlaySound) {
    int8_t timbre = resetToDefault ? 0
        : static_cast<int8_t>(sharedPlayTimbre) + direction;
    if (timbre < 0) timbre = 0;
    if (timbre > 3) timbre = 3;
    sharedPlayTimbre = static_cast<SongTimbre>(timbre);
  } else if (sharedSettingRow == SettingRow::PitchEq) {
    if (resetToDefault) sharedPitchEqEnabled = DEFAULT_PITCH_EQ_ENABLED;
    else if (direction < 0) sharedPitchEqEnabled = false;
    else if (direction > 0) sharedPitchEqEnabled = true;
  }
  markSettingsChanged();
  portEXIT_CRITICAL(&stateMux);
}

void cycleSongTimbre(int8_t direction) {
  portENTER_CRITICAL(&stateMux);
  sharedSongTimbre = cycleTimbreValue(sharedSongTimbre, direction);
  portEXIT_CRITICAL(&stateMux);
}

uint8_t readSongCatalogCount() {
  uint8_t count;
  portENTER_CRITICAL(&stateMux);
  count = songCatalogCount;
  portEXIT_CRITICAL(&stateMux);
  return count;
}

void selectSong(int8_t direction) {
  portENTER_CRITICAL(&stateMux);
  if (sharedCatalogRefreshGate || songCatalogCount == 0) {
    portEXIT_CRITICAL(&stateMux);
    return;
  }
  const uint8_t count = songCatalogCount;
  int8_t index = static_cast<int8_t>(sharedSongIndex) + direction;
  if (index < 0) {
    index = count - 1;
  } else if (index >= count) {
    index = 0;
  }
  sharedSongIndex = static_cast<uint8_t>(index);
  sharedSongRestartRequested = false;
  sharedSongStatus = SongStatus::Stopped;
  sharedSongPositionSamples = 0;
  sharedSongPitch = 0;
  const CatalogSong &selected = songCatalog[sharedSongIndex];
  if (selected.fromFile && selected.events == nullptr) {
    sharedSongLoadState = SongLoadState::Loading;
    selectedSongLoadPending = true;
    selectedSongLoadStarted = false;
  } else {
    sharedSongLoadState = SongLoadState::Ready;
  }
  portEXIT_CRITICAL(&stateMux);
}

const CatalogSong &catalogSong(uint8_t songIndex) {
  const uint8_t count = readSongCatalogCount();
  if (songIndex >= count) {
    songIndex = 0;
  }
  return songCatalog[songIndex];
}

struct MspkgContainerHeader {
  char magic[4];
  uint8_t major;
  uint8_t minor;
  uint8_t contentType;
  uint8_t flags;
  uint32_t headerBytes;
  uint32_t metadataBytes;
  uint32_t payloadBytes;
  uint32_t metadataCrc32;
  uint32_t payloadCrc32;
  uint32_t minimumFirmwareAbi;
} __attribute__((packed));
static_assert(sizeof(MspkgContainerHeader) == 32,
              "Unexpected MSPKG container header size");

struct MspkgTlvHeader {
  uint16_t tag;
  uint8_t type;
  uint8_t flags;
  uint32_t length;
} __attribute__((packed));
static_assert(sizeof(MspkgTlvHeader) == 8,
              "Unexpected MSPKG TLV header size");

struct MspkgSequenceHeader {
  char magic[4];
  uint16_t ticksPerQuarter;
  uint16_t noteRecordBytes;
  uint32_t noteEventCount;
  uint32_t tempoRecordCount;
  uint32_t durationTicks;
  uint16_t timeSignatureCount;
  uint16_t reserved16;
  uint32_t reserved32;
} __attribute__((packed));
static_assert(sizeof(MspkgSequenceHeader) == 28,
              "Unexpected MSPKG sequence header size");

struct MspkgTempoRecord {
  uint32_t tick;
  uint32_t microsecondsPerQuarter;
} __attribute__((packed));
static_assert(sizeof(MspkgTempoRecord) == 8,
              "Unexpected MSPKG tempo record size");

struct MspkgTimeSignatureRecord {
  uint32_t tick;
  uint8_t numerator;
  uint8_t denominatorPower;
  uint8_t midiClocks;
  uint8_t thirtySecondsPerQuarter;
} __attribute__((packed));
static_assert(sizeof(MspkgTimeSignatureRecord) == 8,
              "Unexpected MSPKG time-signature record size");

struct MspkgNoteRecord {
  uint32_t startTick;
  uint32_t durationTick;
  uint8_t midiPitch;
  uint8_t velocity;
  uint8_t voice;
  uint8_t flags;
} __attribute__((packed));
static_assert(sizeof(MspkgNoteRecord) == 12,
              "Unexpected MSPKG note record size");

struct MspkgMetadata {
  char title[MSPKG_TITLE_BYTES + 1];
  char preferredTimbre[MSPKG_TIMBRE_BYTES + 1];
  int32_t transpose;
  uint32_t eventCount;
  uint32_t durationTicks;
  uint32_t peakPolyphony;
  uint32_t sourceHashPrefix;
  bool hasTitle;
  bool hasSourceHash;
};

struct MspkgTempoPoint {
  uint32_t tick;
  uint32_t microsecondsPerQuarter;
};

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t length) {
  crc = ~crc;
  while (length-- > 0) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(
          -static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

bool readExact(File &file, void *buffer, size_t length) {
  return file.read(static_cast<uint8_t *>(buffer), length) == length;
}

bool skipBytes(File &file, uint32_t length) {
  const uint32_t position = static_cast<uint32_t>(file.position());
  const uint32_t fileBytes = static_cast<uint32_t>(file.size());
  if (position > fileBytes || length > fileBytes - position) {
    return false;
  }
  return file.seek(position + length);
}

bool crcFileRange(File &file,
                  uint32_t offset,
                  uint32_t length,
                  uint32_t &crc) {
  if (!file.seek(offset)) {
    return false;
  }
  uint8_t buffer[256];
  crc = 0;
  while (length > 0) {
    const size_t chunk = length < sizeof(buffer) ? length : sizeof(buffer);
    if (file.read(buffer, chunk) != chunk) {
      return false;
    }
    crc = crc32Update(crc, buffer, chunk);
    length -= static_cast<uint32_t>(chunk);
  }
  return true;
}

void copyUtf8Tlv(File &file,
                 uint32_t length,
                 char *destination,
                 size_t destinationBytes) {
  if (destinationBytes == 0) {
    skipBytes(file, length);
    return;
  }
  const size_t toCopy = length < destinationBytes - 1U
      ? length
      : destinationBytes - 1U;
  if (toCopy > 0 && file.read(
          reinterpret_cast<uint8_t *>(destination), toCopy) != toCopy) {
    destination[0] = '\0';
    return;
  }
  destination[toCopy] = '\0';
  if (length > toCopy) {
    skipBytes(file, length - static_cast<uint32_t>(toCopy));
  }
}

bool parseMspkgMetadata(File &file,
                        const MspkgContainerHeader &header,
                        MspkgMetadata &metadata) {
  memset(&metadata, 0, sizeof(metadata));
  metadata.transpose = 0;
  if (!file.seek(header.headerBytes)) {
    return false;
  }
  uint32_t remaining = header.metadataBytes;
  while (remaining > 0) {
    if (remaining < sizeof(MspkgTlvHeader)) {
      return false;
    }
    MspkgTlvHeader tlv;
    if (!readExact(file, &tlv, sizeof(tlv))) {
      return false;
    }
    remaining -= sizeof(tlv);
    if (tlv.length > remaining) {
      return false;
    }
    if (tlv.tag == 1 && tlv.type == 1) {
      copyUtf8Tlv(file, tlv.length, metadata.title, sizeof(metadata.title));
      metadata.hasTitle = metadata.title[0] != '\0';
    } else if (tlv.tag == 5 && tlv.type == 4 && tlv.length == 32) {
      uint8_t sourceHash[32];
      if (!readExact(file, sourceHash, sizeof(sourceHash))) return false;
      metadata.sourceHashPrefix =
          static_cast<uint32_t>(sourceHash[0]) |
          (static_cast<uint32_t>(sourceHash[1]) << 8U) |
          (static_cast<uint32_t>(sourceHash[2]) << 16U) |
          (static_cast<uint32_t>(sourceHash[3]) << 24U);
      metadata.hasSourceHash = true;
    } else if (tlv.tag == 17 && tlv.type == 2 && tlv.length == 4) {
      if (!readExact(file, &metadata.eventCount, 4)) return false;
    } else if (tlv.tag == 18 && tlv.type == 2 && tlv.length == 4) {
      if (!readExact(file, &metadata.durationTicks, 4)) return false;
    } else if (tlv.tag == 19 && tlv.type == 3 && tlv.length == 4) {
      if (!readExact(file, &metadata.transpose, 4)) return false;
    } else if (tlv.tag == 22 && tlv.type == 2 && tlv.length == 4) {
      if (!readExact(file, &metadata.peakPolyphony, 4)) return false;
    } else if (tlv.tag == 25 && tlv.type == 1) {
      copyUtf8Tlv(file, tlv.length,
                  metadata.preferredTimbre,
                  sizeof(metadata.preferredTimbre));
    } else if (!skipBytes(file, tlv.length)) {
      return false;
    }
    remaining -= tlv.length;
  }
  return metadata.hasTitle && metadata.hasSourceHash;
}

uint64_t tickToMicroseconds(uint32_t tick,
                            const MspkgTempoPoint *tempos,
                            uint32_t tempoCount,
                            uint16_t ticksPerQuarter) {
  uint64_t total = 0;
  uint32_t previousTick = 0;
  uint32_t microsecondsPerQuarter = 500000;
  for (uint32_t i = 0; i < tempoCount; ++i) {
    if (tempos[i].tick >= tick) {
      break;
    }
    total += (static_cast<uint64_t>(tempos[i].tick - previousTick) *
              microsecondsPerQuarter) / ticksPerQuarter;
    previousTick = tempos[i].tick;
    microsecondsPerQuarter = tempos[i].microsecondsPerQuarter;
  }
  total += (static_cast<uint64_t>(tick - previousTick) *
            microsecondsPerQuarter) / ticksPerQuarter;
  return total;
}

uint32_t tickToSample(uint32_t tick,
                      const MspkgTempoPoint *tempos,
                      uint32_t tempoCount,
                      uint16_t ticksPerQuarter) {
  const uint64_t microseconds = tickToMicroseconds(
      tick, tempos, tempoCount, ticksPerQuarter);
  return static_cast<uint32_t>(
      (microseconds * SAMPLE_RATE_HZ + 500000ULL) / 1000000ULL);
}

SongTimbre timbreFromMspkg(const char *name) {
  if (strcmp(name, "8BIT") == 0) return SongTimbre::EightBit;
  if (strcmp(name, "ORGAN") == 0) return SongTimbre::Organ;
  if (strcmp(name, "PIANO_SYNTH") == 0 || strcmp(name, "PIANO") == 0) {
    return SongTimbre::PianoSynth;
  }
  return SongTimbre::Sine;
}

bool isAsciiDisplayText(const char *text) {
  if (text == nullptr || text[0] == '\0') return false;
  for (const uint8_t *p = reinterpret_cast<const uint8_t *>(text);
       *p != 0;
       ++p) {
    if (*p < 0x20 || *p > 0x7E) return false;
  }
  return true;
}

void chooseMspkgDisplayName(const char *path,
                            const char *metadataTitle,
                            char *destination,
                            size_t destinationBytes) {
  if (isAsciiDisplayText(metadataTitle)) {
    snprintf(destination, destinationBytes, "%s", metadataTitle);
    return;
  }
  const char *name = path == nullptr ? nullptr : strrchr(path, '/');
  name = name == nullptr ? path : name + 1;
  char fallback[MSPKG_TITLE_BYTES + 1] = {};
  if (name != nullptr) {
    const size_t nameBytes = strlen(name);
    const size_t copyBytes = nameBytes < sizeof(fallback) - 1U
        ? nameBytes
        : sizeof(fallback) - 1U;
    memcpy(fallback, name, copyBytes);
    fallback[copyBytes] = '\0';
    char *extension = strrchr(fallback, '.');
    if (extension != nullptr) *extension = '\0';
  }
  if (isAsciiDisplayText(fallback)) {
    snprintf(destination, destinationBytes, "%s", fallback);
  } else {
    snprintf(destination, destinationBytes, "FILE SONG");
  }
}

bool catalogContainsSourceHash(uint32_t sourceHashPrefix) {
  const uint8_t count = readSongCatalogCount();
  for (uint8_t i = 0; i < count; ++i) {
    if (songCatalog[i].fromFile &&
        songCatalog[i].packageSourceHash == sourceHashPrefix) {
      return true;
    }
  }
  return false;
}

bool parseMspkgSong(const char *path, CatalogSong &parsed,
                    bool rejectDuplicateSourceHash, bool retainEvents) {
  parsed = {};
  File file = FFat.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    return false;
  }
  MspkgContainerHeader header;
  if (!readExact(file, &header, sizeof(header)) ||
      memcmp(header.magic, "MSPK", 4) != 0 ||
      header.major != 1 || header.contentType != 1 ||
      header.headerBytes != sizeof(MspkgContainerHeader) ||
      header.metadataBytes > MAX_MSPKG_METADATA_BYTES ||
      header.payloadBytes > MAX_MSPKG_PAYLOAD_BYTES ||
      header.minimumFirmwareAbi > 1 ||
      static_cast<uint64_t>(header.headerBytes) + header.metadataBytes +
              header.payloadBytes != file.size()) {
    file.close();
    return false;
  }
  uint32_t metadataCrc = 0;
  uint32_t payloadCrc = 0;
  if (!crcFileRange(file, header.headerBytes, header.metadataBytes,
                    metadataCrc) ||
      !crcFileRange(file, header.headerBytes + header.metadataBytes,
                    header.payloadBytes, payloadCrc) ||
      metadataCrc != header.metadataCrc32 ||
      payloadCrc != header.payloadCrc32) {
    file.close();
    return false;
  }
  MspkgMetadata metadata;
  if (!parseMspkgMetadata(file, header, metadata) ||
      metadata.eventCount == 0 ||
      metadata.eventCount > MAX_MSPKG_NOTE_EVENTS ||
      metadata.peakPolyphony == 0 ||
      metadata.peakPolyphony > MAX_MSPKG_ACTIVE_POLYPHONY ||
      metadata.transpose < -48 || metadata.transpose > 48 ||
      (rejectDuplicateSourceHash &&
       catalogContainsSourceHash(metadata.sourceHashPrefix))) {
    file.close();
    return false;
  }
  const uint32_t payloadOffset = header.headerBytes + header.metadataBytes;
  if (!file.seek(payloadOffset)) {
    file.close();
    return false;
  }
  MspkgSequenceHeader sequence;
  if (!readExact(file, &sequence, sizeof(sequence)) ||
      memcmp(sequence.magic, "MSQ1", 4) != 0 ||
      sequence.ticksPerQuarter == 0 ||
      sequence.noteRecordBytes != sizeof(MspkgNoteRecord) ||
      sequence.noteEventCount != metadata.eventCount ||
      sequence.tempoRecordCount == 0 ||
      sequence.tempoRecordCount > MAX_MSPKG_TEMPO_RECORDS ||
      sequence.durationTicks != metadata.durationTicks) {
    file.close();
    return false;
  }
  MspkgTempoPoint tempos[MAX_MSPKG_TEMPO_RECORDS];
  uint32_t previousTempoTick = 0;
  for (uint32_t i = 0; i < sequence.tempoRecordCount; ++i) {
    MspkgTempoRecord tempo;
    if (!readExact(file, &tempo, sizeof(tempo)) ||
        tempo.microsecondsPerQuarter == 0 ||
        (i > 0 && tempo.tick < previousTempoTick)) {
      file.close();
      return false;
    }
    tempos[i] = {tempo.tick, tempo.microsecondsPerQuarter};
    previousTempoTick = tempo.tick;
  }
  const uint64_t minimumPayloadBytes =
      static_cast<uint64_t>(sizeof(MspkgSequenceHeader)) +
      static_cast<uint64_t>(sequence.tempoRecordCount) *
          sizeof(MspkgTempoRecord) +
      static_cast<uint64_t>(sequence.timeSignatureCount) *
          sizeof(MspkgTimeSignatureRecord) +
      static_cast<uint64_t>(sequence.noteEventCount) *
          sizeof(MspkgNoteRecord);
  if (minimumPayloadBytes != header.payloadBytes) {
    file.close();
    return false;
  }
  const uint32_t timeSignatureBytes =
      static_cast<uint32_t>(sequence.timeSignatureCount) *
      sizeof(MspkgTimeSignatureRecord);
  if (!skipBytes(file, timeSignatureBytes)) {
    file.close();
    return false;
  }
  SongEvent *events = new (std::nothrow) SongEvent[sequence.noteEventCount];
  if (events == nullptr) {
    file.close();
    return false;
  }
  uint32_t previousStartSample = 0;
  uint32_t previousStartTick = UINT32_MAX;
  uint32_t activeEnds[MAX_MSPKG_ACTIVE_POLYPHONY] = {};
  uint8_t activeEndCount = 0;
  for (uint32_t i = 0; i < sequence.noteEventCount; ++i) {
    MspkgNoteRecord note;
    if (!readExact(file, &note, sizeof(note))) {
      delete[] events;
      file.close();
      return false;
    }
    const int32_t pitch = static_cast<int32_t>(note.midiPitch) +
                          metadata.transpose;
    if (note.startTick > sequence.durationTicks) {
      delete[] events;
      file.close();
      return false;
    }
    const uint32_t startSample = tickToSample(
        note.startTick, tempos, sequence.tempoRecordCount,
        sequence.ticksPerQuarter);
    const uint64_t endTick64 =
        static_cast<uint64_t>(note.startTick) + note.durationTick;
    if (endTick64 > sequence.durationTicks || endTick64 > UINT32_MAX) {
      delete[] events;
      file.close();
      return false;
    }
    const uint32_t endSample = tickToSample(
        static_cast<uint32_t>(endTick64),
        tempos, sequence.tempoRecordCount,
        sequence.ticksPerQuarter);
    if (note.durationTick == 0 || pitch < 0 || pitch > 127 ||
        (i > 0 && startSample < previousStartSample) ||
        endSample <= startSample) {
      delete[] events;
      file.close();
      return false;
    }
    events[i] = {startSample, endSample - startSample,
                 static_cast<uint8_t>(pitch)};
    if (i == 0 || note.startTick != previousStartTick) {
      uint8_t writeIndex = 0;
      for (uint8_t activeIndex = 0;
           activeIndex < activeEndCount;
           ++activeIndex) {
        if (activeEnds[activeIndex] > note.startTick) {
          activeEnds[writeIndex++] = activeEnds[activeIndex];
        }
      }
      activeEndCount = writeIndex;
    }
    if (activeEndCount >= MAX_MSPKG_ACTIVE_POLYPHONY) {
      delete[] events;
      file.close();
      return false;
    }
    activeEnds[activeEndCount++] = static_cast<uint32_t>(endTick64);
    previousStartSample = startSample;
    previousStartTick = note.startTick;
  }
  const uint32_t durationSamples = tickToSample(
      sequence.durationTicks, tempos, sequence.tempoRecordCount,
      sequence.ticksPerQuarter);
  uint32_t maximumEventEndSample = 0;
  for (uint32_t i = 0; i < sequence.noteEventCount; ++i) {
    const uint32_t eventEnd = events[i].startSample + events[i].durationSamples;
    if (eventEnd > maximumEventEndSample) {
      maximumEventEndSample = eventEnd;
    }
  }
  if (durationSamples == 0 || maximumEventEndSample > durationSamples) {
    delete[] events;
    file.close();
    return false;
  }
  chooseMspkgDisplayName(path, metadata.title,
                         parsed.displayName, sizeof(parsed.displayName));
  strncpy(parsed.path, path, sizeof(parsed.path) - 1U);
  parsed.events = retainEvents ? events : nullptr;
  parsed.eventCount = sequence.noteEventCount;
  parsed.durationSamples = durationSamples;
  parsed.ownedEvents = retainEvents ? events : nullptr;
  parsed.fromFile = true;
  parsed.packageSourceHash = metadata.sourceHashPrefix;
  if (!retainEvents) delete[] events;
  (void)timbreFromMspkg(metadata.preferredTimbre);
  // Preferred timbre is validated/recognized here but does not override the
  // user's current global Song timbre selection. It remains a package hint.
  file.close();
  return true;
}


bool appendMspkgSong(const char *path) {
  if (readSongCatalogCount() >= MAX_RUNTIME_SONGS) return false;
  CatalogSong parsed;
  if (!parseMspkgSong(path, parsed, true, false)) return false;
  portENTER_CRITICAL(&stateMux);
  if (songCatalogCount >= MAX_RUNTIME_SONGS) {
    portEXIT_CRITICAL(&stateMux);
    return false;
  }
  songCatalog[songCatalogCount] = parsed;
  songCatalogCount = static_cast<uint8_t>(songCatalogCount + 1U);
  portEXIT_CRITICAL(&stateMux);
  return true;
}

bool loadCatalogSongEvents(uint8_t index) {
  if (index >= readSongCatalogCount() || !songCatalog[index].fromFile) {
    return index < readSongCatalogCount();
  }
  CatalogSong loaded;
  if (!parseMspkgSong(songCatalog[index].path, loaded, false, true)) {
    return false;
  }
  if (loaded.packageSourceHash != songCatalog[index].packageSourceHash) {
    delete[] loaded.ownedEvents;
    return false;
  }
  songCatalog[index].events = loaded.events;
  songCatalog[index].ownedEvents = loaded.ownedEvents;
  songCatalog[index].eventCount = loaded.eventCount;
  songCatalog[index].durationSamples = loaded.durationSamples;
  return true;
}

bool hasMspkgExtension(const char *name) {
  if (name == nullptr) return false;
  const size_t length = strlen(name);
  // Accept normal .mspkg names and FAT 8.3 preload aliases ending in .msp.
  if (length >= 6) {
    const char *suffix = name + length - 6;
    if (suffix[0] == '.' &&
        (suffix[1] == 'm' || suffix[1] == 'M') &&
        (suffix[2] == 's' || suffix[2] == 'S') &&
        (suffix[3] == 'p' || suffix[3] == 'P') &&
        (suffix[4] == 'k' || suffix[4] == 'K') &&
        (suffix[5] == 'g' || suffix[5] == 'G')) {
      return true;
    }
  }
  if (length >= 4) {
    const char *suffix = name + length - 4;
    return suffix[0] == '.' &&
           (suffix[1] == 'm' || suffix[1] == 'M') &&
           (suffix[2] == 's' || suffix[2] == 'S') &&
           (suffix[3] == 'p' || suffix[3] == 'P');
  }
  return false;
}

bool copyMspkgEntryPath(File &entry, char *path, size_t pathBytes) {
  const char *entryPath = entry.path();
  const char *entryName = entry.name();
  const char *source = entryPath != nullptr && entryPath[0] == '/'
      ? entryPath
      : entryName;
  if (source == nullptr || pathBytes < 2) return false;
  const int written = source[0] == '/'
      ? snprintf(path, pathBytes, "%s", source)
      : snprintf(path, pathBytes, "/%s", source);
  return written > 0 && static_cast<size_t>(written) < pathBytes;
}

bool usbSafeFilename(const char *name, size_t length) {
  if (length == 0 || length > MAX_USB_FILENAME_BYTES || name[0] == '.') {
    return false;
  }
  bool previousDot = false;
  for (size_t i = 0; i < length; ++i) {
    const char c = name[i];
    const bool alphaNumeric = (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9');
    if (!alphaNumeric && c != '.' && c != '_' && c != '-') return false;
    if (c == '.' && previousDot) return false;
    previousDot = c == '.';
  }
  const char *extension = strrchr(name, '.');
  return extension != nullptr &&
         (strcasecmp(extension, ".mspkg") == 0 ||
          strcasecmp(extension, ".msp") == 0);
}

bool usbPackageFilename(const char *name) {
  return name != nullptr && name[0] != '.' && hasMspkgExtension(name);
}

void makeUsbFinalPath(const char *filename, char *path, size_t pathBytes) {
  snprintf(path, pathBytes, "/%s", filename);
}

void makeUsbBackupPath(const char *filename, char *path, size_t pathBytes) {
  snprintf(path, pathBytes, "/.~%s.bak", filename);
}

bool usbEndsWith(const char *text, const char *suffix) {
  const size_t textLength = strlen(text);
  const size_t suffixLength = strlen(suffix);
  return textLength >= suffixLength &&
         strcmp(text + textLength - suffixLength, suffix) == 0;
}

void recoverInterruptedUsbCommits() {
  File root = FFat.open("/");
  if (!root || !root.isDirectory()) return;
  File entry = root.openNextFile();
  while (entry) {
    const char *rawName = entry.name();
    char name[79] = {};
    if (rawName != nullptr) {
      const char *leaf = strrchr(rawName, '/');
      leaf = leaf == nullptr ? rawName : leaf + 1;
      strncpy(name, leaf, sizeof(name) - 1U);
    }
    entry.close();
    if (strncmp(name, ".~", 2) == 0 && usbEndsWith(name, ".bak")) {
      const size_t nameLength = strlen(name);
      const size_t originalLength = nameLength - 2U - 4U;
      if (originalLength > 0 && originalLength <= MAX_USB_FILENAME_BYTES) {
        char original[MAX_USB_FILENAME_BYTES + 1] = {};
        memcpy(original, name + 2, originalLength);
        if (usbSafeFilename(original, originalLength)) {
          char backupPath[80];
          char finalPath[64];
          snprintf(backupPath, sizeof(backupPath), "/%s", name);
          makeUsbFinalPath(original, finalPath, sizeof(finalPath));
          if (FFat.exists(finalPath)) {
            FFat.remove(backupPath);
          } else {
            FFat.rename(backupPath, finalPath);
          }
        }
      }
    }
    entry = root.openNextFile();
  }
  root.close();
  if (FFat.exists(USB_TEMP_PATH)) FFat.remove(USB_TEMP_PATH);
}

void resetRuntimeSongCatalogToEmbedded();
void beginFileSongCatalog();

bool scanMspkgCatalogFromFFat(const char *preferredPath) {
  bool preferredAccepted = preferredPath == nullptr;
  if (!ffatMounted) return false;

  // File songs are the user-managed library. If any valid file exists,
  // embedded songs remain recovery-only and are not shown. Always load files
  // in case-insensitive lexical order; the manager uses 01-..30- prefixes to
  // make the user's visible order deterministic after every refresh/reboot.
  char previousPath[96] = {};
  while (readSongCatalogCount() < MAX_FILE_SONGS) {
    File root = FFat.open("/");
    if (!root || !root.isDirectory()) break;
    char candidate[96] = {};
    File entry = root.openNextFile();
    while (entry) {
      if (!entry.isDirectory() && hasMspkgExtension(entry.name())) {
        char path[96] = {};
        if (copyMspkgEntryPath(entry, path, sizeof(path)) &&
            (previousPath[0] == '\0' || strcasecmp(path, previousPath) > 0) &&
            (candidate[0] == '\0' || strcasecmp(path, candidate) < 0)) {
          strncpy(candidate, path, sizeof(candidate) - 1U);
        }
      }
      entry.close();
      entry = root.openNextFile();
    }
    root.close();
    if (candidate[0] == '\0') break;
    strncpy(previousPath, candidate, sizeof(previousPath) - 1U);
    const bool accepted = appendMspkgSong(candidate);
    if (accepted) ++ffatAcceptedPackages;
    else ++ffatRejectedPackages;
    if (preferredPath != nullptr && strcasecmp(candidate, preferredPath) == 0) {
      preferredAccepted = accepted;
    }
  }
  return preferredAccepted;
}

void loadMspkgCatalogFromFFat() {
  ffatMounted = FFat.begin(false);
  if (!ffatMounted) return;
  recoverInterruptedUsbCommits();
  beginFileSongCatalog();
  scanMspkgCatalogFromFFat(nullptr);
  if (readSongCatalogCount() == 0) {
    resetRuntimeSongCatalogToEmbedded();
  } else {
    const bool loaded = loadCatalogSongEvents(0);
    portENTER_CRITICAL(&stateMux);
    sharedSongLoadState = loaded ? SongLoadState::Ready
                                 : SongLoadState::Failed;
    portEXIT_CRITICAL(&stateMux);
  }
}

void setUsbOverlay(UsbOverlayState state, bool holdResult = false) {
  usbOverlayState = state;
  usbOverlayExpiresAt = holdResult ? millis() + USB_RESULT_DISPLAY_MS : 0;
  ++usbOverlayRevision;
}

bool usbOverlayVisible(uint32_t now) {
  if (usbOverlayState == UsbOverlayState::Receiving ||
      usbOverlayState == UsbOverlayState::Validating ||
      usbOverlayState == UsbOverlayState::Committing ||
      usbOverlayState == UsbOverlayState::Refreshing) {
    return true;
  }
  if ((usbOverlayState == UsbOverlayState::Complete ||
       usbOverlayState == UsbOverlayState::Failed) &&
      static_cast<int32_t>(usbOverlayExpiresAt - now) > 0) {
    return true;
  }
  if (usbOverlayState != UsbOverlayState::Hidden) {
    usbOverlayState = UsbOverlayState::Hidden;
    ++usbOverlayRevision;
  }
  return false;
}

void requestRuntimeCatalogRefresh(const char *finalPath) {
  runtimeCatalogPreferredPath[0] = '\0';
  if (finalPath != nullptr) {
    strncpy(runtimeCatalogPreferredPath, finalPath,
            sizeof(runtimeCatalogPreferredPath) - 1U);
  }
  runtimeCatalogRefreshPending = true;
  runtimeCatalogRefreshStarted = false;
}

void freeLoadedFileSongEvents() {
  SongEvent *owned[MAX_FILE_SONGS] = {};
  uint8_t ownedCount = 0;
  portENTER_CRITICAL(&stateMux);
  for (uint8_t i = 0; i < songCatalogCount && i < MAX_RUNTIME_SONGS; ++i) {
    if (songCatalog[i].fromFile && songCatalog[i].ownedEvents != nullptr) {
      owned[ownedCount++] = songCatalog[i].ownedEvents;
      songCatalog[i].events = nullptr;
      songCatalog[i].ownedEvents = nullptr;
    }
  }
  portEXIT_CRITICAL(&stateMux);
  for (uint8_t i = 0; i < ownedCount; ++i) delete[] owned[i];
}

void resetRuntimeSongCatalogToEmbedded() {
  freeLoadedFileSongEvents();
  portENTER_CRITICAL(&stateMux);
  songCatalog[0] = {"VOYAGER FAREWELL", "", SONG_VOYAGERFAREWELL_EVENTS,
                    SONG_VOYAGERFAREWELL_EVENT_COUNT,
                    SONG_VOYAGERFAREWELL_DURATION_SAMPLES, nullptr, false, 0};
  songCatalog[1] = {"SPRING", "", SONG_SPRING_EVENTS,
                    SONG_SPRING_EVENT_COUNT, SONG_SPRING_DURATION_SAMPLES,
                    nullptr, false, 0};
  songCatalog[2] = {"GLORY SKY", "", SONG_GLORY_EVENTS,
                    SONG_GLORY_EVENT_COUNT, SONG_GLORY_DURATION_SAMPLES,
                    nullptr, false, 0};
  songCatalog[3] = {"REVERSAL SISTERS", "", SONG_REVERSALSISTERS_EVENTS,
                    SONG_REVERSALSISTERS_EVENT_COUNT,
                    SONG_REVERSALSISTERS_DURATION_SAMPLES, nullptr, false, 0};
  songCatalogCount = EMBEDDED_SONG_COUNT;
  sharedSongIndex = 0;
  sharedSongRestartRequested = false;
  sharedSongStatus = SongStatus::Stopped;
  sharedSongPositionSamples = 0;
  sharedSongPitch = 0;
  sharedSongLoadState = SongLoadState::Ready;
  selectedSongLoadPending = false;
  selectedSongLoadStarted = false;
  portEXIT_CRITICAL(&stateMux);
}

void beginFileSongCatalog() {
  portENTER_CRITICAL(&stateMux);
  for (uint8_t i = 0; i < MAX_RUNTIME_SONGS; ++i) songCatalog[i] = {};
  songCatalogCount = 0;
  sharedSongIndex = 0;
  sharedSongLoadState = SongLoadState::Loading;
  portEXIT_CRITICAL(&stateMux);
}

void serviceRuntimeCatalogRefresh() {
  if (!runtimeCatalogRefreshPending) return;
  if (!runtimeCatalogRefreshStarted) {
    // Stop Song and ask the audio task to finish its current 4 ms block before
    // any owned event memory is freed. This loopTask state machine never waits.
    portENTER_CRITICAL(&stateMux);
    sharedSongStatus = SongStatus::Stopped;
    sharedSongRestartRequested = false;
    sharedSongPositionSamples = 0;
    sharedSongPitch = 0;
    sharedCatalogRefreshGate = true;
    sharedAudioCatalogQuiescent = false;
    selectedSongLoadPending = false;
    selectedSongLoadStarted = false;
    portEXIT_CRITICAL(&stateMux);
    runtimeCatalogRefreshStarted = true;
    setUsbOverlay(UsbOverlayState::Refreshing);
    return;
  }

  bool audioQuiescent;
  portENTER_CRITICAL(&stateMux);
  // If audio never started or has already stopped after an I2S failure, no
  // task can retain a catalog pointer and refresh is immediately safe.
  audioQuiescent = sharedAudioCatalogQuiescent || !audioTaskHealthy;
  portEXIT_CRITICAL(&stateMux);
  if (!audioQuiescent) return;

  char previousSelectedPath[MAX_CATALOG_PATH_BYTES] = {};
  const SongSnapshot previousSelection = readSongSnapshot();
  if (previousSelection.songIndex < readSongCatalogCount() &&
      songCatalog[previousSelection.songIndex].fromFile) {
    strncpy(previousSelectedPath, songCatalog[previousSelection.songIndex].path,
            sizeof(previousSelectedPath) - 1U);
  }
  const bool requireExplicitPreferred =
      runtimeCatalogPreferredPath[0] != '\0';
  const char *preferredPath = requireExplicitPreferred
      ? runtimeCatalogPreferredPath
      : (previousSelectedPath[0] != '\0' ? previousSelectedPath : nullptr);
  ffatAcceptedPackages = 0;
  ffatRejectedPackages = 0;
  resetRuntimeSongCatalogToEmbedded();
  beginFileSongCatalog();
  const bool preferredAccepted = scanMspkgCatalogFromFFat(preferredPath);
  const bool catalogRefreshAccepted =
      !requireExplicitPreferred || preferredAccepted;
  bool selectedLoaded = true;
  if (readSongCatalogCount() == 0) {
    resetRuntimeSongCatalogToEmbedded();
  } else {
    uint8_t selectedIndex = 0;
    if (preferredPath != nullptr) {
      for (uint8_t i = 0; i < readSongCatalogCount(); ++i) {
        if (songCatalog[i].fromFile &&
            strcasecmp(songCatalog[i].path, preferredPath) == 0) {
          selectedIndex = i;
          break;
        }
      }
    }
    selectedLoaded = loadCatalogSongEvents(selectedIndex);
    portENTER_CRITICAL(&stateMux);
    sharedSongIndex = selectedIndex;
    sharedSongLoadState = selectedLoaded ? SongLoadState::Ready
                                         : SongLoadState::Failed;
    portEXIT_CRITICAL(&stateMux);
  }
  runtimeCatalogPreferredPath[0] = '\0';
  runtimeCatalogRefreshPending = false;
  runtimeCatalogRefreshStarted = false;
  portENTER_CRITICAL(&stateMux);
  sharedCatalogRefreshGate = false;
  portEXIT_CRITICAL(&stateMux);
  if (!catalogRefreshAccepted || !selectedLoaded) {
    usbLastResult = USB_ERR_PAYLOAD_FORMAT;
  }
  setUsbOverlay((catalogRefreshAccepted && selectedLoaded)
                    ? UsbOverlayState::Complete
                    : UsbOverlayState::Failed,
                true);
}

void serviceSelectedSongLoad() {
  if (runtimeCatalogRefreshPending || !selectedSongLoadPending) return;
  if (!selectedSongLoadStarted) {
    portENTER_CRITICAL(&stateMux);
    sharedSongStatus = SongStatus::Stopped;
    sharedSongRestartRequested = false;
    sharedSongPositionSamples = 0;
    sharedSongPitch = 0;
    sharedSongLoadState = SongLoadState::Loading;
    sharedCatalogRefreshGate = true;
    sharedAudioCatalogQuiescent = false;
    portEXIT_CRITICAL(&stateMux);
    selectedSongLoadStarted = true;
    return;
  }
  bool audioQuiescent;
  uint8_t selectedIndex;
  portENTER_CRITICAL(&stateMux);
  audioQuiescent = sharedAudioCatalogQuiescent || !audioTaskHealthy;
  selectedIndex = sharedSongIndex;
  portEXIT_CRITICAL(&stateMux);
  if (!audioQuiescent) return;

  freeLoadedFileSongEvents();
  const bool loaded = selectedIndex < readSongCatalogCount() &&
      (!songCatalog[selectedIndex].fromFile ||
       loadCatalogSongEvents(selectedIndex));
  portENTER_CRITICAL(&stateMux);
  sharedSongLoadState = loaded ? SongLoadState::Ready
                               : SongLoadState::Failed;
  sharedSongStatus = SongStatus::Stopped;
  sharedCatalogRefreshGate = false;
  selectedSongLoadPending = false;
  selectedSongLoadStarted = false;
  portEXIT_CRITICAL(&stateMux);
}

void writeUsbFrame(uint8_t command, uint32_t sequence,
                   const void *payload, uint32_t length) {
  UsbFrameHeader header = {};
  memcpy(header.magic, USB_FRAME_MAGIC, sizeof(header.magic));
  header.version = USB_PROTOCOL_VERSION;
  header.command = command;
  header.flags = 0;
  header.sequence = sequence;
  header.payloadLength = length;
  header.payloadCrc32 = crc32Update(
      0, static_cast<const uint8_t *>(payload), length);
  Serial.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  if (length > 0) {
    Serial.write(static_cast<const uint8_t *>(payload), length);
  }
}

void sendUsbAckLike(uint8_t responseCommand, uint8_t requestCommand,
                    uint32_t sequence, UsbErrorCode code,
                    bool cache = true) {
  UsbAckPayload payload = {
      requestCommand,
      static_cast<uint8_t>(usbDeviceState),
      static_cast<uint16_t>(code),
      usbUploadOffset,
      usbUploadLength,
      usbUploadExpectedCrc,
  };
  writeUsbFrame(responseCommand, sequence, &payload, sizeof(payload));
  if (cache) {
    usbResponseCacheValid = true;
    usbCachedSequence = sequence;
    usbCachedRequestCommand = requestCommand;
    usbCachedResponseCommand = responseCommand;
    usbCachedAck = payload;
  }
}

void sendUsbError(uint8_t requestCommand, uint32_t sequence,
                  UsbErrorCode code, bool cache = true) {
  usbLastResult = code;
  sendUsbAckLike(USB_RSP_ERROR, requestCommand, sequence, code, cache);
  setUsbOverlay(UsbOverlayState::Failed, true);
}

void sendUsbAck(uint8_t requestCommand, uint32_t sequence,
                UsbErrorCode code = USB_ERR_OK) {
  usbLastResult = code;
  sendUsbAckLike(USB_RSP_ACK, requestCommand, sequence, code, true);
}

void clearUsbUpload(bool removeTemporary) {
  if (usbUploadFile) usbUploadFile.close();
  if (removeTemporary && ffatMounted && FFat.exists(USB_TEMP_PATH)) {
    FFat.remove(USB_TEMP_PATH);
  }
  usbUploadFilename[0] = '\0';
  usbUploadLength = 0;
  usbUploadExpectedCrc = 0;
  usbUploadRunningCrc = 0;
  usbUploadOffset = 0;
  usbDeviceState = USB_STATE_IDLE;
}

UsbErrorCode validateUsbMspkg(const char *path) {
  File file = FFat.open(path, FILE_READ);
  if (!file || file.isDirectory()) return USB_ERR_FILE_IO;
  const uint32_t fileSize = static_cast<uint32_t>(file.size());
  MspkgContainerHeader header;
  if (!readExact(file, &header, sizeof(header))) {
    file.close();
    return USB_ERR_MSPKG_DECLARED_SIZE;
  }
  if (memcmp(header.magic, "MSPK", 4) != 0) {
    file.close();
    return USB_ERR_MSPKG_MAGIC;
  }
  if (header.major != 1 || header.minor != 0 || header.flags != 0 ||
      header.minimumFirmwareAbi > 1) {
    file.close();
    return USB_ERR_MSPKG_VERSION;
  }
  if (header.contentType != 1) {
    file.close();
    return USB_ERR_MSPKG_TYPE;
  }
  const uint64_t declaredSize = static_cast<uint64_t>(header.headerBytes) +
                                header.metadataBytes + header.payloadBytes;
  if (header.headerBytes != sizeof(MspkgContainerHeader) ||
      header.metadataBytes > MAX_MSPKG_METADATA_BYTES ||
      header.payloadBytes > MAX_MSPKG_PAYLOAD_BYTES ||
      declaredSize != fileSize) {
    file.close();
    return USB_ERR_MSPKG_DECLARED_SIZE;
  }
  uint32_t metadataCrc = 0;
  uint32_t payloadCrc = 0;
  if (!crcFileRange(file, header.headerBytes, header.metadataBytes,
                    metadataCrc) ||
      !crcFileRange(file, header.headerBytes + header.metadataBytes,
                    header.payloadBytes, payloadCrc)) {
    file.close();
    return USB_ERR_FILE_IO;
  }
  if (metadataCrc != header.metadataCrc32) {
    file.close();
    return USB_ERR_METADATA_CRC;
  }
  if (payloadCrc != header.payloadCrc32) {
    file.close();
    return USB_ERR_PAYLOAD_CRC;
  }

  if (!file.seek(header.headerBytes)) {
    file.close();
    return USB_ERR_FILE_IO;
  }
  uint32_t metadataRemaining = header.metadataBytes;
  while (metadataRemaining > 0) {
    if (metadataRemaining < sizeof(MspkgTlvHeader)) {
      file.close();
      return USB_ERR_MSPKG_DECLARED_SIZE;
    }
    MspkgTlvHeader tlv;
    if (!readExact(file, &tlv, sizeof(tlv))) {
      file.close();
      return USB_ERR_FILE_IO;
    }
    metadataRemaining -= sizeof(tlv);
    if (tlv.length > metadataRemaining ||
        !file.seek(file.position() + tlv.length)) {
      file.close();
      return USB_ERR_MSPKG_DECLARED_SIZE;
    }
    metadataRemaining -= tlv.length;
  }

  if (header.payloadBytes < sizeof(MspkgSequenceHeader) ||
      !file.seek(header.headerBytes + header.metadataBytes)) {
    file.close();
    return USB_ERR_PAYLOAD_FORMAT;
  }
  MspkgSequenceHeader sequence;
  if (!readExact(file, &sequence, sizeof(sequence)) ||
      memcmp(sequence.magic, "MSQ1", 4) != 0 ||
      sequence.ticksPerQuarter == 0 ||
      sequence.noteRecordBytes != sizeof(MspkgNoteRecord) ||
      sequence.noteEventCount == 0 || sequence.tempoRecordCount == 0 ||
      sequence.reserved16 != 0 || sequence.reserved32 != 0) {
    file.close();
    return USB_ERR_PAYLOAD_FORMAT;
  }
  const uint64_t expectedPayload = sizeof(MspkgSequenceHeader) +
      static_cast<uint64_t>(sequence.tempoRecordCount) *
          sizeof(MspkgTempoRecord) +
      static_cast<uint64_t>(sequence.timeSignatureCount) *
          sizeof(MspkgTimeSignatureRecord) +
      static_cast<uint64_t>(sequence.noteEventCount) *
          sizeof(MspkgNoteRecord);
  file.close();
  return expectedPayload == header.payloadBytes
      ? USB_ERR_OK
      : USB_ERR_MSPKG_DECLARED_SIZE;
}

UsbErrorCode commitUsbUpload() {
  char finalPath[64];
  char backupPath[80];
  makeUsbFinalPath(usbUploadFilename, finalPath, sizeof(finalPath));
  makeUsbBackupPath(usbUploadFilename, backupPath, sizeof(backupPath));

  if (FFat.exists(backupPath)) {
    if (FFat.exists(finalPath)) {
      if (!FFat.remove(backupPath)) return USB_ERR_COMMIT_FAILED;
    } else if (!FFat.rename(backupPath, finalPath)) {
      return USB_ERR_ROLLBACK_FAILED;
    }
  }
  const bool hadFinal = FFat.exists(finalPath);
  if (hadFinal && !FFat.rename(finalPath, backupPath)) {
    return USB_ERR_COMMIT_FAILED;
  }
  if (!FFat.rename(USB_TEMP_PATH, finalPath)) {
    if (hadFinal && !FFat.rename(backupPath, finalPath)) {
      return USB_ERR_ROLLBACK_FAILED;
    }
    return USB_ERR_COMMIT_FAILED;
  }
  if (hadFinal && FFat.exists(backupPath) && !FFat.remove(backupPath)) {
    return USB_ERR_COMMIT_FAILED;
  }
  return USB_ERR_OK;
}

void handleUsbList(uint32_t sequence) {
  if (!ffatMounted) {
    sendUsbError(USB_CMD_LIST, sequence, USB_ERR_FS_NOT_MOUNTED, false);
    return;
  }
  File root = FFat.open("/");
  if (!root || !root.isDirectory()) {
    sendUsbError(USB_CMD_LIST, sequence, USB_ERR_FILE_IO, false);
    return;
  }
  uint16_t count = 0;
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char *rawName = entry.name();
      const char *leaf = rawName == nullptr ? nullptr : strrchr(rawName, '/');
      leaf = leaf == nullptr ? rawName : leaf + 1;
      if (leaf != nullptr && usbPackageFilename(leaf)) {
        const size_t nameLength = strlen(leaf);
        if (nameLength <= MAX_USB_FILENAME_BYTES) {
          uint8_t payload[5 + MAX_USB_FILENAME_BYTES];
          const uint32_t size = static_cast<uint32_t>(entry.size());
          memcpy(payload, &size, 4);
          payload[4] = static_cast<uint8_t>(nameLength);
          memcpy(payload + 5, leaf, nameLength);
          writeUsbFrame(USB_RSP_LIST_ENTRY, sequence,
                        payload, 5 + nameLength);
          ++count;
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  writeUsbFrame(USB_RSP_LIST_DONE, sequence, &count, sizeof(count));
}

void handleUsbStatus(uint32_t sequence) {
  const UsbStatusPayload payload = {
      static_cast<uint8_t>(usbDeviceState),
      static_cast<uint8_t>(ffatMounted ? 1 : 0),
      static_cast<uint16_t>(usbLastResult),
      usbUploadOffset,
      usbUploadLength,
      usbUploadExpectedCrc,
      usbUploadRunningCrc,
  };
  writeUsbFrame(USB_RSP_STATUS, sequence, &payload, sizeof(payload));
}

void handleUsbBegin(uint32_t sequence, const uint8_t *payload,
                    uint32_t length) {
  if (!ffatMounted) {
    sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_FS_NOT_MOUNTED);
    return;
  }
  if (length < 9) {
    sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_BAD_PAYLOAD);
    return;
  }
  uint32_t fileLength;
  uint32_t fileCrc;
  memcpy(&fileLength, payload, 4);
  memcpy(&fileCrc, payload + 4, 4);
  const uint8_t nameLength = payload[8];
  if (length != 9U + nameLength || nameLength == 0 ||
      nameLength > MAX_USB_FILENAME_BYTES) {
    sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_BAD_PAYLOAD);
    return;
  }
  char filename[MAX_USB_FILENAME_BYTES + 1] = {};
  memcpy(filename, payload + 9, nameLength);
  if (!usbSafeFilename(filename, nameLength)) {
    sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_UNSAFE_FILENAME);
    return;
  }
  if (fileLength < sizeof(MspkgContainerHeader) ||
      fileLength > MAX_USB_FILE_BYTES) {
    sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_FILE_TOO_LARGE);
    return;
  }
  if (usbDeviceState == USB_STATE_RECEIVING) {
    if (strcmp(filename, usbUploadFilename) == 0 &&
        fileLength == usbUploadLength && fileCrc == usbUploadExpectedCrc) {
      usbLastUploadActivityMs = millis();
      sendUsbAck(USB_CMD_BEGIN, sequence);
    } else {
      sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_BUSY);
    }
    return;
  }
  if (usbDeviceState != USB_STATE_IDLE || runtimeCatalogRefreshPending) {
    sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_BUSY);
    return;
  }
  clearUsbUpload(true);
  usbUploadFile = FFat.open(USB_TEMP_PATH, FILE_WRITE);
  if (!usbUploadFile) {
    sendUsbError(USB_CMD_BEGIN, sequence, USB_ERR_FILE_IO);
    return;
  }
  strncpy(usbUploadFilename, filename, sizeof(usbUploadFilename) - 1U);
  usbUploadLength = fileLength;
  usbUploadExpectedCrc = fileCrc;
  usbUploadRunningCrc = 0;
  usbUploadOffset = 0;
  usbLastUploadActivityMs = millis();
  usbDeviceState = USB_STATE_RECEIVING;
  setUsbOverlay(UsbOverlayState::Receiving);
  sendUsbAck(USB_CMD_BEGIN, sequence);
}

void handleUsbChunk(uint32_t sequence, const uint8_t *payload,
                    uint32_t length) {
  if (usbDeviceState != USB_STATE_RECEIVING || !usbUploadFile) {
    sendUsbError(USB_CMD_CHUNK, sequence, USB_ERR_NO_UPLOAD);
    return;
  }
  if (length < 8) {
    sendUsbError(USB_CMD_CHUNK, sequence, USB_ERR_BAD_PAYLOAD);
    return;
  }
  uint32_t offset;
  uint16_t chunkLength;
  uint16_t reserved;
  memcpy(&offset, payload, 4);
  memcpy(&chunkLength, payload + 4, 2);
  memcpy(&reserved, payload + 6, 2);
  if (reserved != 0 || chunkLength == 0 ||
      chunkLength > MAX_USB_CHUNK_BYTES || length != 8U + chunkLength ||
      offset > usbUploadLength || chunkLength > usbUploadLength - offset) {
    sendUsbError(USB_CMD_CHUNK, sequence, USB_ERR_CHUNK_LENGTH);
    return;
  }
  const uint8_t *chunk = payload + 8;
  if (offset == usbUploadOffset) {
    if (usbUploadFile.write(chunk, chunkLength) != chunkLength) {
      sendUsbError(USB_CMD_CHUNK, sequence, USB_ERR_NO_SPACE);
      return;
    }
    usbUploadRunningCrc = crc32Update(
        usbUploadRunningCrc, chunk, chunkLength);
    usbUploadOffset += chunkLength;
  } else if (offset < usbUploadOffset &&
             chunkLength <= usbUploadOffset - offset) {
    usbUploadFile.flush();
    File compareFile = FFat.open(USB_TEMP_PATH, FILE_READ);
    uint8_t compare[128];
    uint32_t compared = 0;
    bool matches = compareFile && compareFile.seek(offset);
    while (matches && compared < chunkLength) {
      const size_t amount = (chunkLength - compared) < sizeof(compare)
          ? chunkLength - compared
          : sizeof(compare);
      if (compareFile.read(compare, amount) != amount ||
          memcmp(compare, chunk + compared, amount) != 0) {
        matches = false;
        break;
      }
      compared += static_cast<uint32_t>(amount);
    }
    compareFile.close();
    if (!matches) {
      sendUsbError(USB_CMD_CHUNK, sequence, USB_ERR_OFFSET_MISMATCH);
      return;
    }
  } else {
    sendUsbError(USB_CMD_CHUNK, sequence, USB_ERR_OFFSET_MISMATCH);
    return;
  }
  usbLastUploadActivityMs = millis();
  ++usbOverlayRevision;
  sendUsbAck(USB_CMD_CHUNK, sequence);
}

void handleUsbFinish(uint32_t sequence, uint32_t length) {
  if (length != 0) {
    sendUsbError(USB_CMD_FINISH, sequence, USB_ERR_BAD_PAYLOAD);
    return;
  }
  if (usbDeviceState != USB_STATE_RECEIVING || !usbUploadFile) {
    sendUsbError(USB_CMD_FINISH, sequence, USB_ERR_NO_UPLOAD);
    return;
  }
  if (usbUploadOffset != usbUploadLength) {
    sendUsbError(USB_CMD_FINISH, sequence, USB_ERR_FILE_LENGTH);
    return;
  }
  usbUploadFile.flush();
  usbUploadFile.close();
  if (usbUploadRunningCrc != usbUploadExpectedCrc) {
    usbDeviceState = USB_STATE_ERROR;
    sendUsbError(USB_CMD_FINISH, sequence, USB_ERR_FILE_CRC);
    clearUsbUpload(true);
    return;
  }
  usbDeviceState = USB_STATE_VALIDATING;
  setUsbOverlay(UsbOverlayState::Validating);
  UsbErrorCode result = validateUsbMspkg(USB_TEMP_PATH);
  if (result != USB_ERR_OK) {
    usbDeviceState = USB_STATE_ERROR;
    sendUsbError(USB_CMD_FINISH, sequence, result);
    clearUsbUpload(true);
    return;
  }
  usbDeviceState = USB_STATE_COMMITTING;
  setUsbOverlay(UsbOverlayState::Committing);
  result = commitUsbUpload();
  if (result != USB_ERR_OK) {
    usbDeviceState = USB_STATE_ERROR;
    sendUsbError(USB_CMD_FINISH, sequence, result);
    usbUploadFilename[0] = '\0';
    usbUploadLength = 0;
    usbUploadExpectedCrc = 0;
    usbUploadRunningCrc = 0;
    usbUploadOffset = 0;
    return;
  }
  char finalPath[MAX_USB_FILENAME_BYTES + 2] = {};
  makeUsbFinalPath(usbUploadFilename, finalPath, sizeof(finalPath));
  usbDeviceState = USB_STATE_IDLE;
  usbLastResult = USB_ERR_OK;
  sendUsbAck(USB_CMD_FINISH, sequence);
  requestRuntimeCatalogRefresh(finalPath);
  usbUploadFilename[0] = '\0';
  usbUploadLength = 0;
  usbUploadExpectedCrc = 0;
  usbUploadRunningCrc = 0;
  usbUploadOffset = 0;
}

void handleUsbAbort(uint32_t sequence, uint32_t length) {
  if (length != 0) {
    sendUsbError(USB_CMD_ABORT, sequence, USB_ERR_BAD_PAYLOAD);
    return;
  }
  clearUsbUpload(true);
  usbLastResult = USB_ERR_OK;
  usbOverlayState = UsbOverlayState::Hidden;
  ++usbOverlayRevision;
  sendUsbAck(USB_CMD_ABORT, sequence);
}

void handleUsbDelete(uint32_t sequence, const uint8_t *payload,
                     uint32_t length) {
  if (!ffatMounted) {
    sendUsbError(USB_CMD_DELETE, sequence, USB_ERR_FS_NOT_MOUNTED);
    return;
  }
  if (usbDeviceState != USB_STATE_IDLE || runtimeCatalogRefreshPending) {
    sendUsbError(USB_CMD_DELETE, sequence, USB_ERR_BUSY);
    return;
  }
  if (length < 2 || payload[0] == 0 ||
      payload[0] > MAX_USB_FILENAME_BYTES || length != 1U + payload[0]) {
    sendUsbError(USB_CMD_DELETE, sequence, USB_ERR_BAD_PAYLOAD);
    return;
  }
  char filename[MAX_USB_FILENAME_BYTES + 1] = {};
  memcpy(filename, payload + 1, payload[0]);
  if (!usbSafeFilename(filename, payload[0])) {
    sendUsbError(USB_CMD_DELETE, sequence, USB_ERR_UNSAFE_FILENAME);
    return;
  }
  char finalPath[MAX_USB_FILENAME_BYTES + 2] = {};
  char backupPath[80] = {};
  makeUsbFinalPath(filename, finalPath, sizeof(finalPath));
  makeUsbBackupPath(filename, backupPath, sizeof(backupPath));
  if (FFat.exists(backupPath) && !FFat.remove(backupPath)) {
    sendUsbError(USB_CMD_DELETE, sequence, USB_ERR_DELETE_FAILED);
    return;
  }
  if (FFat.exists(finalPath) && !FFat.remove(finalPath)) {
    sendUsbError(USB_CMD_DELETE, sequence, USB_ERR_DELETE_FAILED);
    return;
  }
  usbLastResult = USB_ERR_OK;
  sendUsbAck(USB_CMD_DELETE, sequence);
  requestRuntimeCatalogRefresh(nullptr);
}

void handleUsbInitStorage(uint32_t sequence, const uint8_t *payload,
                          uint32_t length) {
  const size_t expectedLength = strlen(USB_INIT_STORAGE_CONFIRMATION);
  if (length != expectedLength ||
      memcmp(payload, USB_INIT_STORAGE_CONFIRMATION, expectedLength) != 0) {
    sendUsbError(USB_CMD_INIT_STORAGE, sequence, USB_ERR_BAD_PAYLOAD);
    return;
  }
  if (usbDeviceState != USB_STATE_IDLE || runtimeCatalogRefreshPending) {
    sendUsbError(USB_CMD_INIT_STORAGE, sequence, USB_ERR_BUSY);
    return;
  }

  // This explicit command is the only path allowed to format FFat. It never
  // erases the app/NVS partitions and is protected by a host-side typed
  // confirmation plus this exact wire token.
  requestKeyboardReleaseAll();
  portENTER_CRITICAL(&stateMux);
  sharedSongStatus = SongStatus::Stopped;
  sharedSongRestartRequested = false;
  sharedSongPositionSamples = 0;
  sharedSongPitch = 0;
  sharedCatalogRefreshGate = true;
  sharedAudioCatalogQuiescent = false;
  portEXIT_CRITICAL(&stateMux);
  const uint32_t waitStarted = millis();
  while (audioTaskHealthy && !sharedAudioCatalogQuiescent &&
         millis() - waitStarted < 250U) {
    vTaskDelay(pdMS_TO_TICKS(4));
  }

  clearUsbUpload(false);
  if (ffatMounted) {
    FFat.end();
    ffatMounted = false;
  }
  // FFat::format() may report false when its preliminary wear-level mount
  // fails even though the subsequent format succeeds. The authoritative
  // result is whether the freshly formatted partition mounts afterward.
  FFat.format(false);
  ffatMounted = FFat.begin(false);
  if (ffatMounted) recoverInterruptedUsbCommits();
  resetRuntimeSongCatalogToEmbedded();
  runtimeCatalogPreferredPath[0] = '\0';
  runtimeCatalogRefreshPending = false;
  runtimeCatalogRefreshStarted = false;
  portENTER_CRITICAL(&stateMux);
  sharedCatalogRefreshGate = false;
  portEXIT_CRITICAL(&stateMux);
  if (!ffatMounted) {
    sendUsbError(USB_CMD_INIT_STORAGE, sequence, USB_ERR_FORMAT_FAILED);
    return;
  }
  usbLastResult = USB_ERR_OK;
  setUsbOverlay(UsbOverlayState::Complete, true);
  sendUsbAck(USB_CMD_INIT_STORAGE, sequence);
}

void handleUsbFrame(const UsbFrameHeader &header, const uint8_t *payload) {
  if (usbResponseCacheValid && header.sequence == usbCachedSequence &&
      header.command == usbCachedRequestCommand) {
    writeUsbFrame(usbCachedResponseCommand, header.sequence,
                  &usbCachedAck, sizeof(usbCachedAck));
    return;
  }
  switch (header.command) {
    case USB_CMD_LIST:
      if (header.payloadLength != 0) {
        sendUsbError(USB_CMD_LIST, header.sequence,
                     USB_ERR_BAD_PAYLOAD, false);
      } else {
        handleUsbList(header.sequence);
      }
      break;
    case USB_CMD_STATUS:
      if (header.payloadLength != 0) {
        sendUsbError(USB_CMD_STATUS, header.sequence,
                     USB_ERR_BAD_PAYLOAD, false);
      } else {
        handleUsbStatus(header.sequence);
      }
      break;
    case USB_CMD_BEGIN:
      handleUsbBegin(header.sequence, payload, header.payloadLength);
      break;
    case USB_CMD_CHUNK:
      handleUsbChunk(header.sequence, payload, header.payloadLength);
      break;
    case USB_CMD_FINISH:
      handleUsbFinish(header.sequence, header.payloadLength);
      break;
    case USB_CMD_ABORT:
      handleUsbAbort(header.sequence, header.payloadLength);
      break;
    case USB_CMD_DELETE:
      handleUsbDelete(header.sequence, payload, header.payloadLength);
      break;
    case USB_CMD_INIT_STORAGE:
      handleUsbInitStorage(header.sequence, payload, header.payloadLength);
      break;
    default:
      sendUsbError(header.command, header.sequence,
                   USB_ERR_UNKNOWN_COMMAND, false);
      break;
  }
}

void resetUsbParser() {
  usbIncomingHeader = {};
  usbIncomingHeaderBytes = 0;
  usbIncomingPayloadBytes = 0;
  usbMagicMatchBytes = 0;
}

void feedUsbProtocolByte(uint8_t value) {
  usbLastParserByteMs = millis();
  if (usbIncomingHeaderBytes < sizeof(USB_FRAME_MAGIC)) {
    if (value == static_cast<uint8_t>(
            USB_FRAME_MAGIC[usbMagicMatchBytes])) {
      reinterpret_cast<uint8_t *>(&usbIncomingHeader)[usbMagicMatchBytes] =
          value;
      ++usbMagicMatchBytes;
      usbIncomingHeaderBytes = usbMagicMatchBytes;
      return;
    }
    usbMagicMatchBytes =
        value == static_cast<uint8_t>(USB_FRAME_MAGIC[0]) ? 1 : 0;
    usbIncomingHeaderBytes = usbMagicMatchBytes;
    if (usbMagicMatchBytes == 1) {
      reinterpret_cast<uint8_t *>(&usbIncomingHeader)[0] = value;
    }
    return;
  }

  if (usbIncomingHeaderBytes < sizeof(UsbFrameHeader)) {
    reinterpret_cast<uint8_t *>(&usbIncomingHeader)
        [usbIncomingHeaderBytes++] = value;
    if (usbIncomingHeaderBytes == sizeof(UsbFrameHeader)) {
      if (usbIncomingHeader.version != USB_PROTOCOL_VERSION) {
        sendUsbError(usbIncomingHeader.command, usbIncomingHeader.sequence,
                     USB_ERR_PROTOCOL_VERSION, false);
        resetUsbParser();
      } else if (usbIncomingHeader.payloadLength > MAX_USB_FRAME_PAYLOAD) {
        sendUsbError(usbIncomingHeader.command, usbIncomingHeader.sequence,
                     USB_ERR_FRAME_TOO_LARGE, false);
        resetUsbParser();
      } else if (usbIncomingHeader.payloadLength == 0) {
        if (usbIncomingHeader.payloadCrc32 != 0) {
          sendUsbError(usbIncomingHeader.command, usbIncomingHeader.sequence,
                       USB_ERR_BAD_FRAME_CRC, false);
        } else {
          handleUsbFrame(usbIncomingHeader, usbFramePayload);
        }
        resetUsbParser();
      }
    }
    return;
  }

  usbFramePayload[usbIncomingPayloadBytes++] = value;
  if (usbIncomingPayloadBytes == usbIncomingHeader.payloadLength) {
    if (crc32Update(0, usbFramePayload, usbIncomingPayloadBytes) !=
        usbIncomingHeader.payloadCrc32) {
      sendUsbError(usbIncomingHeader.command, usbIncomingHeader.sequence,
                   USB_ERR_BAD_FRAME_CRC, false);
    } else {
      handleUsbFrame(usbIncomingHeader, usbFramePayload);
    }
    resetUsbParser();
  }
}

void serviceUsbProtocol() {
  uint16_t budget = USB_LOOP_BYTE_BUDGET;
  while (budget-- > 0 && Serial.available() > 0) {
    feedUsbProtocolByte(static_cast<uint8_t>(Serial.read()));
  }
}

void serviceUsbTimeouts(uint32_t now) {
  if (usbIncomingHeaderBytes > 0 &&
      static_cast<uint32_t>(now - usbLastParserByteMs) >
          USB_PARSER_IDLE_TIMEOUT_MS) {
    resetUsbParser();
  }
  if (usbDeviceState == USB_STATE_RECEIVING &&
      static_cast<uint32_t>(now - usbLastUploadActivityMs) >
          USB_UPLOAD_IDLE_TIMEOUT_MS) {
    clearUsbUpload(true);
    usbLastResult = USB_ERR_NO_UPLOAD;
    usbResponseCacheValid = false;
    setUsbOverlay(UsbOverlayState::Failed, true);
  }
}

const char *songTimbreName(SongTimbre timbre) {
  switch (timbre) {
    case SongTimbre::Sine:
      return "SINE";
    case SongTimbre::EightBit:
      return "8BIT";
    case SongTimbre::Organ:
      return "ORGAN";
    case SongTimbre::PianoSynth:
      return "PIANO";
  }
  return "SOUND";
}

const char *songStatusName(SongStatus status) {
  switch (status) {
    case SongStatus::Stopped:
      return "READY";
    case SongStatus::Playing:
      return "PLAY";
    case SongStatus::Paused:
      return "PAUSE";
    case SongStatus::Finished:
      return "DONE";
  }
  return "SONG";
}

AppId appForMenuNote(uint8_t noteIndex) {
  constexpr AppId APP_BY_NOTE[NOTE_COUNT] = {
      AppId::Play,
      AppId::Song,
      AppId::Loop,
      AppId::Beat,
      AppId::Pet,
      AppId::Keyboard,
      AppId::Game,
  };
  return APP_BY_NOTE[noteIndex];
}

const char *appName(AppId app) {
  switch (app) {
    case AppId::Play:
      return "PLAY";
    case AppId::Home:
      return "HOME";
    case AppId::Song:
      return "SONG";
    case AppId::Loop:
      return "LOOP";
    case AppId::Beat:
      return "BEAT";
    case AppId::Pet:
      return "PET";
    case AppId::Keyboard:
      return "GAME KEYBOARD";
    case AppId::Game:
      return "GAME";
    case AppId::Settings:
      return "SETTINGS";
  }
  return "APP";
}

float nonSineOctaveCompensation(SongTimbre timbre, uint8_t midiPitch);

uint32_t phaseStepForOctave(uint32_t baseStep, int8_t octaveOffset) {
  if (octaveOffset > 0) {
    return baseStep << 1;
  }
  if (octaveOffset < 0) {
    return baseStep >> 1;
  }
  return baseStep;
}

uint8_t octaveGainIndex(int8_t octaveOffset) {
  return static_cast<uint8_t>(octaveOffset + 1);
}

float smoothstep01(float value) {
  return value * value * (3.0f - 2.0f * value);
}

void setPitchImmediately(uint8_t note, int8_t octaveOffset,
                         SongTimbre timbre = SongTimbre::Sine,
                         bool pitchEqEnabled = DEFAULT_PITCH_EQ_ENABLED) {
  const float phaseStep =
      static_cast<float>(phaseStepForOctave(basePhaseSteps[note], octaveOffset));
  const uint8_t midiPitch = static_cast<uint8_t>(60 + note + 12 * octaveOffset);
  const float pitchGain = !pitchEqEnabled ? 1.0f : timbre == SongTimbre::Sine
      ? noteGainByOctave[octaveGainIndex(octaveOffset)][note]
      : noteGainByOctave[octaveGainIndex(octaveOffset)][note] *
            nonSineOctaveCompensation(timbre, midiPitch);
  currentPhaseSteps[note] = phaseStep;
  glideStartPhaseSteps[note] = phaseStep;
  glideTargetPhaseSteps[note] = phaseStep;
  currentPitchGains[note] = pitchGain;
  glideStartPitchGains[note] = pitchGain;
  glideTargetPitchGains[note] = pitchGain;
  const float normalizationGain = pitchEqEnabled
      ? noteGainByOctave[octaveGainIndex(octaveOffset)][note] : 1.0f;
  currentNormalizationPitchGains[note] = normalizationGain;
  glideStartNormalizationPitchGains[note] = normalizationGain;
  glideTargetNormalizationPitchGains[note] = normalizationGain;
}

void startOctaveGlide(int8_t octaveOffset, uint8_t activeNoteMask,
                      const SongTimbre *latchedTimbres,
                      const bool *latchedPitchEq) {
  const uint8_t gainIndex = octaveGainIndex(octaveOffset);

  for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
    const bool sounding = envelopes[note] > 0.0001f;
    const bool pressed =
        (activeNoteMask & static_cast<uint8_t>(1U << note)) != 0;
    if (!sounding && !pressed) {
      setPitchImmediately(note, octaveOffset, latchedTimbres[note],
                          latchedPitchEq[note]);
      continue;
    }

    glideStartPhaseSteps[note] = currentPhaseSteps[note];
    glideTargetPhaseSteps[note] = static_cast<float>(
        phaseStepForOctave(basePhaseSteps[note], octaveOffset));
    glideStartPitchGains[note] = currentPitchGains[note];
    glideStartNormalizationPitchGains[note] =
        currentNormalizationPitchGains[note];
    glideTargetNormalizationPitchGains[note] = latchedPitchEq[note]
        ? noteGainByOctave[gainIndex][note] : 1.0f;
    const uint8_t midiPitch = static_cast<uint8_t>(60 + note + 12 * octaveOffset);
    glideTargetPitchGains[note] = !latchedPitchEq[note] ? 1.0f
        : latchedTimbres[note] == SongTimbre::Sine
        ? noteGainByOctave[gainIndex][note]
        : noteGainByOctave[gainIndex][note] *
              nonSineOctaveCompensation(latchedTimbres[note], midiPitch);
  }

  glideSamplesRemaining = OCTAVE_GLIDE_SAMPLES;
}

void advanceOctaveGlide() {
  if (glideSamplesRemaining == 0) {
    return;
  }

  const uint16_t completedSamples = static_cast<uint16_t>(
      OCTAVE_GLIDE_SAMPLES - glideSamplesRemaining + 1U);
  const float linearProgress =
      static_cast<float>(completedSamples) /
      static_cast<float>(OCTAVE_GLIDE_SAMPLES);
  const float blend = smoothstep01(linearProgress);

  for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
    currentPhaseSteps[note] =
        glideStartPhaseSteps[note] +
        (glideTargetPhaseSteps[note] - glideStartPhaseSteps[note]) * blend;
    currentPitchGains[note] =
        glideStartPitchGains[note] +
        (glideTargetPitchGains[note] - glideStartPitchGains[note]) * blend;
    currentNormalizationPitchGains[note] =
        glideStartNormalizationPitchGains[note] +
        (glideTargetNormalizationPitchGains[note] -
         glideStartNormalizationPitchGains[note]) * blend;
  }

  --glideSamplesRemaining;
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

    const bool octaveDownPressed = debouncedPressed[NOTE_COUNT];
    const bool octaveUpPressed = debouncedPressed[NOTE_COUNT + 1];
    uint16_t physicalMask = noteMask;
    if (octaveDownPressed) physicalMask |= KEYBOARD_SW8_MASK;
    if (octaveUpPressed) physicalMask |= KEYBOARD_SW9_MASK;
    portENTER_CRITICAL(&stateMux);
    sharedKeyboardPhysicalMask = physicalMask;
    portEXIT_CRITICAL(&stateMux);
    const bool homeChordPressed = octaveDownPressed && octaveUpPressed;
    const uint32_t nowMs = millis();

    if (homeChordPressed) {
      if (!globalHomeChordTracking) {
        globalHomeChordTracking = true;
        globalHomeChordTriggered = false;
        globalHomeChordStartedAt = nowMs;
      } else {
        const uint32_t heldMs = nowMs - globalHomeChordStartedAt;
        if (!globalHomeChordTriggered && heldMs >= GLOBAL_HOME_HOLD_MS) {
          publishActiveApp(AppId::Home);
          suppressInputsUntilAllReleased = true;
          globalHomeChordTriggered = true;
        }
        if (!globalBlePairTriggered && heldMs >= GLOBAL_BLE_PAIR_HOLD_MS) {
          requestGlobalBlePairing();
          globalBlePairTriggered = true;
        }
      }
    } else {
      globalHomeChordTracking = false;
      globalHomeChordTriggered = false;
      globalBlePairTriggered = false;
    }

    const bool anyInputPressed =
        noteMask != 0 || octaveDownPressed || octaveUpPressed;
    if (suppressInputsUntilAllReleased && !anyInputPressed) {
      suppressInputsUntilAllReleased = false;
    }

    AppId activeApp = readActiveApp();
    if (!suppressInputsUntilAllReleased && activeApp == AppId::Home) {
      for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
        const uint8_t noteBit = static_cast<uint8_t>(1U << note);
        if ((noteMask & noteBit) != 0 &&
            (previousInputNoteMask & noteBit) == 0) {
          publishActiveApp(appForMenuNote(note));
          activeApp = readActiveApp();
          suppressInputsUntilAllReleased = true;
          break;
        }
      }
      if (!suppressInputsUntilAllReleased &&
          octaveDownPressed && !previousOctaveDownPressed) {
        publishActiveApp(AppId::Settings);
        activeApp = AppId::Settings;
        suppressInputsUntilAllReleased = true;
      } else if (!suppressInputsUntilAllReleased &&
                 octaveUpPressed && !previousOctaveUpPressed) {
        publishActiveApp(readLastResumableApp());
        activeApp = readActiveApp();
        suppressInputsUntilAllReleased = true;
      }
    }

    const bool appAcceptsInput = !suppressInputsUntilAllReleased;
    if (activeApp == AppId::Keyboard) {
      if (appAcceptsInput) clearKeyboardReleaseGateIfAllReleased(physicalMask);
      const KeyboardTransportSnapshot transport =
          readKeyboardTransportSnapshot();
      const uint16_t desiredMask =
          (!appAcceptsInput || transport.releaseGate) ? 0 : physicalMask;
      static uint16_t previousQueuedMask = 0xFFFF;
      if (transport.connected && desiredMask != previousQueuedMask) {
        queueKeyboardMaskTransition(desiredMask);
        previousQueuedMask = desiredMask;
      } else if (!transport.connected) {
        previousQueuedMask = 0xFFFF;
      }
    }
    const SettingsSnapshot settings = readSettingsSnapshot();
    if (activeApp != AppId::Play || homeChordPressed ||
        settings.octaveMode != PlayOctaveMode::Latch) {
      // Commit a latch step only after a single button is released. This makes
      // the two-button Home/BLE chord authoritative even with large press skew.
      pendingLatchedOctaveDirection = 0;
    } else if (appAcceptsInput) {
      if (octaveDownPressed && !previousOctaveDownPressed) {
        pendingLatchedOctaveDirection = -1;
      } else if (octaveUpPressed && !previousOctaveUpPressed) {
        pendingLatchedOctaveDirection = 1;
      }
      const bool pendingReleased =
          (pendingLatchedOctaveDirection < 0 && !octaveDownPressed) ||
          (pendingLatchedOctaveDirection > 0 && !octaveUpPressed);
      if (pendingReleased) {
        portENTER_CRITICAL(&stateMux);
        int8_t octave = sharedLatchedPlayOctave +
                        pendingLatchedOctaveDirection;
        if (octave < -1) octave = -1;
        if (octave > 1) octave = 1;
        sharedLatchedPlayOctave = octave;
        sharedSettingsRevision = sharedSettingsRevision + 1U;
        portEXIT_CRITICAL(&stateMux);
        pendingLatchedOctaveDirection = 0;
      }
    }
    if (appAcceptsInput && activeApp == AppId::Song) {
      const uint8_t pressedEdges =
          static_cast<uint8_t>(noteMask & static_cast<uint8_t>(~previousInputNoteMask));
      if (pressedEdges & 0x01U) {
        const SongSnapshot song = readSongSnapshot();
        if (song.loadState == SongLoadState::Ready) {
          if (song.status == SongStatus::Playing) {
            setSongStatus(SongStatus::Paused);
          } else {
            setSongStatus(SongStatus::Playing);
          }
        }
      }
      if (pressedEdges & 0x02U) {
        requestSongRestart();
      }
      if (pressedEdges & 0x04U) {
        selectSong(-1);
      }
      if (pressedEdges & 0x08U) {
        selectSong(1);
      }
      if (octaveDownPressed && !previousOctaveDownPressed) {
        cycleSongTimbre(-1);
      }
      if (octaveUpPressed && !previousOctaveUpPressed) {
        cycleSongTimbre(1);
      }
    }

    if (appAcceptsInput && activeApp == AppId::Settings) {
      const uint8_t pressedEdges = static_cast<uint8_t>(
          noteMask & static_cast<uint8_t>(~previousInputNoteMask));
      if (pressedEdges & 0x01U) selectSettingRow(-1);  // C.
      if (pressedEdges & 0x02U) selectSettingRow(1);   // D.
      if (pressedEdges & 0x04U) changeSelectedSetting(-1, false);  // E.
      if (pressedEdges & 0x08U) changeSelectedSetting(1, false);   // F.
      if (pressedEdges & 0x10U) changeSelectedSetting(0, true);    // G.
    }

    const bool playActive = appAcceptsInput && activeApp == AppId::Play;
    int8_t octaveOffset = 0;
    if (playActive) {
      const SettingsSnapshot currentSettings = readSettingsSnapshot();
      octaveOffset = currentSettings.octaveMode == PlayOctaveMode::Hold
          ? static_cast<int8_t>(octaveUpPressed) -
                static_cast<int8_t>(octaveDownPressed)
          : currentSettings.latchedOctave;
    }
    publishPerformanceState(playActive ? noteMask : 0, octaveOffset);

    previousInputNoteMask = noteMask;
    previousOctaveDownPressed = octaveDownPressed;
    previousOctaveUpPressed = octaveUpPressed;
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

float interpolatedSinePitchGain(uint8_t midiPitch) {
  constexpr uint8_t calibratedMidi[21] = {
      48, 50, 52, 53, 55, 57, 59,
      60, 62, 64, 65, 67, 69, 71,
      72, 74, 76, 77, 79, 81, 83,
  };
  if (midiPitch <= calibratedMidi[0]) {
    return noteGainByOctave[0][0];
  }
  if (midiPitch >= calibratedMidi[20]) {
    return noteGainByOctave[2][6];
  }
  for (uint8_t i = 0; i < 20; ++i) {
    const uint8_t loMidi = calibratedMidi[i];
    const uint8_t hiMidi = calibratedMidi[i + 1];
    if (midiPitch < loMidi || midiPitch > hiMidi) {
      continue;
    }
    const uint8_t loOctave = i / 7;
    const uint8_t loNote = i % 7;
    const uint8_t hiOctave = (i + 1) / 7;
    const uint8_t hiNote = (i + 1) % 7;
    const float loGain = noteGainByOctave[loOctave][loNote];
    const float hiGain = noteGainByOctave[hiOctave][hiNote];
    const float fraction = static_cast<float>(midiPitch - loMidi) /
                           static_cast<float>(hiMidi - loMidi);
    return loGain + (hiGain - loGain) * fraction;
  }
  return 1.0f;
}

float nonSineOctaveCompensation(SongTimbre timbre, uint8_t midiPitch) {
  if (timbre == SongTimbre::Sine || midiPitch < 60) return 1.0f;
  return midiPitch >= 72 ? NON_SINE_C5_GAIN : NON_SINE_C4_GAIN;
}

float normalizationPitchGain(uint8_t midiPitch, bool pitchEqEnabled) {
  return pitchEqEnabled ? interpolatedSinePitchGain(midiPitch) : 1.0f;
}

float calibratedPitchGain(SongTimbre timbre, uint8_t midiPitch,
                          bool pitchEqEnabled) {
  if (!pitchEqEnabled) return 1.0f;
  const float sineGain = interpolatedSinePitchGain(midiPitch);
  if (timbre == SongTimbre::Sine) return sineGain;
  return sineGain * nonSineOctaveCompensation(timbre, midiPitch);
}

uint32_t phaseStepForMidi(uint8_t midiPitch) {
  const float frequency = 440.0f * powf(2.0f,
      (static_cast<float>(midiPitch) - 69.0f) / 12.0f);
  return static_cast<uint32_t>(frequency * PHASE_SCALE / SAMPLE_RATE_HZ);
}

size_t findSongEventIndex(const CatalogSong &song,
                          uint32_t positionSamples) {
  size_t low = 0;
  size_t high = song.eventCount;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (song.events[middle].startSample < positionSamples) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

float sineForPhase(uint32_t phase) {
  const uint8_t tableIndex = static_cast<uint8_t>(phase >> 24);
  return static_cast<float>(sineTable[tableIndex]) / 32767.0f;
}

float renderSongOscillator(SongTimbre timbre,
                           uint32_t phase,
                           uint32_t phaseStep,
                           float &smoothedEightBit,
                           float pianoHammerEnvelope) {
  if (timbre == SongTimbre::Sine) {
    return sineForPhase(phase);
  }

  if (timbre == SongTimbre::Organ) {
    // A modest drawbar-like additive voice: strong fundamental plus octave,
    // twelfth, and double-octave components. Coefficients sum to one so the
    // global limiter remains a safety ceiling rather than normal operation.
    const float fundamental = sineForPhase(phase);
    const float octave = sineForPhase(phase * 2U);
    const float twelfth = sineForPhase(phase * 3U);
    const float doubleOctave = sineForPhase(phase * 4U);
    return ORGAN_TIMBRE_GAIN *
           (0.55f * fundamental + 0.24f * octave +
            0.14f * twelfth + 0.07f * doubleOctave);
  }

  if (timbre == SongTimbre::PianoSynth) {
    // Keep the sustained body warm and place the extra brightness in a short
    // hammer transient. Sustaining a bright harmonic stack sounds electronic.
    const float fundamental = sineForPhase(phase);
    const float second = sineForPhase(phase * 2U);
    const float third = sineForPhase(phase * 3U);
    const float fourth = sineForPhase(phase * 4U);
    const float fifth = sineForPhase(phase * 5U);
    const float warmBody =
        0.66f * fundamental + 0.22f * second + 0.12f * third;
    const float hammer =
        0.33f * second + 0.31f * third + 0.23f * fourth + 0.13f * fifth;
    return PIANO_TIMBRE_GAIN *
           (0.74f * warmBody + 0.26f * pianoHammerEnvelope * hammer);
  }

  const float target = (phase & 0x80000000UL) == 0U
      ? EIGHT_BIT_TIMBRE_GAIN
      : -EIGHT_BIT_TIMBRE_GAIN;
  // Slew limiting keeps the retro square-like character but removes the
  // one-sample vertical edges that excite the speaker and sound like clicks.
  const float halfPeriodSamples = static_cast<float>(0x80000000UL) /
                                  static_cast<float>(phaseStep);
  float edgeSamples = fminf(static_cast<float>(EIGHT_BIT_EDGE_SAMPLES),
                            fmaxf(1.0f, halfPeriodSamples * 0.45f));
  const float maxDelta = (2.0f * EIGHT_BIT_TIMBRE_GAIN) / edgeSamples;
  float delta = target - smoothedEightBit;
  if (delta > maxDelta) {
    delta = maxDelta;
  } else if (delta < -maxDelta) {
    delta = -maxDelta;
  }
  smoothedEightBit += delta;
  return smoothedEightBit;
}

void audioRenderTask(void *) {
  const float attackStep = 1.0f / (ATTACK_SECONDS * SAMPLE_RATE_HZ);
  const float releaseStep = 1.0f / (RELEASE_SECONDS * SAMPLE_RATE_HZ);
  const float songFadeStep = 1.0f / SONG_NOTE_FADE_SAMPLES;
  const float pianoAttackStep =
      1.0f / (PIANO_ATTACK_SECONDS * SAMPLE_RATE_HZ);
  const float pianoDecayStep =
      (1.0f - PIANO_SUSTAIN_LEVEL) /
      (PIANO_DECAY_SECONDS * SAMPLE_RATE_HZ);
  const float pianoHammerDecayStep =
      1.0f / (PIANO_HAMMER_DECAY_SECONDS * SAMPLE_RATE_HZ);
  uint8_t previousNoteMask = 0;
  int8_t previousOctaveOffset = 0;
  AppId previousAudioApp = AppId::Play;
  SongStatus previousSongStatus = SongStatus::Stopped;
  uint8_t activeSongIndex = 0;
  uint32_t songPositionSamples = 0;
  size_t songEventIndex = 0;
  SequenceVoice sequenceVoices[SEQUENCE_VOICE_COUNT] = {};
  uint8_t activeSongPitch = 0;
  SongTimbre latchedPlayTimbres[NOTE_COUNT] = {};
  bool latchedPlayPitchEq[NOTE_COUNT] = {};
  float playSmoothedEightBit[NOTE_COUNT] = {};
  float playPianoEnvelopes[NOTE_COUNT] = {};
  bool playPianoAttackPhases[NOTE_COUNT] = {};
  float playPianoHammerEnvelopes[NOTE_COUNT] = {};

  for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
    setPitchImmediately(note, previousOctaveOffset);
  }

  // Keep CTRL low while valid I2S clocks and digital silence settle.
  fillSilenceBlock();
  for (uint16_t block = 0; block < STARTUP_SILENCE_BLOCKS; ++block) {
    if (!writeAudioFrames()) {
      vTaskDelete(nullptr);
      return;
    }
  }

  // Continuous silence while idle avoids a power-amplifier enable transient on
  // every short key press. This v1 therefore keeps the NS4168 enabled only
  // after a controlled silent startup; no wireless/high-load mode is present.
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_ENABLE_LEVEL);
  audioTaskHealthy = true;

  for (;;) {
    const AppId audioApp = readActiveApp();
    const SharedPerformanceState state = readPerformanceState();
    const SettingsSnapshot audioSettings = readSettingsSnapshot();
    const SongTimbre playTimbre = audioSettings.playTimbre;
    const float globalVolumeGain =
        static_cast<float>(audioSettings.volumePercent) / 100.0f;
    const SongSnapshot songControl = readSongSnapshot();
    bool catalogRefreshGate;
    portENTER_CRITICAL(&stateMux);
    catalogRefreshGate = sharedCatalogRefreshGate;
    sharedAudioCatalogQuiescent = catalogRefreshGate;
    portEXIT_CRITICAL(&stateMux);
    const uint8_t pressedEdges =
        static_cast<uint8_t>(state.noteMask & static_cast<uint8_t>(~previousNoteMask));
    const bool octaveChanged = state.octaveOffset != previousOctaveOffset;

    // While refresh is gated, reference only immutable embedded storage. The
    // loopTask waits for sharedAudioCatalogQuiescent before freeing file events.
    const CatalogSong &selectedSong = catalogRefreshGate
        ? AUDIO_SAFE_EMBEDDED_SONG
        : catalogSong(songControl.songIndex);
    const bool songSelectionChanged = songControl.songIndex != activeSongIndex;

    if (audioApp != previousAudioApp) {
      previousNoteMask = 0;
      for (auto &voice : sequenceVoices) {
        voice = {};
      }
      activeSongPitch = 0;
      if (audioApp != AppId::Song) {
        songPositionSamples = 0;
        songEventIndex = 0;
        setSongStatus(SongStatus::Stopped);
      }
      previousAudioApp = audioApp;
    }

    if (audioApp == AppId::Song &&
        (songControl.restartRequested || songSelectionChanged)) {
      activeSongIndex = songControl.songIndex;
      songPositionSamples = 0;
      songEventIndex = 0;
      for (auto &voice : sequenceVoices) {
        voice = {};
      }
      activeSongPitch = 0;
      portENTER_CRITICAL(&stateMux);
      sharedSongRestartRequested = false;
      sharedSongPositionSamples = 0;
      sharedSongPitch = 0;
      portEXIT_CRITICAL(&stateMux);
    }

    if (audioApp == AppId::Song &&
        previousSongStatus != SongStatus::Playing &&
        songControl.status == SongStatus::Playing) {
      if (previousSongStatus == SongStatus::Stopped ||
          previousSongStatus == SongStatus::Finished) {
        songPositionSamples = 0;
        songEventIndex = 0;
      } else {
        songEventIndex = findSongEventIndex(selectedSong, songPositionSamples);
      }
    }
    if (audioApp == AppId::Song && songControl.status == SongStatus::Stopped) {
      songPositionSamples = 0;
      songEventIndex = 0;
      for (auto &voice : sequenceVoices) {
        voice = {};
      }
      activeSongPitch = 0;
    }
    previousSongStatus = songControl.status;

    if (audioApp == AppId::Play && octaveChanged) {
      startOctaveGlide(state.octaveOffset, state.noteMask, latchedPlayTimbres,
                       latchedPlayPitchEq);
      previousOctaveOffset = state.octaveOffset;
    }

    for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
      if (audioApp == AppId::Play &&
          (pressedEdges & static_cast<uint8_t>(1U << note))) {
        // New notes begin directly at the selected octave. Only already-sounding
        // notes glide, so a normal key attack remains crisp and repeatable.
        latchedPlayTimbres[note] = playTimbre;
        // Latch alongside timbre: a settings change must not retune tail gains.
        latchedPlayPitchEq[note] = audioSettings.pitchEqEnabled;
        setPitchImmediately(note, state.octaveOffset, playTimbre,
                             latchedPlayPitchEq[note]);
        phases[note] = 0;
        envelopes[note] = 0.0f;
        playSmoothedEightBit[note] = 0.0f;
        playPianoEnvelopes[note] = 0.0f;
        playPianoAttackPhases[note] = true;
        playPianoHammerEnvelopes[note] = 1.0f;
      }
    }
    previousNoteMask = state.noteMask;

    for (size_t frame = 0; frame < AUDIO_FRAMES_PER_BLOCK; ++frame) {
      advanceOctaveGlide();
      float mixed = 0.0f;
      float envelopeWeight = 0.0f;

      for (uint8_t note = 0; note < NOTE_COUNT; ++note) {
        const bool pressed = audioApp == AppId::Play &&
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
          const SongTimbre noteTimbre = latchedPlayTimbres[note];
          if (noteTimbre == SongTimbre::PianoSynth &&
              playPianoHammerEnvelopes[note] > 0.0f) {
            playPianoHammerEnvelopes[note] -= pianoHammerDecayStep;
            if (playPianoHammerEnvelopes[note] < 0.0f) {
              playPianoHammerEnvelopes[note] = 0.0f;
            }
          }
          const uint32_t phaseStep =
              static_cast<uint32_t>(currentPhaseSteps[note]);
          const float normalizedSample = renderSongOscillator(
              noteTimbre,
              phases[note],
              phaseStep,
              playSmoothedEightBit[note],
              playPianoHammerEnvelopes[note]);
          float playTimbreEnvelope = 1.0f;
          if (noteTimbre == SongTimbre::PianoSynth) {
            if (playPianoAttackPhases[note]) {
              playPianoEnvelopes[note] += pianoAttackStep;
              if (playPianoEnvelopes[note] >= 1.0f) {
                playPianoEnvelopes[note] = 1.0f;
                playPianoAttackPhases[note] = false;
              }
            } else if (playPianoEnvelopes[note] > PIANO_SUSTAIN_LEVEL) {
              playPianoEnvelopes[note] -= pianoDecayStep;
              if (playPianoEnvelopes[note] < PIANO_SUSTAIN_LEVEL) {
                playPianoEnvelopes[note] = PIANO_SUSTAIN_LEVEL;
              }
            }
            playTimbreEnvelope = playPianoEnvelopes[note];
          }
          const float calibratedEnvelope =
              envelopes[note] * playTimbreEnvelope * currentPitchGains[note];
          mixed += normalizedSample * calibratedEnvelope;
          const float normalizationEnvelope =
              envelopes[note] * playTimbreEnvelope *
              currentNormalizationPitchGains[note];
          envelopeWeight += normalizationEnvelope;
          phases[note] += phaseStep;
        }
      }

      if (audioApp == AppId::Song &&
          songControl.status == SongStatus::Playing &&
          songControl.loadState == SongLoadState::Ready &&
          selectedSong.events != nullptr) {
        while (songEventIndex < selectedSong.eventCount &&
               selectedSong.events[songEventIndex].startSample <=
                   songPositionSamples) {
          const SongEvent &event = selectedSong.events[songEventIndex];
          int8_t slot = -1;
          for (uint8_t voiceIndex = 0;
               voiceIndex < SEQUENCE_VOICE_COUNT;
               ++voiceIndex) {
            if (!sequenceVoices[voiceIndex].active ||
                sequenceVoices[voiceIndex].envelope <= 0.0001f) {
              slot = static_cast<int8_t>(voiceIndex);
              break;
            }
          }
          if (slot < 0) {
            // Defensive fallback for malformed input: reuse the quietest tail.
            slot = 0;
            for (uint8_t voiceIndex = 1;
                 voiceIndex < SEQUENCE_VOICE_COUNT;
                 ++voiceIndex) {
              if (sequenceVoices[voiceIndex].envelope <
                  sequenceVoices[slot].envelope) {
                slot = static_cast<int8_t>(voiceIndex);
              }
            }
          }
          SequenceVoice &voice = sequenceVoices[slot];
          voice.phase = 0;
          voice.endSample = event.startSample + event.durationSamples;
          voice.phaseStep = phaseStepForMidi(event.midiPitch);
          voice.pitchGain = calibratedPitchGain(songControl.timbre,
                                                event.midiPitch,
                                                audioSettings.pitchEqEnabled);
          voice.normalizationPitchGain =
              normalizationPitchGain(event.midiPitch, audioSettings.pitchEqEnabled);
          voice.envelope = 0.0f;
          voice.smoothedEightBit = 0.0f;
          voice.pianoEnvelope = 0.0f;
          voice.pianoHammerEnvelope = 1.0f;
          voice.midiPitch = event.midiPitch;
          voice.timbre = songControl.timbre;
          voice.active = true;
          voice.pianoAttackPhase = true;
          activeSongPitch = event.midiPitch;
          ++songEventIndex;
        }

        for (auto &voice : sequenceVoices) {
          if (voice.active && songPositionSamples >= voice.endSample) {
            voice.active = false;
          }
          if (voice.active) {
            voice.envelope += songFadeStep;
            if (voice.envelope > 1.0f) {
              voice.envelope = 1.0f;
            }
          } else {
            voice.envelope -= songFadeStep;
            if (voice.envelope < 0.0f) {
              voice.envelope = 0.0f;
            }
          }
          if (voice.envelope <= 0.0001f) {
            continue;
          }

          if (voice.timbre == SongTimbre::PianoSynth &&
              voice.pianoHammerEnvelope > 0.0f) {
            voice.pianoHammerEnvelope -= pianoHammerDecayStep;
            if (voice.pianoHammerEnvelope < 0.0f) {
              voice.pianoHammerEnvelope = 0.0f;
            }
          }
          const float songSample = renderSongOscillator(
              voice.timbre,
              voice.phase,
              voice.phaseStep,
              voice.smoothedEightBit,
              voice.pianoHammerEnvelope);
          float timbreEnvelope = 1.0f;
          if (voice.timbre == SongTimbre::PianoSynth) {
            if (voice.pianoAttackPhase) {
              voice.pianoEnvelope += pianoAttackStep;
              if (voice.pianoEnvelope >= 1.0f) {
                voice.pianoEnvelope = 1.0f;
                voice.pianoAttackPhase = false;
              }
            } else if (voice.pianoEnvelope > PIANO_SUSTAIN_LEVEL) {
              voice.pianoEnvelope -= pianoDecayStep;
              if (voice.pianoEnvelope < PIANO_SUSTAIN_LEVEL) {
                voice.pianoEnvelope = PIANO_SUSTAIN_LEVEL;
              }
            }
            timbreEnvelope = voice.pianoEnvelope;
          }
          const float activeGain =
              voice.envelope * timbreEnvelope * voice.pitchGain;
          mixed += songSample * activeGain;
          envelopeWeight += voice.envelope * timbreEnvelope *
                            voice.normalizationPitchGain;
          voice.phase += voice.phaseStep;
        }

        ++songPositionSamples;
        if (songPositionSamples >= selectedSong.durationSamples) {
          setSongStatus(SongStatus::Finished);
          for (auto &voice : sequenceVoices) {
            voice.active = false;
          }
        }
        portENTER_CRITICAL(&stateMux);
        sharedSongPositionSamples = songPositionSamples;
        sharedSongPitch = activeSongPitch;
        portEXIT_CRITICAL(&stateMux);
      } else if (audioApp == AppId::Song) {
        portENTER_CRITICAL(&stateMux);
        sharedSongPositionSamples = songPositionSamples;
        sharedSongPitch = activeSongPitch;
        portEXIT_CRITICAL(&stateMux);
      }

      // Chords are normalized so adding notes does not multiply peak output.
      if (envelopeWeight > 1.0f) {
        mixed /= envelopeWeight;
      }

      // Global volume follows normalization and precedes the final safety clamp.
      // At 100%, multiplication by 1.0f preserves the accepted output path.
      mixed *= globalVolumeGain;

      if (mixed > 1.0f) {
        mixed = 1.0f;
      } else if (mixed < -1.0f) {
        mixed = -1.0f;
      }

      const int16_t sample = static_cast<int16_t>(mixed * MASTER_PEAK);
      audioFrames[frame].left = 0;
      audioFrames[frame].right = sample;  // CTRL HIGH selects the right slot.
    }

    if (!writeAudioFrames()) {
      audioTaskHealthy = false;
      vTaskDelete(nullptr);
      return;
    }
  }
}

void drawGlobalBleBadge(uint32_t nowMs) {
  bool connected;
  bool pairing;
  portENTER_CRITICAL(&stateMux);
  connected = sharedKeyboardConnected;
  pairing = sharedGlobalBlePairingActive;
  portEXIT_CRITICAL(&stateMux);
  char badge[5] = "B-";
  if (connected) {
    strncpy(badge, "B+", sizeof(badge));
  } else if (pairing) {
    const uint8_t seconds = globalBlePairingSecondsRemaining(nowMs);
    snprintf(badge, sizeof(badge), "B%02u", seconds);
  }
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(126 - display.getStrWidth(badge), 8, badge);
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

void updatePlayDisplay(const SharedPerformanceState &state) {
  if (!displayReady) {
    return;
  }

  display.clearBuffer();

  // Rows 0-15 are physically yellow: status and octave only.
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, audioTaskHealthy ? "PLAY" : "WAIT");
  const char *playTimbreName = songTimbreName(readPlayTimbre());
  const char *octaveName = "C4";
  if (state.octaveOffset > 0) {
    octaveName = "C5";
  } else if (state.octaveOffset < 0) {
    octaveName = "C3";
  }
  char timbreAndOctave[16];
  snprintf(timbreAndOctave, sizeof(timbreAndOctave),
           "%s %s", playTimbreName, octaveName);
  const int16_t timbreWidth = display.getStrWidth(timbreAndOctave);
  display.drawStr(101 - timbreWidth, 11, timbreAndOctave);
  drawGlobalBleBadge(millis());
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  // Rows 16-63 are physically blue: current chord and note-key states.
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

void drawHomeDisplay() {
  if (!displayReady) {
    return;
  }

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, "HOME");
  display.drawStr(36, 11, "SW8 SET");
  drawGlobalBleBadge(millis());
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  display.setFont(u8g2_font_6x10_tf);
  drawCentered("PRESS ONE APP KEY", 35);

  const char *const labels[NOTE_COUNT] = {"PL", "SG", "LP", "BT", "PT", "KB", "GM"};
  display.setFont(u8g2_font_5x7_tf);
  constexpr uint8_t cellWidth = 16;
  constexpr uint8_t cellGap = 2;
  constexpr uint8_t cellsX = 2;
  constexpr uint8_t cellsY = 46;
  constexpr uint8_t cellsHeight = 16;

  for (uint8_t i = 0; i < NOTE_COUNT; ++i) {
    const uint8_t x = static_cast<uint8_t>(cellsX + i * (cellWidth + cellGap));
    display.drawFrame(x, cellsY, cellWidth, cellsHeight);
    const int16_t labelWidth = display.getStrWidth(labels[i]);
    display.drawStr(x + (cellWidth - labelWidth) / 2, 58, labels[i]);
  }

  display.sendBuffer();
}

void drawSongDisplay(const SongSnapshot &song) {
  if (!displayReady) {
    return;
  }

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  const char *status = song.loadState == SongLoadState::Loading
      ? "LOAD"
      : (song.loadState == SongLoadState::Failed
          ? "FAILED" : songStatusName(song.status));
  display.drawStr(2, 11, status);
  const char *timbre = songTimbreName(song.timbre);
  const int16_t timbreWidth = display.getStrWidth(timbre);
  display.drawStr(101 - timbreWidth, 11, timbre);
  drawGlobalBleBadge(millis());
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  display.setFont(u8g2_font_6x10_tf);
  const CatalogSong &selectedSong = catalogSong(song.songIndex);
  drawCentered(selectedSong.displayName, 29);

  const uint32_t currentSecond = song.positionSamples / SAMPLE_RATE_HZ;
  const uint32_t totalSecond = selectedSong.durationSamples / SAMPLE_RATE_HZ;
  char progress[28];
  snprintf(progress, sizeof(progress), "%02u/%02u  %lu:%02lu/%lu:%02lu",
           static_cast<unsigned>(song.songIndex + 1U),
           static_cast<unsigned>(readSongCatalogCount()),
           static_cast<unsigned long>(currentSecond / 60U),
           static_cast<unsigned long>(currentSecond % 60U),
           static_cast<unsigned long>(totalSecond / 60U),
           static_cast<unsigned long>(totalSecond % 60U));
  display.setFont(u8g2_font_5x7_tf);
  drawCentered(progress, 43);

  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(2, 56, "C PLAY  D RESTART");
  display.drawStr(2, 63, "E/F SELECT  SW8/9 SOUND");
  display.sendBuffer();
}

void drawKeyboardKeyCell(int16_t x, int16_t y, int16_t width, int16_t height,
                         const char *label, bool pressed) {
  if (pressed) {
    display.drawBox(x, y, width, height);
    display.setDrawColor(0);
  } else {
    display.drawFrame(x, y, width, height);
    display.setDrawColor(1);
  }
  display.drawStr(x + (width - display.getStrWidth(label)) / 2,
                  y + height - 4, label);
  display.setDrawColor(1);
}

void drawKeyboardDisplay() {
  if (!displayReady) return;
  uint16_t physicalMask;
  KeyboardBleState bleState;
  bool releaseGate;
  portENTER_CRITICAL(&stateMux);
  physicalMask = sharedKeyboardPhysicalMask;
  bleState = sharedKeyboardBleState;
  releaseGate = sharedKeyboardReleaseGate;
  portEXIT_CRITICAL(&stateMux);
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, "KEYBOARD");
  (void)bleState;
  drawGlobalBleBadge(millis());
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);
  display.setFont(u8g2_font_5x7_tf);
  constexpr int16_t keyWidth = 25;
  constexpr int16_t keyHeight = 15;
  constexpr int16_t gap = 4;
  const char *const topLabels[4] = {"Q", "W", "E", "R"};
  const uint8_t topBits[4] = {0, 1, 2, 7};
  const char *const bottomLabels[4] = {"A", "S", "D", "F"};
  const uint8_t bottomBits[4] = {3, 4, 5, 6};
  for (uint8_t i = 0; i < 4; ++i) {
    const int16_t x = 3 + i * (keyWidth + gap);
    drawKeyboardKeyCell(x, 18, keyWidth, keyHeight, topLabels[i],
                        (physicalMask & (1U << topBits[i])) != 0);
    drawKeyboardKeyCell(x, 35, keyWidth, keyHeight, bottomLabels[i],
                        (physicalMask & (1U << bottomBits[i])) != 0);
  }
  drawKeyboardKeyCell(3, 52, 36, 11, "ALT",
                      (physicalMask & KEYBOARD_SW9_MASK) != 0);
  display.drawStr(44, 61, releaseGate ? "HID RELEASE" : "HID ACTIVE");
  display.sendBuffer();
}

const char *octaveModeName(PlayOctaveMode mode) {
  return mode == PlayOctaveMode::Latch ? "LATCH" : "HOLD";
}

uint8_t settingsFirstVisibleRow(SettingRow selectedRow) {
  const uint8_t selected = static_cast<uint8_t>(selectedRow);
  return selected < SETTINGS_VISIBLE_ROWS ? 0
      : selected - SETTINGS_VISIBLE_ROWS + 1;
}

void drawSettingsDisplay() {
  if (!displayReady) return;
  const SettingsSnapshot settings = readSettingsSnapshot();
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, "SETTINGS");
  drawGlobalBleBadge(millis());
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  display.setFont(u8g2_font_5x7_tf);
  char value[24];
  const uint8_t firstRow = settingsFirstVisibleRow(settings.selectedRow);
  for (uint8_t visible = 0; visible < SETTINGS_VISIBLE_ROWS; ++visible) {
    const SettingRow row = static_cast<SettingRow>(firstRow + visible);
    const char marker = settings.selectedRow == row ? '>' : ' ';
    switch (row) {
      case SettingRow::Volume:
        snprintf(value, sizeof(value), "%c VOLUME       %3u%%", marker,
                 static_cast<unsigned>(settings.volumePercent));
        break;
      case SettingRow::OctaveMode:
        snprintf(value, sizeof(value), "%c OCT MODE     %s", marker,
                 octaveModeName(settings.octaveMode));
        break;
      case SettingRow::PlaySound:
        snprintf(value, sizeof(value), "%c PLAY SOUND   %s", marker,
                 songTimbreName(settings.playTimbre));
        break;
      case SettingRow::PitchEq:
        snprintf(value, sizeof(value), "%c PITCH EQ:    %s", marker,
                 settings.pitchEqEnabled ? "ON" : "OFF");
        break;
    }
    display.drawStr(1, 24 + visible * 10, value);
  }
  display.drawStr(1, 55, "C/D ROW  E/F CHANGE");
  display.drawStr(1, 63, "G RESET  HOLD BOTH HOME");
  display.sendBuffer();
}

void drawPlaceholderDisplay(AppId app) {
  if (!displayReady) {
    return;
  }

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, appName(app));
  drawGlobalBleBadge(millis());
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  display.setFont(u8g2_font_ncenB12_tr);
  drawCentered("COMING SOON", 40);
  display.setFont(u8g2_font_5x7_tf);
  drawCentered("HOLD SW8+SW9", 57);
  display.sendBuffer();
}

void drawUsbTransferDisplay() {
  if (!displayReady) return;
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(2, 11, "USB IMPORT");
  const char *stateText = "READY";
  if (usbOverlayState == UsbOverlayState::Receiving) stateText = "RECEIVE";
  else if (usbOverlayState == UsbOverlayState::Validating) stateText = "VERIFY";
  else if (usbOverlayState == UsbOverlayState::Committing) stateText = "SAVE";
  else if (usbOverlayState == UsbOverlayState::Refreshing) stateText = "CATALOG";
  else if (usbOverlayState == UsbOverlayState::Complete) stateText = "DONE";
  else if (usbOverlayState == UsbOverlayState::Failed) stateText = "FAILED";
  display.drawStr(126 - display.getStrWidth(stateText), 11, stateText);
  display.drawLine(0, OLED_YELLOW_ROWS - 1, 127, OLED_YELLOW_ROWS - 1);

  display.setFont(u8g2_font_ncenB12_tr);
  if (usbOverlayState == UsbOverlayState::Failed) {
    char errorText[16];
    snprintf(errorText, sizeof(errorText), "ERROR %u",
             static_cast<unsigned>(usbLastResult));
    drawCentered(errorText, 39);
  } else if (usbOverlayState == UsbOverlayState::Complete) {
    drawCentered("SONGS UPDATED", 39);
  } else {
    const uint32_t percent = usbUploadLength == 0
        ? 0
        : static_cast<uint32_t>(
              (static_cast<uint64_t>(usbUploadOffset) * 100U) /
              usbUploadLength);
    char progress[16];
    snprintf(progress, sizeof(progress), "%lu%%",
             static_cast<unsigned long>(percent));
    drawCentered(progress, 39);
  }
  display.setFont(u8g2_font_5x7_tf);
  if (usbOverlayState == UsbOverlayState::Receiving &&
      usbUploadFilename[0] != '\0') {
    drawCentered(usbUploadFilename, 58);
  } else {
    drawCentered("MUSB V1", 58);
  }
  display.sendBuffer();
}

void updateDisplay(AppId app, const SharedPerformanceState &state) {
  if (usbOverlayVisible(millis())) {
    drawUsbTransferDisplay();
  } else if (app == AppId::Play) {
    updatePlayDisplay(state);
  } else if (app == AppId::Home) {
    drawHomeDisplay();
  } else if (app == AppId::Song) {
    drawSongDisplay(readSongSnapshot());
  } else if (app == AppId::Keyboard) {
    drawKeyboardDisplay();
  } else if (app == AppId::Settings) {
    drawSettingsDisplay();
  } else {
    drawPlaceholderDisplay(app);
  }
}

}  // namespace

void setup() {
  // Safety first: CTRL stays low until I2S clocks and silence are established.
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  // Hardware CDC defaults to only 256 RX bytes. Real-board validation showed
  // that a full 1032-byte CHUNK frame is stable only when this is enlarged
  // before begin(). Never wait for Serial; no host is required for normal use.
  Serial.setRxBufferSize(USB_RX_BUFFER_BYTES);
  Serial.begin(115200);

  for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
    pinMode(ALL_INPUT_PINS[i], MINI_SYNTH_KEY_PIN_MODE);
  }
  loadPersistentSettings();
  initializeSynthesisTables();

  // Mount without format-on-fail: a corrupt or absent FAT partition must never
  // erase songs automatically. Valid MSPKG files form a metadata-only catalog; only index 0 is loaded
  // before tasks start, and later file selections load through the audio gate.
  loadMspkgCatalogFromFFat();

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

  if (!i2sReady) {
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
    updateDisplay(readActiveApp(), readPerformanceState());
    return;
  }

  const BaseType_t inputTaskResult = xTaskCreate(
      inputScanTask, "mini-input", 3072, nullptr, 4, nullptr);
  const BaseType_t audioTaskResult = xTaskCreate(
      audioRenderTask, "mini-audio", 4096, nullptr, 3, nullptr);

  if (inputTaskResult != pdPASS || audioTaskResult != pdPASS) {
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  }

  keyboardReportQueue = xQueueCreate(
      KEYBOARD_REPORT_QUEUE_DEPTH, sizeof(KeyboardReportRequest));
  if (keyboardReportQueue != nullptr) {
    const BaseType_t keyboardTaskResult = xTaskCreate(
        keyboardHidSendTask, "ble-hid-send", 4096, nullptr, 1, nullptr);
    if (keyboardTaskResult != pdPASS) {
      portENTER_CRITICAL(&stateMux);
      sharedKeyboardBleState = KeyboardBleState::Error;
      portEXIT_CRITICAL(&stateMux);
    }
  } else {
    portENTER_CRITICAL(&stateMux);
    sharedKeyboardBleState = KeyboardBleState::Error;
    portEXIT_CRITICAL(&stateMux);
  }

  const SharedPerformanceState initialState = readPerformanceState();
  updateDisplay(readActiveApp(), initialState);
  lastUiRefreshAt = millis();
  lastDisplayedNoteMask = initialState.noteMask;
  lastDisplayedOctaveOffset = initialState.octaveOffset;
  lastDisplayedApp = readActiveApp();
}

void loop() {
  serviceUsbProtocol();
  const uint32_t now = millis();
  serviceUsbTimeouts(now);
  serviceRuntimeCatalogRefresh();
  serviceSelectedSongLoad();
  serviceSettingsPersistence();

  const SharedPerformanceState state = readPerformanceState();
  const AppId activeApp = readActiveApp();
  const SongSnapshot song = readSongSnapshot();
  const SettingsSnapshot settings = readSettingsSnapshot();
  const SongTimbre playTimbre = settings.playTimbre;
  uint16_t keyboardPhysicalMask;
  KeyboardBleState keyboardBleState;
  bool keyboardReleaseGate;
  bool globalBlePairingActive;
  portENTER_CRITICAL(&stateMux);
  keyboardPhysicalMask = sharedKeyboardPhysicalMask;
  keyboardBleState = sharedKeyboardBleState;
  keyboardReleaseGate = sharedKeyboardReleaseGate;
  globalBlePairingActive = sharedGlobalBlePairingActive;
  portEXIT_CRITICAL(&stateMux);

  if (audioWriteFailed) {
    digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
  }

  const uint32_t songSecond = song.positionSamples / SAMPLE_RATE_HZ;
  const uint8_t globalBlePairingSecond =
      globalBlePairingSecondsRemaining(now);
  const bool displayStateChanged =
      activeApp != lastDisplayedApp ||
      state.noteMask != lastDisplayedNoteMask ||
      state.octaveOffset != lastDisplayedOctaveOffset ||
      playTimbre != lastDisplayedPlayTimbre ||
      song.songIndex != lastDisplayedSongIndex ||
      song.status != lastDisplayedSongStatus ||
      song.timbre != lastDisplayedSongTimbre ||
      song.loadState != lastDisplayedSongLoadState ||
      settings.revision != lastDisplayedSettingsRevision ||
      songSecond != lastDisplayedSongSecond ||
      keyboardPhysicalMask != lastDisplayedKeyboardPhysicalMask ||
      keyboardBleState != lastDisplayedKeyboardBleState ||
      keyboardReleaseGate != lastDisplayedKeyboardReleaseGate ||
      globalBlePairingActive != lastDisplayedGlobalBlePairingActive ||
      globalBlePairingSecond != lastDisplayedGlobalBlePairingSecond ||
      usbOverlayRevision != lastDisplayedUsbOverlayRevision;
  const bool changeRefreshDue =
      displayStateChanged && now - lastUiRefreshAt >= UI_MIN_REFRESH_MS;
  const bool periodicRefreshDue =
      now - lastUiRefreshAt >= UI_PERIODIC_REFRESH_MS;

  if (changeRefreshDue || periodicRefreshDue) {
    updateDisplay(activeApp, state);
    lastUiRefreshAt = now;
    lastDisplayedNoteMask = state.noteMask;
    lastDisplayedOctaveOffset = state.octaveOffset;
    lastDisplayedApp = activeApp;
    lastDisplayedPlayTimbre = playTimbre;
    lastDisplayedSongIndex = song.songIndex;
    lastDisplayedSongStatus = song.status;
    lastDisplayedSongTimbre = song.timbre;
    lastDisplayedSongLoadState = song.loadState;
    lastDisplayedSettingsRevision = settings.revision;
    lastDisplayedSongSecond = songSecond;
    lastDisplayedKeyboardPhysicalMask = keyboardPhysicalMask;
    lastDisplayedKeyboardBleState = keyboardBleState;
    lastDisplayedKeyboardReleaseGate = keyboardReleaseGate;
    lastDisplayedGlobalBlePairingActive = globalBlePairingActive;
    lastDisplayedGlobalBlePairingSecond = globalBlePairingSecond;
    lastDisplayedUsbOverlayRevision = usbOverlayRevision;
  }

  delay(1);
}
