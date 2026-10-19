#include "update/json_lite.h"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace fr::json {

namespace {

class Parser {
 public:
  Parser(std::string_view s, std::string* err) : s_(s), err_(err) {}

  bool run(Value* out) {
    skip_ws();
    if (!value(out, 0)) return false;
    skip_ws();
    if (i_ != s_.size()) return fail("trailing characters");
    return true;
  }

 private:
  bool fail(const char* msg) {
    if (err_) *err_ = "offset " + std::to_string(i_) + ": " + msg;
    return false;
  }

  void skip_ws() {
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        ++i_;
      else
        break;
    }
  }

  bool eat(char c) {
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }

  bool literal(std::string_view lit, Value* out) {
    if (s_.substr(i_, lit.size()) != lit) return fail("invalid literal");
    i_ += lit.size();
    if (lit == "null") {
      out->type = Type::Null;
    } else {
      out->type = Type::Bool;
      out->boolean = (lit == "true");
    }
    return true;
  }

  static void append_utf8(std::string* out, uint32_t cp) {
    if (cp <= 0x7F) {
      out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t* out) {
    if (i_ + 4 > s_.size()) return fail("short \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_ + k];
      uint32_t d;
      if (c >= '0' && c <= '9')
        d = static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        d = static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        d = static_cast<uint32_t>(c - 'A' + 10);
      else
        return fail("invalid \\u escape");
      v = (v << 4) | d;
    }
    i_ += 4;
    *out = v;
    return true;
  }

  bool str(std::string* out) {
    if (!eat('"')) return fail("expected string");
    out->clear();
    while (i_ < s_.size()) {
      const unsigned char c = static_cast<unsigned char>(s_[i_++]);
      if (c == '"') return true;
      if (c < 0x20) return fail("control character in string");
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        continue;
      }
      if (i_ >= s_.size()) return fail("unterminated escape");
      const char e = s_[i_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i_ + 1 >= s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u')
              return fail("missing low surrogate");
            i_ += 2;
            uint32_t lo = 0;
            if (!hex4(&lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("lone low surrogate");
          }
          append_utf8(out, cp);
          break;
        }
        default: return fail("invalid escape");
      }
    }
    return fail("unterminated string");
  }

  bool number(Value* out) {
    const size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size()) return fail("bad number");
    if (s_[i_] == '0') {
      ++i_;
    } else if (s_[i_] >= '1' && s_[i_] <= '9') {
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    } else {
      return fail("bad number");
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad fraction");
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad exponent");
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    double v = 0.0;
    const auto [p, ec] = std::from_chars(s_.data() + start, s_.data() + i_, v);
    if (ec != std::errc() || p != s_.data() + i_ || !std::isfinite(v)) return fail("bad number");
    out->type = Type::Number;
    out->number = v;
    return true;
  }

  bool object(Value* out, int depth) {
    if (depth >= kMaxDepth) return fail("nesting too deep");
    if (!eat('{')) return fail("expected object");
    out->type = Type::Object;
    out->object.clear();
    skip_ws();
    if (eat('}')) return true;
    for (;;) {
      skip_ws();
      std::string key;
      if (!str(&key)) return false;
      skip_ws();
      if (!eat(':')) return fail("expected ':'");
      Value v;
      if (!value(&v, depth + 1)) return false;
      bool replaced = false;
      for (auto& kv : out->object) {
        if (kv.first == key) {
          kv.second = std::move(v);
          replaced = true;
          break;
        }
      }
      if (!replaced) out->object.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (eat(',')) continue;
      if (eat('}')) return true;
      return fail("expected ',' or '}'");
    }
  }

  bool array(Value* out, int depth) {
    if (depth >= kMaxDepth) return fail("nesting too deep");
    if (!eat('[')) return fail("expected array");
    out->type = Type::Array;
    out->array.clear();
    skip_ws();
    if (eat(']')) return true;
    for (;;) {
      Value v;
      if (!value(&v, depth + 1)) return false;
      out->array.push_back(std::move(v));
      skip_ws();
      if (eat(',')) continue;
      if (eat(']')) return true;
      return fail("expected ',' or ']'");
    }
  }

  bool value(Value* out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    skip_ws();
    if (i_ >= s_.size()) return fail("unexpected end of input");
    const char c = s_[i_];
    if (c == '{') return object(out, depth);
    if (c == '[') return array(out, depth);
    if (c == '"') {
      out->type = Type::String;
      return str(&out->string);
    }
    if (c == 't') return literal("true", out);
    if (c == 'f') return literal("false", out);
    if (c == 'n') return literal("null", out);
    if (c == '-' || (c >= '0' && c <= '9')) return number(out);
    return fail("unexpected character");
  }

  std::string_view s_;
  std::string* err_;
  size_t i_ = 0;
};

}  // namespace

const Value* Value::find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& kv : object)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string Value::get_string(std::string_view key, const std::string& dft) const {
  const Value* v = find(key);
  return (v && v->type == Type::String) ? v->string : dft;
}

double Value::get_number(std::string_view key, double dft) const {
  const Value* v = find(key);
  return (v && v->type == Type::Number) ? v->number : dft;
}

bool Value::get_bool(std::string_view key, bool dft) const {
  const Value* v = find(key);
  return (v && v->type == Type::Bool) ? v->boolean : dft;
}

bool parse(std::string_view text, Value* out, std::string* err, size_t max_bytes) {
  if (!out) return false;
  *out = Value{};
  if (err) err->clear();
  if (text.size() > max_bytes) {
    if (err) *err = "response too large";
    return false;
  }
  Parser p(text, err);
  return p.run(out);
}

}  // namespace fr::json
