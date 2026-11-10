#pragma once
#include <Arduino.h>

// STM32 firmware update through the custom STM32 bootloader.
// STATUS: interface only. The flow is specified in
// /workspace/esp32_review/bootloader_protocol.md (sections 2-6) and is paused
// until the STM32 bootloader exists and the flash size (C6 vs C8) is
// confirmed over SWD. Every call returns NotImplemented today so callers and
// the backend contract can be wired up without risk to the power stage.
namespace stm32update {
enum class State : uint8_t { Idle, Downloading, Staged, RequestEntry, WaitBootloader, Info, Erase, Write, Verify, SetValid, Boot, Confirm, Done, Failed };
enum class Result : uint8_t { Ok, NotImplemented, Busy, BadImage, NotIdle, LinkError };

struct StagedImage {   // 64-byte header in the raw "stm32fw" partition (PCST)
  uint32_t fwVersion = 0, size = 0, crc32 = 0, loadAddr = 0x08002000;
  char hwModel[16] = {};
};

inline State state() { return State::Idle; }
inline const char *stateName(State) { return "idle"; }
// Download + verify (SHA-256 from the manifest, CRC32 from the header) into the staging partition.
inline Result stage(const String & /*downloadPath*/, size_t /*size*/, const String & /*sha256*/) { return Result::NotImplemented; }
// Run the flashing state machine once the STM32 reports SYS_IDLE.
inline Result flashStaged() { return Result::NotImplemented; }
inline void service() {}
}  // namespace stm32update
