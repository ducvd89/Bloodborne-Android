// SPDX-License-Identifier: GPL-3.0-or-later
// A streaming SHA-256 (the PKG check hashes tens of GB): portable code, and the x86 SHA extensions when the processor has them (about 1.5 GB/s per
// core instead of 0.3).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t n);
  void final(uint8_t out[32]);
  static std::string hex(const uint8_t d[32]);
  static bool hardware();  // the SHA extensions are in use
  static void forcePortable(bool on);  // tests

 private:
  void block(const uint8_t* p, size_t blocks);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};
