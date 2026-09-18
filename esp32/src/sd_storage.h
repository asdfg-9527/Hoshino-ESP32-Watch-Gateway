#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SPI.h>

namespace hoshino {

struct SdStorageStatus {
  bool mounted = false;
  uint8_t cardType = 0;
  uint64_t cardSize = 0;
  uint64_t totalBytes = 0;
  uint64_t usedBytes = 0;
};

class SdStorageService {
 public:
  SdStorageService(int8_t csPin = 5, int8_t sckPin = 18, int8_t misoPin = 19, int8_t mosiPin = 23);

  // Mounts lazily so Bluetooth authentication keeps as much heap as possible.
  bool ensureMounted(String& error);
  void unmount();
  bool isMounted() const { return mounted_; }
  SdStorageStatus status() const;

  // All user-visible paths are forced below /hoshino on the card.
  bool resolvePath(const String& relativePath, String& fullPath, String& error, bool allowRoot = true) const;
  File openRead(const String& relativePath, String& error);
  File openWrite(const String& relativePath, String& error, bool truncate = true);
  bool removeFile(const String& relativePath, String& error);
  bool ensureParentDirectories(const String& fullPath, String& error);

  static const char* rootPath() { return "/hoshino"; }

 private:
  int8_t csPin_;
  int8_t sckPin_;
  int8_t misoPin_;
  int8_t mosiPin_;
  SPIClass spi_;
  bool mounted_ = false;
};

}  // namespace hoshino
