#include "journal_files.h"

#include "load_ac_energy.h"

#include <string.h>

namespace loadac {
namespace {

bool path_valid(const JournalDirectory &dir, const char *path) {
  if (!path || dir.file_size(path) != kStoreBytes) return false;
  uint8_t header[kHeaderBytes];
  if (!dir.read_at(path, 0, header, kHeaderBytes) || !journal_header_valid(header)) return false;
  uint8_t slot[kDefaultSlotBytes];
  for (uint32_t i = 0; i < kDefaultSlots; ++i) {
    uint32_t off = kHeaderBytes + i * kDefaultSlotBytes;
    if (!dir.read_at(path, off, slot, kDefaultSlotBytes) || !journal_slot_valid(slot)) return false;
  }
  return true;
}

}  // namespace

bool journal_verify_path(const JournalDirectory &dir, const char *path, const uint8_t *image, uint32_t n) {
  if (!path || !image || dir.file_size(path) != n) return false;
  uint8_t chunk[128];
  uint32_t off = 0;
  while (off < n) {
    uint32_t want = n - off;
    if (want > sizeof(chunk)) want = sizeof(chunk);
    if (!dir.read_at(path, off, chunk, want) || memcmp(chunk, image + off, want) != 0) return false;
    off += want;
  }
  return journal_image_valid(image, n);
}

bool journal_write_tmp(JournalDirectory &dir, const JournalPaths &paths, const uint8_t *image, uint32_t n) {
  if (!paths.tmp_path || !journal_image_valid(image, n)) return false;
  if (!dir.write_all(paths.tmp_path, image, n)) return false;
  if (!journal_verify_path(dir, paths.tmp_path, image, n)) {
    dir.remove_file(paths.tmp_path);
    return false;
  }
  return true;
}

bool journal_move_main_to_bak(JournalDirectory &dir, const JournalPaths &paths) {
  if (!paths.main_path || !paths.bak_path) return false;
  if (!dir.exists(paths.main_path)) return true;
  if (dir.exists(paths.bak_path) && !dir.remove_file(paths.bak_path)) return false;
  return dir.rename_file(paths.main_path, paths.bak_path);
}

bool journal_rename_tmp_to_main(JournalDirectory &dir, const JournalPaths &paths) {
  if (!paths.tmp_path || !paths.main_path || !paths.bak_path) return false;
  if (dir.exists(paths.main_path)) {
    if (!path_valid(dir, paths.bak_path)) return false;
    if (!dir.remove_file(paths.main_path)) return false;
  }
  return dir.rename_file(paths.tmp_path, paths.main_path);
}

bool journal_restore_main_from_bak(JournalDirectory &dir, const JournalPaths &paths) {
  if (path_valid(dir, paths.main_path)) return true;
  if (dir.exists(paths.main_path) && !dir.remove_file(paths.main_path)) return false;
  if (!path_valid(dir, paths.bak_path)) return false;
  if (!dir.rename_file(paths.bak_path, paths.main_path)) return false;
  return path_valid(dir, paths.main_path);
}

bool journal_load(JournalDirectory &dir, const JournalPaths &paths, uint8_t *dst, uint32_t n) {
  if (!dst || n != kStoreBytes) return false;
  if (dir.file_size(paths.main_path) == n && dir.read_at(paths.main_path, 0, dst, n) && journal_image_valid(dst, n))
    return true;
  if (dir.file_size(paths.bak_path) == n && dir.read_at(paths.bak_path, 0, dst, n) && journal_image_valid(dst, n))
    return true;
  memset(dst, 0, n);
  return !dir.exists(paths.main_path) && !dir.exists(paths.bak_path);
}

bool journal_commit(JournalDirectory &dir, const JournalPaths &paths, const uint8_t *image, uint32_t n) {
  if (!journal_write_tmp(dir, paths, image, n)) return false;
  if (!journal_move_main_to_bak(dir, paths)) return false;
  if (!journal_rename_tmp_to_main(dir, paths) || !journal_verify_path(dir, paths.main_path, image, n)) {
    journal_restore_main_from_bak(dir, paths);
    return false;
  }
  return true;
}

}  // namespace loadac
