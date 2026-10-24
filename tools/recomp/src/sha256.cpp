// SPDX-License-Identifier: GPL-3.0-or-later
#include "sha256.h"

#include <algorithm>
#include <cstring>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {
const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void portable(uint32_t st[8], const uint8_t* p, size_t blocks) {
  while (blocks--) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    p += 64;
  }
}

#if defined(__x86_64__)
__attribute__((target("sha,ssse3,sse4.1"))) void hardwareBlocks(uint32_t state[8], const uint8_t* data, size_t blocks) {
  const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i tmp = _mm_loadu_si128((const __m128i*)&state[0]);
  __m128i s1 = _mm_loadu_si128((const __m128i*)&state[4]);
  tmp = _mm_shuffle_epi32(tmp, 0xB1);
  s1 = _mm_shuffle_epi32(s1, 0x1B);
  __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);
  s1 = _mm_blend_epi16(s1, tmp, 0xF0);
  while (blocks--) {
    const __m128i save0 = s0, save1 = s1;
    __m128i m[4];
    for (int i = 0; i < 16; ++i) {
      if (i < 4) m[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 16 * i)), mask);
      else {
        __m128i x = _mm_sha256msg1_epu32(m[i & 3], m[(i + 1) & 3]);
        x = _mm_add_epi32(x, _mm_alignr_epi8(m[(i + 3) & 3], m[(i + 2) & 3], 4));
        m[i & 3] = _mm_sha256msg2_epu32(x, m[(i + 3) & 3]);
      }
      __m128i msg = _mm_add_epi32(m[i & 3], _mm_loadu_si128((const __m128i*)(K + 4 * i)));
      s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
      msg = _mm_shuffle_epi32(msg, 0x0E);
      s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
    }
    s0 = _mm_add_epi32(s0, save0);
    s1 = _mm_add_epi32(s1, save1);
    data += 64;
  }
  tmp = _mm_shuffle_epi32(s0, 0x1B);
  s1 = _mm_shuffle_epi32(s1, 0xB1);
  s0 = _mm_blend_epi16(tmp, s1, 0xF0);
  s1 = _mm_alignr_epi8(s1, tmp, 8);
  _mm_storeu_si128((__m128i*)&state[0], s0);
  _mm_storeu_si128((__m128i*)&state[4], s1);
}
#endif

bool g_forcePortable = false;
bool hasHardware() {
#if defined(__x86_64__)
  static const bool yes = __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("ssse3");
  return yes && !g_forcePortable;
#else
  return false;
#endif
}
}  // namespace

bool Sha256::hardware() { return hasHardware(); }
void Sha256::forcePortable(bool on) { g_forcePortable = on; }

void Sha256::reset() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(h_, init, sizeof h_); used_ = 0; total_ = 0;
}

void Sha256::block(const uint8_t* p, size_t blocks) {
#if defined(__x86_64__)
  if (hasHardware()) { hardwareBlocks(h_, p, blocks); return; }
#endif
  portable(h_, p, blocks);
}

void Sha256::update(const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  total_ += n;
  if (used_) {
    size_t c = std::min(n, 64 - used_);
    memcpy(buf_ + used_, p, c); used_ += c; p += c; n -= c;
    if (used_ == 64) { block(buf_, 1); used_ = 0; }
  }
  if (n >= 64) { size_t b = n / 64; block(p, b); p += b * 64; n -= b * 64; }
  if (n) { memcpy(buf_, p, n); used_ = n; }
}

void Sha256::final(uint8_t out[32]) {
  uint64_t bits = total_ * 8;
  uint8_t pad[72] = {0x80};
  size_t padLen = (used_ < 56 ? 56 : 120) - used_;
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  update(pad, padLen);
  update(len, 8);
  for (int i = 0; i < 8; ++i) { out[4 * i] = (uint8_t)(h_[i] >> 24); out[4 * i + 1] = (uint8_t)(h_[i] >> 16); out[4 * i + 2] = (uint8_t)(h_[i] >> 8); out[4 * i + 3] = (uint8_t)h_[i]; }
}

std::string Sha256::hex(const uint8_t d[32]) {
  static const char* x = "0123456789abcdef"; std::string s;
  for (int i = 0; i < 32; ++i) { s += x[d[i] >> 4]; s += x[d[i] & 15]; }
  return s;
}
