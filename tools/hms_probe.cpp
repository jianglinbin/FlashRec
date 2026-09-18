// 探针：链接真实 src/app/str_util.cpp，实测 parse_hms_seconds
#include <cstdio>
#include <string>

namespace fr {
double parse_hms_seconds(const std::string& s);  // 真实实现（str_util.cpp）
}

int main() {
  const char* cases[] = {"0:02:58", "0:01:27", "1:02:58", "178", "0:02"};
  for (auto* c : cases)
    printf("%-8s -> %g\n", c, fr::parse_hms_seconds(c));
  return 0;
}
