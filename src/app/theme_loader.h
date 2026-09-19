#pragma once
// d180 皮肤引擎 M1：JSON 驱动皮肤加载（解析 / 校验 / 深合并 / 回退）。
//
// 设计要点（见 SKIN_ENGINE_PLAN.md §11）：
//   - "默认皮肤是不变量"：base 为 Theme::make_default()；JSON 只做**覆盖**，不参与从零构造。
//   - 字段级容错：坏值/类型错/越界 → 保留默认 + 记 warn，绝不整份崩溃。
//   - 结构性错误（JSON 语法 / 根非对象 / skins 缺失）→ 返回 false + err。
//   - 零第三方依赖：自带极简 JSON 解析器（config.cpp 的 MiniJson 只支持扁平路径，不够用）。
#include <string>
#include <vector>

#include "ui/theme.h"

namespace fr {

struct SkinMeta {
  std::string id, name, personality, risk;
};
struct NamedTheme {
  SkinMeta meta;
  Theme theme;
};

// 解析 bundle（assets/skins.json：meta/layout/skins[]/stagePlaceholder）。
// 成功返回 true 并把各皮肤写入 out（顺序同文件）；warns 收集字段级回退信息。
bool load_skin_bundle(const std::string& path, const Theme& base,
                      std::vector<NamedTheme>* out, std::vector<std::string>* warns,
                      std::string* err);

// 解析用户单文件皮肤（%APPDATA%/FlashRec/skins/*.json）。base 应已含全局 layout。
bool load_skin_file(const std::string& path, const Theme& base, NamedTheme* out,
                    std::vector<std::string>* warns, std::string* err);

// d183：合并内置 bundle + 用户目录（*.json，同名 id 用户覆盖内置）。
// 单个用户皮肤坏掉只 warn 跳过，不影响内置；bundle 坏则整体失败。
bool load_all_skins(const std::string& bundle_path, const std::string& user_dir,
                    const Theme& base, std::vector<NamedTheme>* out,
                    std::vector<std::string>* warns, std::string* err);

}  // namespace fr
