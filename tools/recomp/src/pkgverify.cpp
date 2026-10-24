// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkgverify.h"

#include "sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
constexpr uint32_t HEADER = 0x1000, HEADER_DIGESTED = 0xFE0;
constexpr uint64_t MAX_BODY = 1ull << 30, CHUNK = 8u << 20;

uint64_t be32(const unsigned char* d) { return ((uint64_t)d[0] << 24) | ((uint64_t)d[1] << 16) | ((uint64_t)d[2] << 8) | d[3]; }
uint64_t be64(const unsigned char* d) { return (be32(d) << 32) | be32(d + 4); }
bool zero(const unsigned char* d) { for (int i = 0; i < 32; ++i) if (d[i]) return false; return true; }
std::string bytes(uint64_t n) {
  char b[48];
  if (n >= (1ull << 30)) snprintf(b, sizeof b, "%.2f GB (%llu bytes)", n / 1073741824.0, (unsigned long long)n);
  else if (n >= (1u << 20)) snprintf(b, sizeof b, "%.1f MB (%llu bytes)", n / 1048576.0, (unsigned long long)n);
  else snprintf(b, sizeof b, "%llu bytes", (unsigned long long)n);
  return b;
}
std::string cap(std::string s) { if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]); return s; }
}  // namespace

std::string PkgReport::describe() const {
  std::string s;
  for (auto& d : damaged) { if (!s.empty()) s += "\n"; s += "- " + d; }
  return s;
}

PkgVerifier::PkgVerifier(const std::string& path) : path_(path) {}

bool PkgVerifier::open(std::string& err) {
  std::error_code ec;
  fileSize_ = std::filesystem::file_size(std::filesystem::path(std::u8string(path_.begin(), path_.end())), ec);
  if (ec) { err = "cannot read " + path_ + ": " + ec.message(); return false; }
  std::ifstream f(std::filesystem::path(std::u8string(path_.begin(), path_.end())), std::ios::binary);
  if (!f) { err = "cannot read " + path_; return false; }
  total_ = 0; haveHead_ = false;
  if (fileSize_ < HEADER) return true;
  f.read((char*)head_, HEADER);
  if ((uint64_t)f.gcount() < HEADER) { err = "cannot read " + path_; return false; }
  if (head_[0] != 0x7f || head_[1] != 'C' || head_[2] != 'N' || head_[3] != 'T') return true;
  haveHead_ = true;
  // The plan: the digests present and the bytes they cover (as PkgVerifier.java).
  auto inside = [&](uint64_t off, uint64_t size) { return size > 0 && off <= fileSize_ && size <= fileSize_ - off; };
  const uint64_t bodyOffset = be64(head_ + 0x20), bodySize = be64(head_ + 0x28), pfsOffset = be64(head_ + 0x410), pfsSize = be64(head_ + 0x418), signedLength = be32(head_ + 0x438);
  const bool doBody = !zero(head_ + 0x160) && inside(bodyOffset, bodySize) && bodySize <= MAX_BODY;
  uint64_t tableSize = 0;
  if (!zero(head_ + 0x140)) {
    uint64_t count = be32(head_ + 0x10), table = be32(head_ + 0x18);
    if (count > 0 && count <= 4096 && inside(table, count * 32)) {
      std::vector<unsigned char> e(count * 32);
      f.clear(); f.seekg((std::streamoff)table); f.read((char*)e.data(), (std::streamsize)e.size());
      for (uint64_t i = 0; i < count && (uint64_t)f.gcount() == e.size(); ++i)
        if (be32(&e[i * 32]) == 1) { uint64_t off = be32(&e[i * 32 + 16]), size = be32(&e[i * 32 + 20]); if (size <= (1u << 20) && inside(off, size)) tableSize = size; break; }
    }
  }
  const bool imageKnown = inside(pfsOffset, pfsSize), doImage = imageKnown && !zero(head_ + 0x440), doSigned = imageKnown && !zero(head_ + 0x460) && signedLength > 0 && signedLength <= pfsSize;
  const uint64_t imageBytes = doImage ? pfsSize : doSigned ? signedLength : 0;
  total_ = HEADER_DIGESTED + (doBody ? bodySize : 0) + tableSize + imageBytes;
  return true;
}

bool PkgVerifier::run(PkgReport& report, std::string& err, const std::function<void(uint64_t)>& progress, const std::function<bool()>& cancel) {
  if (!haveHead_) return true;
  std::ifstream f(std::filesystem::path(std::u8string(path_.begin(), path_.end())), std::ios::binary);
  if (!f) { err = "cannot read " + path_; return false; }
  auto inside = [&](uint64_t off, uint64_t size) { return size > 0 && off <= fileSize_ && size <= fileSize_ - off; };
  const unsigned char* head = head_;
  const uint64_t bodyOffset = be64(head + 0x20), bodySize = be64(head + 0x28), pfsOffset = be64(head + 0x410), pfsSize = be64(head + 0x418), signedLength = be32(head + 0x438);
  uint64_t done = 0;
  std::vector<char> buf(CHUNK);
  auto fail = [&](const char* what) { err = std::string(what); return false; };
  // Reads [offset, offset+size) in chunks into `main` (and the first signedLen bytes into `signedH`).
  auto hash = [&](uint64_t offset, uint64_t size, Sha256* mainH, Sha256* signedH, uint64_t signedLen) -> bool {
    f.clear(); f.seekg((std::streamoff)offset);
    uint64_t at = 0;
    while (at < size) {
      if (cancel && cancel()) { err = "cancelled"; return false; }
      size_t n = (size_t)std::min<uint64_t>(CHUNK, size - at);
      f.read(buf.data(), (std::streamsize)n);
      if ((size_t)f.gcount() != n) { err = "The PKG ended early (the file changed while it was read)"; return false; }
      if (signedH && at < signedLen) signedH->update(buf.data(), (size_t)std::min<uint64_t>(n, signedLen - at));
      if (mainH) mainH->update(buf.data(), n);
      at += n; done += n; report.bytesRead += n;
      if (progress) progress(done);
    }
    return true;
  };
  auto note = [&](bool same, const char* part, const std::string& problem) { if (same) report.matched.push_back(part); else report.damaged.push_back(cap(problem)); };

  // What the file's size says (a cut-short download is the usual damage).
  const uint64_t packageSize = be64(head + 0x430);
  if (packageSize > fileSize_)
    report.damaged.push_back("The file is shorter than the PKG says it should be: expected " + bytes(packageSize) + ", it has " + bytes(fileSize_) + " (the download or copy was cut short)");
  // The entry table's bounds (no hashing).
  {
    uint64_t count = be32(head + 0x10), table = be32(head + 0x18);
    if (count > 0 && count <= 4096) {
      if (!inside(table, count * 32)) report.damaged.push_back("The PKG's entry table lies outside the file (offset " + std::to_string(table) + ", " + std::to_string(count) + " entries)");
      else {
        std::vector<unsigned char> e(count * 32);
        f.clear(); f.seekg((std::streamoff)table); f.read((char*)e.data(), (std::streamsize)e.size());
        unsigned bad = 0; uint64_t firstId = 0;
        for (uint64_t i = 0; i < count && (uint64_t)f.gcount() == e.size(); ++i) {
          uint64_t off = be32(&e[i * 32 + 16]), size = be32(&e[i * 32 + 20]);
          if (size > 0 && !(off <= fileSize_ && size <= fileSize_ - off)) { if (!bad) firstId = be32(&e[i * 32]); ++bad; }
        }
        if (bad) {
          char b[160]; snprintf(b, sizeof b, "%u entries of the PKG's file table lie outside the file (the first is entry 0x%llx): the PKG is incomplete or damaged", bad, (unsigned long long)firstId);
          report.damaged.push_back(b);
        }
      }
    }
  }
  done = 0;
  if (progress) progress(0);
  // The header, the body and the digest table first (a few MB), so a damaged one stops before the long read.
  if (!zero(head + 0xFE0)) {
    Sha256 s; uint8_t d[32]; s.update(head, HEADER_DIGESTED); s.final(d);
    done += HEADER_DIGESTED;
    note(!memcmp(d, head + 0xFE0, 32), "header", "the PKG header doesn't match its checksum (header digest mismatch)");
  }
  const bool doBody = !zero(head + 0x160) && inside(bodyOffset, bodySize) && bodySize <= MAX_BODY;
  if (doBody) {
    Sha256 s; uint8_t d[32];
    if (!hash(bodyOffset, bodySize, &s, nullptr, 0)) return false;
    s.final(d);
    note(!memcmp(d, head + 0x160, 32), "body", "the PKG's file table and its small files (the body) don't match their checksum (body digest mismatch)");
  }
  if (!zero(head + 0x140)) {
    uint64_t count = be32(head + 0x10), table = be32(head + 0x18), off = 0, size = 0;
    if (count > 0 && count <= 4096 && inside(table, count * 32)) {
      std::vector<unsigned char> e(count * 32);
      f.clear(); f.seekg((std::streamoff)table); f.read((char*)e.data(), (std::streamsize)e.size());
      for (uint64_t i = 0; i < count && (uint64_t)f.gcount() == e.size(); ++i)
        if (be32(&e[i * 32]) == 1) { off = be32(&e[i * 32 + 16]); size = be32(&e[i * 32 + 20]); if (!(size <= (1u << 20) && inside(off, size))) size = 0; break; }
    }
    if (size > 0) {
      Sha256 s; uint8_t d[32];
      if (!hash(off, size, &s, nullptr, 0)) return false;
      s.final(d);
      note(!memcmp(d, head + 0x140, 32), "digest table", "the PKG's list of checksums (digest table) doesn't match its own checksum (digest table mismatch)");
    }
  }
  const bool imageKnown = inside(pfsOffset, pfsSize), doImage = imageKnown && !zero(head + 0x440), doSigned = imageKnown && !zero(head + 0x460) && signedLength > 0 && signedLength <= pfsSize;
  const uint64_t imageBytes = doImage ? pfsSize : doSigned ? signedLength : 0;
  if (imageBytes > 0) {
    if (!report.ok()) { report.skippedImage = true; return true; }
    Sha256 full, sig; uint8_t d[32];
    if (!hash(pfsOffset, imageBytes, doImage ? &full : nullptr, doSigned ? &sig : nullptr, doSigned ? signedLength : 0)) return false;
    if (doImage) { full.final(d); note(!memcmp(d, head + 0x440, 32), "image", "the game data (PFS image) doesn't match its checksum (image digest mismatch)"); }
    if (doSigned) { sig.final(d); note(!memcmp(d, head + 0x460, 32), "image start", "the start of the game data (PFS image) doesn't match its checksum (image start digest mismatch)"); }
  }
  if (progress) progress(total_);
  (void)fail;
  return true;
}
