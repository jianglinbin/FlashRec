#pragma once
// 更新报文专用最小 JSON 解析器（AUTO_UPDATE_PLAN §3）。
// 只服务 GitHub API / latest.json 这两个外部输入：严格语法、深度与体积上限、
// 错误带字节偏移；不依赖第三方库，不参与应用配置解析（Config 用的是 MiniJson）。
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fr::json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  bool is_null() const { return type == Type::Null; }
  bool is_bool() const { return type == Type::Bool; }
  bool is_number() const { return type == Type::Number; }
  bool is_string() const { return type == Type::String; }
  bool is_array() const { return type == Type::Array; }
  bool is_object() const { return type == Type::Object; }

  // 对象字段查找（线性；更新报文很小）。非对象/不存在返回 nullptr。
  const Value* find(std::string_view key) const;

  std::string get_string(std::string_view key, const std::string& dft = {}) const;
  double get_number(std::string_view key, double dft = 0.0) const;
  bool get_bool(std::string_view key, bool dft = false) const;
};

constexpr size_t kDefaultMaxBytes = 4u * 1024u * 1024u;  // GitHub API 响应上限
constexpr int kMaxDepth = 32;

// 解析失败返回 false，err 形如 "offset 123: expected ':'"。
bool parse(std::string_view text, Value* out, std::string* err,
           size_t max_bytes = kDefaultMaxBytes);

}  // namespace fr::json
