#include <Arduino.h>
#include <FFat.h>
#include <MiniSynthPins.h>
#include <FS.h>
#include <string.h>

namespace {

// Hardware CDC/JTAG is selected by the project FQBN (USBMode=hwcdc). This
// sketch deliberately uses Serial and does not include or initialize TinyUSB.
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr char FRAME_MAGIC[4] = {'M', 'U', 'S', 'B'};
constexpr uint32_t MAX_METADATA_BYTES = 4096;
constexpr uint32_t MAX_MSPKG_PAYLOAD_BYTES = 512UL * 1024UL;
constexpr uint32_t MAX_FILE_BYTES = 32UL + MAX_METADATA_BYTES + MAX_MSPKG_PAYLOAD_BYTES;
constexpr size_t SERIAL_RX_BUFFER_BYTES = 2048;
constexpr uint16_t MAX_FRAME_PAYLOAD = 1152;
constexpr uint16_t MAX_CHUNK_BYTES = 1024;
constexpr uint8_t MAX_FILENAME_BYTES = 48;
constexpr uint32_t UPLOAD_IDLE_TIMEOUT_MS = 30000;
constexpr char TEMP_PATH[] = "/.~usb-upload.tmp";

static_assert(MAX_FILE_BYTES == 528416UL, "Unexpected MSPKG upload boundary");

enum Command : uint8_t {
  CMD_LIST = 0x01,
  CMD_STATUS = 0x02,
  CMD_BEGIN = 0x03,
  CMD_CHUNK = 0x04,
  CMD_FINISH = 0x05,
  CMD_ABORT = 0x06,
  RSP_ACK = 0x80,
  RSP_ERROR = 0x81,
  RSP_LIST_ENTRY = 0x82,
  RSP_LIST_DONE = 0x83,
  RSP_STATUS = 0x84,
};

enum DeviceState : uint8_t {
  STATE_IDLE = 0,
  STATE_RECEIVING = 1,
  STATE_VALIDATING = 2,
  STATE_COMMITTING = 3,
  STATE_ERROR = 4,
};

enum ErrorCode : uint16_t {
  ERR_OK = 0,
  ERR_BAD_FRAME_CRC = 1,
  ERR_PROTOCOL_VERSION = 2,
  ERR_UNKNOWN_COMMAND = 3,
  ERR_BAD_PAYLOAD = 4,
  ERR_UNSAFE_FILENAME = 5,
  ERR_FILE_TOO_LARGE = 6,
  ERR_FS_NOT_MOUNTED = 7,
  ERR_BUSY = 8,
  ERR_NO_UPLOAD = 9,
  ERR_OFFSET_MISMATCH = 10,
  ERR_CHUNK_LENGTH = 11,
  ERR_FILE_IO = 12,
  ERR_FILE_LENGTH = 13,
  ERR_FILE_CRC = 14,
  ERR_MSPKG_MAGIC = 15,
  ERR_MSPKG_VERSION = 16,
  ERR_MSPKG_TYPE = 17,
  ERR_MSPKG_DECLARED_SIZE = 18,
  ERR_METADATA_CRC = 19,
  ERR_PAYLOAD_CRC = 20,
  ERR_PAYLOAD_FORMAT = 21,
  ERR_COMMIT_FAILED = 22,
  ERR_ROLLBACK_FAILED = 23,
  ERR_NO_SPACE = 24,
  ERR_FRAME_TOO_LARGE = 25,
};

struct FrameHeader {
  char magic[4];
  uint8_t version;
  uint8_t command;
  uint16_t flags;
  uint32_t sequence;
  uint32_t payloadLength;
  uint32_t payloadCrc32;
} __attribute__((packed));
static_assert(sizeof(FrameHeader) == 20, "Unexpected protocol header size");
static_assert(SERIAL_RX_BUFFER_BYTES >= sizeof(FrameHeader) + MAX_FRAME_PAYLOAD,
              "Hardware CDC RX buffer must hold one maximum-size protocol frame");

struct AckPayload {
  uint8_t requestCommand;
  uint8_t state;
  uint16_t code;
  uint32_t nextOffset;
  uint32_t totalLength;
  uint32_t fileCrc32;
} __attribute__((packed));
static_assert(sizeof(AckPayload) == 16, "Unexpected ACK payload size");

struct StatusPayload {
  uint8_t state;
  uint8_t mounted;
  uint16_t code;
  uint32_t nextOffset;
  uint32_t totalLength;
  uint32_t expectedCrc32;
  uint32_t runningCrc32;
} __attribute__((packed));
static_assert(sizeof(StatusPayload) == 20, "Unexpected status payload size");

struct MspkgHeader {
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
static_assert(sizeof(MspkgHeader) == 32, "Unexpected MSPKG header size");

struct TlvHeader {
  uint16_t tag;
  uint8_t type;
  uint8_t flags;
  uint32_t length;
} __attribute__((packed));
static_assert(sizeof(TlvHeader) == 8, "Unexpected TLV header size");

struct SequenceHeader {
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
static_assert(sizeof(SequenceHeader) == 28, "Unexpected sequence header size");

bool ffatMounted = false;
DeviceState deviceState = STATE_IDLE;
ErrorCode lastResult = ERR_OK;
char uploadFilename[MAX_FILENAME_BYTES + 1] = {};
uint32_t uploadLength = 0;
uint32_t uploadExpectedCrc = 0;
uint32_t uploadRunningCrc = 0;
uint32_t uploadOffset = 0;
uint32_t lastUploadActivityMs = 0;
File uploadFile;

uint8_t framePayload[MAX_FRAME_PAYLOAD];
FrameHeader incomingHeader = {};
uint8_t incomingHeaderBytes = 0;
uint32_t incomingPayloadBytes = 0;
uint8_t magicMatchBytes = 0;
uint32_t lastParserByteMs = 0;
constexpr uint32_t PARSER_IDLE_TIMEOUT_MS = 1000;

// A retried host request uses the same sequence. Caching upload ACK/ERROR
// responses prevents BEGIN from truncating or CHUNK from being applied twice.
bool responseCacheValid = false;
uint32_t cachedSequence = 0;
uint8_t cachedRequestCommand = 0;
uint8_t cachedResponseCommand = 0;
AckPayload cachedAck = {};

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t length) {
  crc = ~crc;
  while (length-- > 0) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

void writeFrame(uint8_t command, uint32_t sequence, const void *payload, uint32_t length) {
  FrameHeader header = {};
  memcpy(header.magic, FRAME_MAGIC, sizeof(header.magic));
  header.version = PROTOCOL_VERSION;
  header.command = command;
  header.flags = 0;
  header.sequence = sequence;
  header.payloadLength = length;
  header.payloadCrc32 = crc32Update(0, static_cast<const uint8_t *>(payload), length);
  Serial.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  if (length > 0) {
    Serial.write(static_cast<const uint8_t *>(payload), length);
  }
}

void sendAckLike(uint8_t responseCommand,
                 uint8_t requestCommand,
                 uint32_t sequence,
                 ErrorCode code,
                 bool cache = true) {
  AckPayload payload = {
      requestCommand,
      static_cast<uint8_t>(deviceState),
      static_cast<uint16_t>(code),
      uploadOffset,
      uploadLength,
      uploadExpectedCrc,
  };
  writeFrame(responseCommand, sequence, &payload, sizeof(payload));
  if (cache) {
    responseCacheValid = true;
    cachedSequence = sequence;
    cachedRequestCommand = requestCommand;
    cachedResponseCommand = responseCommand;
    cachedAck = payload;
  }
}

void sendError(uint8_t requestCommand, uint32_t sequence, ErrorCode code, bool cache = true) {
  lastResult = code;
  sendAckLike(RSP_ERROR, requestCommand, sequence, code, cache);
}

void sendAck(uint8_t requestCommand, uint32_t sequence, ErrorCode code = ERR_OK) {
  lastResult = code;
  sendAckLike(RSP_ACK, requestCommand, sequence, code, true);
}

bool safeFilename(const char *name, size_t length) {
  if (length == 0 || length > MAX_FILENAME_BYTES || name[0] == '.') return false;
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
  if (extension == nullptr) return false;
  return strcasecmp(extension, ".mspkg") == 0 || strcasecmp(extension, ".msp") == 0;
}

bool packageFilename(const char *name) {
  if (name == nullptr || name[0] == '.') return false;
  const char *extension = strrchr(name, '.');
  return extension != nullptr &&
         (strcasecmp(extension, ".mspkg") == 0 || strcasecmp(extension, ".msp") == 0);
}

void makeFinalPath(const char *filename, char *path, size_t pathBytes) {
  snprintf(path, pathBytes, "/%s", filename);
}

void makeBackupPath(const char *filename, char *path, size_t pathBytes) {
  snprintf(path, pathBytes, "/.~%s.bak", filename);
}

bool endsWith(const char *text, const char *suffix) {
  const size_t textLength = strlen(text);
  const size_t suffixLength = strlen(suffix);
  return textLength >= suffixLength && strcmp(text + textLength - suffixLength, suffix) == 0;
}

void recoverInterruptedCommits() {
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
    if (strncmp(name, ".~", 2) == 0 && endsWith(name, ".bak")) {
      const size_t nameLength = strlen(name);
      const size_t originalLength = nameLength - 2U - 4U;
      if (originalLength > 0 && originalLength <= MAX_FILENAME_BYTES) {
        char original[MAX_FILENAME_BYTES + 1] = {};
        memcpy(original, name + 2, originalLength);
        if (safeFilename(original, originalLength)) {
          char backupPath[80];
          char finalPath[64];
          snprintf(backupPath, sizeof(backupPath), "/%s", name);
          makeFinalPath(original, finalPath, sizeof(finalPath));
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
  // A transfer temp is never a catalog file and is safe to discard after reset.
  if (FFat.exists(TEMP_PATH)) FFat.remove(TEMP_PATH);
}

void clearUpload(bool removeTemporary) {
  if (uploadFile) uploadFile.close();
  if (removeTemporary && ffatMounted && FFat.exists(TEMP_PATH)) {
    FFat.remove(TEMP_PATH);
  }
  uploadFilename[0] = '\0';
  uploadLength = 0;
  uploadExpectedCrc = 0;
  uploadRunningCrc = 0;
  uploadOffset = 0;
  deviceState = STATE_IDLE;
}

bool readExact(File &file, void *destination, size_t length) {
  return file.read(static_cast<uint8_t *>(destination), length) == length;
}

bool crcFileRange(File &file, uint32_t offset, uint32_t length, uint32_t &result) {
  if (!file.seek(offset)) return false;
  uint8_t buffer[256];
  result = 0;
  while (length > 0) {
    const size_t amount = length < sizeof(buffer) ? length : sizeof(buffer);
    if (file.read(buffer, amount) != amount) return false;
    result = crc32Update(result, buffer, amount);
    length -= static_cast<uint32_t>(amount);
  }
  return true;
}

ErrorCode validateMspkg(const char *path) {
  File file = FFat.open(path, FILE_READ);
  if (!file || file.isDirectory()) return ERR_FILE_IO;
  const uint32_t fileSize = static_cast<uint32_t>(file.size());
  MspkgHeader header;
  if (!readExact(file, &header, sizeof(header))) {
    file.close();
    return ERR_MSPKG_DECLARED_SIZE;
  }
  if (memcmp(header.magic, "MSPK", 4) != 0) {
    file.close();
    return ERR_MSPKG_MAGIC;
  }
  if (header.major != 1 || header.minor != 0 || header.flags != 0 ||
      header.minimumFirmwareAbi > 1) {
    file.close();
    return ERR_MSPKG_VERSION;
  }
  if (header.contentType != 1) {
    file.close();
    return ERR_MSPKG_TYPE;
  }
  const uint64_t declaredSize = static_cast<uint64_t>(header.headerBytes) +
                                header.metadataBytes + header.payloadBytes;
  if (header.headerBytes != sizeof(MspkgHeader) ||
      header.metadataBytes > MAX_METADATA_BYTES ||
      header.payloadBytes > MAX_MSPKG_PAYLOAD_BYTES ||
      declaredSize != fileSize) {
    file.close();
    return ERR_MSPKG_DECLARED_SIZE;
  }
  uint32_t metadataCrc = 0;
  uint32_t payloadCrc = 0;
  if (!crcFileRange(file, header.headerBytes, header.metadataBytes, metadataCrc) ||
      !crcFileRange(file, header.headerBytes + header.metadataBytes,
                    header.payloadBytes, payloadCrc)) {
    file.close();
    return ERR_FILE_IO;
  }
  if (metadataCrc != header.metadataCrc32) {
    file.close();
    return ERR_METADATA_CRC;
  }
  if (payloadCrc != header.payloadCrc32) {
    file.close();
    return ERR_PAYLOAD_CRC;
  }

  // Check that metadata is a complete sequence of bounded TLVs.
  if (!file.seek(header.headerBytes)) {
    file.close();
    return ERR_FILE_IO;
  }
  uint32_t metadataRemaining = header.metadataBytes;
  while (metadataRemaining > 0) {
    if (metadataRemaining < sizeof(TlvHeader)) {
      file.close();
      return ERR_MSPKG_DECLARED_SIZE;
    }
    TlvHeader tlv;
    if (!readExact(file, &tlv, sizeof(tlv))) {
      file.close();
      return ERR_FILE_IO;
    }
    metadataRemaining -= sizeof(tlv);
    if (tlv.length > metadataRemaining || !file.seek(file.position() + tlv.length)) {
      file.close();
      return ERR_MSPKG_DECLARED_SIZE;
    }
    metadataRemaining -= tlv.length;
  }

  if (header.payloadBytes < sizeof(SequenceHeader) ||
      !file.seek(header.headerBytes + header.metadataBytes)) {
    file.close();
    return ERR_PAYLOAD_FORMAT;
  }
  SequenceHeader sequence;
  if (!readExact(file, &sequence, sizeof(sequence)) ||
      memcmp(sequence.magic, "MSQ1", 4) != 0 ||
      sequence.ticksPerQuarter == 0 || sequence.noteRecordBytes != 12 ||
      sequence.noteEventCount == 0 || sequence.tempoRecordCount == 0 ||
      sequence.reserved16 != 0 || sequence.reserved32 != 0) {
    file.close();
    return ERR_PAYLOAD_FORMAT;
  }
  const uint64_t expectedPayload = sizeof(SequenceHeader) +
      static_cast<uint64_t>(sequence.tempoRecordCount) * 8ULL +
      static_cast<uint64_t>(sequence.timeSignatureCount) * 8ULL +
      static_cast<uint64_t>(sequence.noteEventCount) * 12ULL;
  file.close();
  return expectedPayload == header.payloadBytes ? ERR_OK : ERR_MSPKG_DECLARED_SIZE;
}

ErrorCode commitUpload() {
  char finalPath[64];
  char backupPath[80];
  makeFinalPath(uploadFilename, finalPath, sizeof(finalPath));
  makeBackupPath(uploadFilename, backupPath, sizeof(backupPath));

  // Resolve stale state deterministically before starting a new replacement.
  if (FFat.exists(backupPath)) {
    if (FFat.exists(finalPath)) {
      if (!FFat.remove(backupPath)) return ERR_COMMIT_FAILED;
    } else if (!FFat.rename(backupPath, finalPath)) {
      return ERR_ROLLBACK_FAILED;
    }
  }

  const bool hadFinal = FFat.exists(finalPath);
  if (hadFinal && !FFat.rename(finalPath, backupPath)) return ERR_COMMIT_FAILED;
  if (!FFat.rename(TEMP_PATH, finalPath)) {
    if (hadFinal && !FFat.rename(backupPath, finalPath)) return ERR_ROLLBACK_FAILED;
    return ERR_COMMIT_FAILED;
  }
  if (hadFinal && FFat.exists(backupPath) && !FFat.remove(backupPath)) {
    // Both files are valid at this point. Startup recovery will keep the new
    // final and delete this stale backup if power is lost before another call.
    return ERR_COMMIT_FAILED;
  }
  return ERR_OK;
}

void handleList(uint32_t sequence) {
  if (!ffatMounted) {
    sendError(CMD_LIST, sequence, ERR_FS_NOT_MOUNTED, false);
    return;
  }
  File root = FFat.open("/");
  if (!root || !root.isDirectory()) {
    sendError(CMD_LIST, sequence, ERR_FILE_IO, false);
    return;
  }
  uint16_t count = 0;
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char *rawName = entry.name();
      const char *leaf = rawName == nullptr ? nullptr : strrchr(rawName, '/');
      leaf = leaf == nullptr ? rawName : leaf + 1;
      if (leaf != nullptr && packageFilename(leaf)) {
        const size_t nameLength = strlen(leaf);
        if (nameLength <= MAX_FILENAME_BYTES) {
          uint8_t payload[5 + MAX_FILENAME_BYTES];
          const uint32_t size = static_cast<uint32_t>(entry.size());
          memcpy(payload, &size, 4);
          payload[4] = static_cast<uint8_t>(nameLength);
          memcpy(payload + 5, leaf, nameLength);
          writeFrame(RSP_LIST_ENTRY, sequence, payload, 5 + nameLength);
          ++count;
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  writeFrame(RSP_LIST_DONE, sequence, &count, sizeof(count));
}

void handleStatus(uint32_t sequence) {
  StatusPayload payload = {
      static_cast<uint8_t>(deviceState),
      static_cast<uint8_t>(ffatMounted ? 1 : 0),
      static_cast<uint16_t>(lastResult),
      uploadOffset,
      uploadLength,
      uploadExpectedCrc,
      uploadRunningCrc,
  };
  writeFrame(RSP_STATUS, sequence, &payload, sizeof(payload));
}

void handleBegin(uint32_t sequence, const uint8_t *payload, uint32_t length) {
  if (!ffatMounted) {
    sendError(CMD_BEGIN, sequence, ERR_FS_NOT_MOUNTED);
    return;
  }
  if (length < 9) {
    sendError(CMD_BEGIN, sequence, ERR_BAD_PAYLOAD);
    return;
  }
  uint32_t fileLength;
  uint32_t fileCrc;
  memcpy(&fileLength, payload, 4);
  memcpy(&fileCrc, payload + 4, 4);
  const uint8_t nameLength = payload[8];
  if (length != 9U + nameLength || nameLength == 0 || nameLength > MAX_FILENAME_BYTES) {
    sendError(CMD_BEGIN, sequence, ERR_BAD_PAYLOAD);
    return;
  }
  char filename[MAX_FILENAME_BYTES + 1] = {};
  memcpy(filename, payload + 9, nameLength);
  if (!safeFilename(filename, nameLength)) {
    sendError(CMD_BEGIN, sequence, ERR_UNSAFE_FILENAME);
    return;
  }
  if (fileLength < sizeof(MspkgHeader) || fileLength > MAX_FILE_BYTES) {
    sendError(CMD_BEGIN, sequence, ERR_FILE_TOO_LARGE);
    return;
  }
  if (deviceState == STATE_RECEIVING) {
    if (strcmp(filename, uploadFilename) == 0 && fileLength == uploadLength &&
        fileCrc == uploadExpectedCrc) {
      lastUploadActivityMs = millis();
      sendAck(CMD_BEGIN, sequence);
    } else {
      sendError(CMD_BEGIN, sequence, ERR_BUSY);
    }
    return;
  }
  if (deviceState != STATE_IDLE) {
    sendError(CMD_BEGIN, sequence, ERR_BUSY);
    return;
  }
  clearUpload(true);
  uploadFile = FFat.open(TEMP_PATH, FILE_WRITE);
  if (!uploadFile) {
    sendError(CMD_BEGIN, sequence, ERR_FILE_IO);
    return;
  }
  strncpy(uploadFilename, filename, sizeof(uploadFilename) - 1U);
  uploadLength = fileLength;
  uploadExpectedCrc = fileCrc;
  uploadRunningCrc = 0;
  uploadOffset = 0;
  lastUploadActivityMs = millis();
  deviceState = STATE_RECEIVING;
  sendAck(CMD_BEGIN, sequence);
}

void handleChunk(uint32_t sequence, const uint8_t *payload, uint32_t length) {
  if (deviceState != STATE_RECEIVING || !uploadFile) {
    sendError(CMD_CHUNK, sequence, ERR_NO_UPLOAD);
    return;
  }
  if (length < 8) {
    sendError(CMD_CHUNK, sequence, ERR_BAD_PAYLOAD);
    return;
  }
  uint32_t offset;
  uint16_t chunkLength;
  uint16_t reserved;
  memcpy(&offset, payload, 4);
  memcpy(&chunkLength, payload + 4, 2);
  memcpy(&reserved, payload + 6, 2);
  if (reserved != 0 || chunkLength == 0 || chunkLength > MAX_CHUNK_BYTES ||
      length != 8U + chunkLength || offset > uploadLength ||
      chunkLength > uploadLength - offset) {
    sendError(CMD_CHUNK, sequence, ERR_CHUNK_LENGTH);
    return;
  }
  const uint8_t *chunk = payload + 8;
  if (offset == uploadOffset) {
    if (uploadFile.write(chunk, chunkLength) != chunkLength) {
      sendError(CMD_CHUNK, sequence, ERR_NO_SPACE);
      return;
    }
    uploadRunningCrc = crc32Update(uploadRunningCrc, chunk, chunkLength);
    uploadOffset += chunkLength;
  } else if (offset < uploadOffset && chunkLength <= uploadOffset - offset) {
    // A duplicate after a lost ACK is accepted only if bytes match exactly.
    // Keep the sequential writer open and compare through a separate read
    // handle because FILE_WRITE is not guaranteed to be readable.
    uploadFile.flush();
    File compareFile = FFat.open(TEMP_PATH, FILE_READ);
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
      sendError(CMD_CHUNK, sequence, ERR_OFFSET_MISMATCH);
      return;
    }
  } else {
    sendError(CMD_CHUNK, sequence, ERR_OFFSET_MISMATCH);
    return;
  }
  lastUploadActivityMs = millis();
  sendAck(CMD_CHUNK, sequence);
}

void handleFinish(uint32_t sequence, uint32_t length) {
  if (length != 0) {
    sendError(CMD_FINISH, sequence, ERR_BAD_PAYLOAD);
    return;
  }
  if (deviceState != STATE_RECEIVING || !uploadFile) {
    sendError(CMD_FINISH, sequence, ERR_NO_UPLOAD);
    return;
  }
  if (uploadOffset != uploadLength) {
    sendError(CMD_FINISH, sequence, ERR_FILE_LENGTH);
    return;
  }
  uploadFile.flush();
  uploadFile.close();
  if (uploadRunningCrc != uploadExpectedCrc) {
    deviceState = STATE_ERROR;
    sendError(CMD_FINISH, sequence, ERR_FILE_CRC);
    clearUpload(true);
    return;
  }
  deviceState = STATE_VALIDATING;
  ErrorCode result = validateMspkg(TEMP_PATH);
  if (result != ERR_OK) {
    deviceState = STATE_ERROR;
    sendError(CMD_FINISH, sequence, result);
    clearUpload(true);
    return;
  }
  deviceState = STATE_COMMITTING;
  result = commitUpload();
  if (result != ERR_OK) {
    deviceState = STATE_ERROR;
    sendError(CMD_FINISH, sequence, result);
    // Preserve TEMP_PATH on a commit failure for diagnosis. The final path is
    // either untouched/restored, or startup recovery can use the backup after
    // ERR_ROLLBACK_FAILED. Keep STATE_ERROR and lastResult observable.
    uploadFilename[0] = '\0';
    uploadLength = 0;
    uploadExpectedCrc = 0;
    uploadRunningCrc = 0;
    uploadOffset = 0;
    return;
  }
  deviceState = STATE_IDLE;
  lastResult = ERR_OK;
  // Keep completion values in the ACK; clear only after it is cached/sent.
  sendAck(CMD_FINISH, sequence);
  uploadFilename[0] = '\0';
  uploadLength = 0;
  uploadExpectedCrc = 0;
  uploadRunningCrc = 0;
  uploadOffset = 0;
}

void handleAbort(uint32_t sequence, uint32_t length) {
  if (length != 0) {
    sendError(CMD_ABORT, sequence, ERR_BAD_PAYLOAD);
    return;
  }
  clearUpload(true);
  lastResult = ERR_OK;
  sendAck(CMD_ABORT, sequence);
}

void handleFrame(const FrameHeader &header, const uint8_t *payload) {
  if (responseCacheValid && header.sequence == cachedSequence &&
      header.command == cachedRequestCommand) {
    writeFrame(cachedResponseCommand, header.sequence, &cachedAck, sizeof(cachedAck));
    return;
  }
  switch (header.command) {
    case CMD_LIST:
      if (header.payloadLength != 0) sendError(CMD_LIST, header.sequence, ERR_BAD_PAYLOAD, false);
      else handleList(header.sequence);
      break;
    case CMD_STATUS:
      if (header.payloadLength != 0) sendError(CMD_STATUS, header.sequence, ERR_BAD_PAYLOAD, false);
      else handleStatus(header.sequence);
      break;
    case CMD_BEGIN:
      handleBegin(header.sequence, payload, header.payloadLength);
      break;
    case CMD_CHUNK:
      handleChunk(header.sequence, payload, header.payloadLength);
      break;
    case CMD_FINISH:
      handleFinish(header.sequence, header.payloadLength);
      break;
    case CMD_ABORT:
      handleAbort(header.sequence, header.payloadLength);
      break;
    default:
      sendError(header.command, header.sequence, ERR_UNKNOWN_COMMAND, false);
      break;
  }
}

void resetParser() {
  incomingHeader = {};
  incomingHeaderBytes = 0;
  incomingPayloadBytes = 0;
  magicMatchBytes = 0;
}

void feedProtocolByte(uint8_t value) {
  lastParserByteMs = millis();
  if (incomingHeaderBytes < sizeof(FRAME_MAGIC)) {
    if (value == static_cast<uint8_t>(FRAME_MAGIC[magicMatchBytes])) {
      reinterpret_cast<uint8_t *>(&incomingHeader)[magicMatchBytes] = value;
      ++magicMatchBytes;
      incomingHeaderBytes = magicMatchBytes;
      return;
    }
    magicMatchBytes = value == static_cast<uint8_t>(FRAME_MAGIC[0]) ? 1 : 0;
    incomingHeaderBytes = magicMatchBytes;
    if (magicMatchBytes == 1) reinterpret_cast<uint8_t *>(&incomingHeader)[0] = value;
    return;
  }

  if (incomingHeaderBytes < sizeof(FrameHeader)) {
    reinterpret_cast<uint8_t *>(&incomingHeader)[incomingHeaderBytes++] = value;
    if (incomingHeaderBytes == sizeof(FrameHeader)) {
      if (incomingHeader.version != PROTOCOL_VERSION) {
        sendError(incomingHeader.command, incomingHeader.sequence, ERR_PROTOCOL_VERSION, false);
        resetParser();
      } else if (incomingHeader.payloadLength > MAX_FRAME_PAYLOAD) {
        sendError(incomingHeader.command, incomingHeader.sequence, ERR_FRAME_TOO_LARGE, false);
        resetParser();
      } else if (incomingHeader.payloadLength == 0) {
        if (incomingHeader.payloadCrc32 != 0) {
          sendError(incomingHeader.command, incomingHeader.sequence, ERR_BAD_FRAME_CRC, false);
        } else {
          handleFrame(incomingHeader, framePayload);
        }
        resetParser();
      }
    }
    return;
  }

  framePayload[incomingPayloadBytes++] = value;
  if (incomingPayloadBytes == incomingHeader.payloadLength) {
    if (crc32Update(0, framePayload, incomingPayloadBytes) != incomingHeader.payloadCrc32) {
      sendError(incomingHeader.command, incomingHeader.sequence, ERR_BAD_FRAME_CRC, false);
    } else {
      handleFrame(incomingHeader, framePayload);
    }
    resetParser();
  }
}

void serviceProtocol() {
  uint16_t budget = 2048;
  while (budget-- > 0 && Serial.available() > 0) {
    feedProtocolByte(static_cast<uint8_t>(Serial.read()));
  }
}

}  // namespace

void setup() {
  // First executable actions: hold NS4168 in shutdown. This importer never
  // initializes I2S and never enables the amplifier.
  pinMode(MINI_SYNTH_PIN_AMP_CTRL, OUTPUT);
  digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);

  // Arduino-ESP32 3.3.11 HWCDC defaults to a 256-byte RX queue, smaller than
  // one 1032-byte CHUNK payload plus its frame header. Preset a queue large
  // enough for the protocol maximum before begin() creates the default queue.
  Serial.setRxBufferSize(SERIAL_RX_BUFFER_BYTES);
  Serial.begin(115200);
  // Never wait for Serial: standalone boot and FFat safety recovery must work
  // with no host attached.

  ffatMounted = FFat.begin(false);  // Never format automatically.
  if (ffatMounted) {
    recoverInterruptedCommits();
  } else {
    lastResult = ERR_FS_NOT_MOUNTED;
  }
}

void loop() {
  serviceProtocol();
  const uint32_t now = millis();
  if (incomingHeaderBytes > 0 &&
      static_cast<uint32_t>(now - lastParserByteMs) > PARSER_IDLE_TIMEOUT_MS) {
    // A truncated frame must not hold the parser forever; abandon it and scan
    // for the next magic. No response is possible because the sequence may be
    // incomplete, so the host timeout/retry path supplies the recovery.
    resetParser();
  }
  if (deviceState == STATE_RECEIVING &&
      static_cast<uint32_t>(now - lastUploadActivityMs) > UPLOAD_IDLE_TIMEOUT_MS) {
    clearUpload(true);
    lastResult = ERR_NO_UPLOAD;
    responseCacheValid = false;
  }
  delay(1);
}
