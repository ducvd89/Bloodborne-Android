// SPDX-License-Identifier: GPL-3.0-or-later
// The damage check of a PS4 PKG before it is unpacked: a port of the app's PkgVerifier.java (same fields, same rules, so the tool and the app
// agree), with checks added that need no hashing (the entry table's bounds). The digests are SHA-256 over parts of the PKG that the PKG carries
// itself (header, body, digest table, PFS image), so a cut-short or corrupted download or copy is found before anything is extracted.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PkgReport {
  std::vector<std::string> damaged, matched;
  bool skippedImage = false;  // the PFS image was not read because the header or body is already damaged
  uint64_t bytesRead = 0;
  bool ok() const { return damaged.empty(); }
  std::string describe() const;  // "- part\n- part"
};

class PkgVerifier {
 public:
  explicit PkgVerifier(const std::string& path);
  // false: the file cannot be read (err). A file that is not a PKG gives an empty plan (readPkgHeader reports that).
  bool open(std::string& err);
  uint64_t plannedBytes() const { return total_; }
  // Runs the check. progress(done bytes of plannedBytes()) is called often; cancel() true stops (returns false with err "cancelled"); false also on read errors.
  bool run(PkgReport& report, std::string& err, const std::function<void(uint64_t)>& progress, const std::function<bool()>& cancel);

 private:
  std::string path_;
  uint64_t fileSize_ = 0, total_ = 0;
  unsigned char head_[0x1000];
  bool haveHead_ = false;
};
