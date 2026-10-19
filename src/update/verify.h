#pragma once
// SHA-256（FIPS 180-4）：清单资产校验用。自包含实现，不引平台加密库。
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fr::update {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, std::size_t size);
  std::string finish_hex();

 private:
  void transform(const unsigned char* block);

  std::uint32_t state_[8];
  std::uint64_t total_bytes_;
  unsigned char buffer_[64];
  std::size_t buffer_size_;
  bool finished_;
};

std::string sha256_hex(std::string_view data);

// 流式读取文件计算十六进制摘要；失败返回 false 并写 error。
bool sha256_file(const std::string& path, std::string* hex, std::string* error);

}  // namespace fr::update
