#include "update/verify.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace fr::update {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

inline std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

inline std::uint32_t big_endian32(const unsigned char* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline void store_big_endian32(unsigned char* p, std::uint32_t v) {
  p[0] = static_cast<unsigned char>(v >> 24);
  p[1] = static_cast<unsigned char>(v >> 16);
  p[2] = static_cast<unsigned char>(v >> 8);
  p[3] = static_cast<unsigned char>(v);
}

std::string to_hex(const unsigned char* data, std::size_t size) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out[i * 2] = kHex[data[i] >> 4];
    out[i * 2 + 1] = kHex[data[i] & 0x0f];
  }
  return out;
}

}  // namespace

void Sha256::reset() {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  total_bytes_ = 0;
  buffer_size_ = 0;
  finished_ = false;
}

void Sha256::transform(const unsigned char* block) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = big_endian32(block + i * 4);
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
  if (finished_ || size == 0) return;
  const auto* bytes = static_cast<const unsigned char*>(data);
  total_bytes_ += size;
  while (size > 0) {
    const std::size_t take = std::min(size, sizeof(buffer_) - buffer_size_);
    std::memcpy(buffer_ + buffer_size_, bytes, take);
    buffer_size_ += take;
    bytes += take;
    size -= take;
    if (buffer_size_ == sizeof(buffer_)) {
      transform(buffer_);
      buffer_size_ = 0;
    }
  }
}

std::string Sha256::finish_hex() {
  if (finished_) return std::string();
  const std::uint64_t bit_length = total_bytes_ * 8;
  unsigned char padding[72] = {0x80};
  const std::size_t pad_size =
      buffer_size_ < 56 ? 56 - buffer_size_ : 120 - buffer_size_;
  update(padding, pad_size);
  unsigned char length_be[8];
  for (int i = 0; i < 8; ++i)
    length_be[i] = static_cast<unsigned char>(bit_length >> (56 - i * 8));
  update(length_be, sizeof(length_be));
  finished_ = true;

  unsigned char digest[32];
  for (int i = 0; i < 8; ++i) store_big_endian32(digest + i * 4, state_[i]);
  return to_hex(digest, sizeof(digest));
}

std::string sha256_hex(std::string_view data) {
  Sha256 hasher;
  hasher.update(data.data(), data.size());
  return hasher.finish_hex();
}

bool sha256_file(const std::string& path, std::string* hex, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error) *error = "无法打开文件: " + path;
    return false;
  }
  Sha256 hasher;
  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = file.gcount();
    if (got > 0) hasher.update(buffer.data(), static_cast<std::size_t>(got));
  }
  if (!file.eof()) {
    if (error) *error = "读取文件失败: " + path;
    return false;
  }
  if (hex) *hex = hasher.finish_hex();
  return true;
}

}  // namespace fr::update
