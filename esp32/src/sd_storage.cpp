#include "sd_storage.h"

#include <SD.h>
#include <SPI.h>

namespace hoshino {

namespace {
// Keep first-generation jumper-wire/card-module setups conservative.  Some
// modules initialise successfully at 12 MHz but then fail their first FAT
// sector read, and Arduino-ESP32 2.x has an unsafe cleanup path for that
// particular mount failure. 1 MHz is still sufficient for the read-only
// diagnostic path and for proving the wiring/card before throughput tuning.
constexpr uint32_t kSdFrequencyHz = 1000000;
constexpr size_t kMaxRelativePathLength = 160;

bool isUnsafeSegment(const String& segment) {
  return segment == "." || segment == ".." || segment.length() > 64;
}
}  // namespace

SdStorageService::SdStorageService(int8_t csPin, int8_t sckPin, int8_t misoPin, int8_t mosiPin)
    : csPin_(csPin), sckPin_(sckPin), misoPin_(misoPin), mosiPin_(mosiPin), spi_(HSPI) {}

bool SdStorageService::ensureMounted(String& error) {
  error = "";
  if (mounted_) {
    if (SD.cardType() != CARD_NONE) return true;
    unmount();
  }

  // Keep storage on a dedicated SPI host. The global VSPI instance can carry
  // transaction state from other framework components; Arduino-ESP32 2.x may
  // panic in its mount-failure cleanup when that shared mutex is disturbed.
  spi_.begin(sckPin_, misoPin_, mosiPin_, csPin_);
  if (!SD.begin(csPin_, spi_, kSdFrequencyHz)) {
    error = "sd_mount_failed";
    spi_.end();
    mounted_ = false;
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    error = "sd_card_missing";
    SD.end();
    mounted_ = false;
    return false;
  }
  mounted_ = true;
  File root = SD.open(rootPath(), FILE_READ);
  const bool rootReady = root && root.isDirectory();
  root.close();
  if (!rootReady) {
    error = "sd_root_missing";
    unmount();
    return false;
  }
  return true;
}

void SdStorageService::unmount() {
  if (mounted_) SD.end();
  spi_.end();
  mounted_ = false;
}

SdStorageStatus SdStorageService::status() const {
  SdStorageStatus out;
  out.mounted = mounted_;
  if (!mounted_) return out;
  out.cardType = static_cast<uint8_t>(SD.cardType());
  out.cardSize = SD.cardSize();
  out.totalBytes = SD.totalBytes();
  out.usedBytes = SD.usedBytes();
  return out;
}

bool SdStorageService::resolvePath(const String& relativePath, String& fullPath, String& error, bool allowRoot) const {
  error = "";
  fullPath = "";
  String path = relativePath;
  if (path.length() > kMaxRelativePathLength) {
    error = "path_too_long";
    return false;
  }
  if (path.startsWith("/") || path.startsWith("\\") ||
      path.indexOf('\\') >= 0 || path.indexOf(':') >= 0 || path.indexOf('\0') >= 0) {
    error = "invalid_path";
    return false;
  }
  for (size_t i = 0; i < path.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(path[i]);
    if (c < 0x20 || c == 0x7f || c == '?' || c == '#') {
      error = "invalid_path_character";
      return false;
    }
  }
  while (path.endsWith("/") && !path.isEmpty()) path.remove(path.length() - 1);
  if (path.isEmpty()) {
    if (!allowRoot) {
      error = "path_required";
      return false;
    }
    fullPath = rootPath();
    return true;
  }

  String normalized;
  size_t start = 0;
  while (start < path.length()) {
    const int slash = path.indexOf('/', start);
    const size_t end = slash < 0 ? path.length() : static_cast<size_t>(slash);
    const String segment = path.substring(start, end);
    if (segment.isEmpty() || isUnsafeSegment(segment)) {
      error = "invalid_path_segment";
      return false;
    }
    for (size_t i = 0; i < segment.length(); ++i) {
      const unsigned char c = static_cast<unsigned char>(segment[i]);
      if (c < 0x20 || c == 0x7f || c == '?' || c == '#') {
        error = "invalid_path_character";
        return false;
      }
    }
    normalized += '/';
    normalized += segment;
    if (slash < 0) break;
    start = end + 1;
  }

  fullPath = String(rootPath()) + normalized;
  return true;
}

File SdStorageService::openRead(const String& relativePath, String& error) {
  if (!ensureMounted(error)) return File();
  String fullPath;
  if (!resolvePath(relativePath, fullPath, error, true)) return File();
  File file = SD.open(fullPath.c_str(), FILE_READ);
  if (!file) error = "not_found";
  return file;
}

bool SdStorageService::ensureParentDirectories(const String& fullPath, String& error) {
  error = "";
  const int lastSlash = fullPath.lastIndexOf('/');
  if (lastSlash <= 0) return true;
  String current;
  size_t start = 1;
  while (start < static_cast<size_t>(lastSlash)) {
    const int slash = fullPath.indexOf('/', start);
    const size_t end = slash < 0 || slash > lastSlash ? static_cast<size_t>(lastSlash) : static_cast<size_t>(slash);
    const String segment = fullPath.substring(start, end);
    if (!segment.isEmpty()) {
      current += '/';
      current += segment;
      if (!SD.exists(current.c_str()) && !SD.mkdir(current.c_str())) {
        error = "mkdir_failed";
        return false;
      }
    }
    if (end >= static_cast<size_t>(lastSlash)) break;
    start = end + 1;
  }
  return true;
}

File SdStorageService::openWrite(const String& relativePath, String& error, bool truncate) {
  if (!ensureMounted(error)) return File();
  String fullPath;
  if (!resolvePath(relativePath, fullPath, error, false)) return File();
  if (!ensureParentDirectories(fullPath, error)) return File();
  if (truncate && SD.exists(fullPath.c_str()) && !SD.remove(fullPath.c_str())) {
    error = "truncate_failed";
    return File();
  }
  File file = SD.open(fullPath.c_str(), FILE_WRITE);
  if (!file) error = "open_write_failed";
  return file;
}

bool SdStorageService::removeFile(const String& relativePath, String& error) {
  if (!ensureMounted(error)) return false;
  String fullPath;
  if (!resolvePath(relativePath, fullPath, error, false)) return false;
  if (!SD.exists(fullPath.c_str())) {
    error = "not_found";
    return false;
  }
  if (!SD.remove(fullPath.c_str())) {
    error = "remove_failed";
    return false;
  }
  return true;
}

}  // namespace hoshino
