#pragma once

#include <stdint.h>

namespace loadac {

// Three-file journal commit shared by the SPIFFS adapter and the host tests.
// The host tests call these steps with an in-memory directory. They do not
// power-cycle SPIFFS and they are not a hardware power-cut result.
//
// Order: write and verify tmp, move main to bak, rename tmp to main, verify
// main. A failed verify restores bak. A stop between steps leaves the files
// for journal_load, which accepts main only when the whole image validates
// and otherwise uses bak.

class JournalDirectory {
 public:
  virtual ~JournalDirectory() {}
  virtual bool exists(const char *path) const = 0;
  virtual uint32_t file_size(const char *path) const = 0;
  virtual bool remove_file(const char *path) = 0;
  virtual bool rename_file(const char *from, const char *to) = 0;
  virtual bool write_all(const char *path, const uint8_t *src, uint32_t n) = 0;
  virtual bool read_at(const char *path, uint32_t offset, uint8_t *dst, uint32_t n) const = 0;
};

struct JournalPaths {
  const char *main_path;
  const char *bak_path;
  const char *tmp_path;
};

bool journal_write_tmp(JournalDirectory &dir, const JournalPaths &paths, const uint8_t *image, uint32_t n);
bool journal_move_main_to_bak(JournalDirectory &dir, const JournalPaths &paths);
bool journal_rename_tmp_to_main(JournalDirectory &dir, const JournalPaths &paths);
bool journal_verify_path(const JournalDirectory &dir, const char *path, const uint8_t *image, uint32_t n);
bool journal_restore_main_from_bak(JournalDirectory &dir, const JournalPaths &paths);
bool journal_load(JournalDirectory &dir, const JournalPaths &paths, uint8_t *dst, uint32_t n);
bool journal_commit(JournalDirectory &dir, const JournalPaths &paths, const uint8_t *image, uint32_t n);

}  // namespace loadac
