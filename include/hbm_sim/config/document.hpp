#pragma once

// 分节配置文档只负责“读取、选择和分层”，不直接修改 DramSpec/Controller。
// CLI 仍保留逐 key 映射，因此每个公开配置项最终影响哪个模型字段依然可审计。

#include <cstddef>
#include <string>
#include <vector>

namespace hbm_sim::config {

enum class ValidationMode {
  // 只拒绝无法执行或内部自相矛盾的配置；偏离标准 preset 会记录为差异。
  Exploratory,
  // 除执行检查外，把偏离所选标准基线的配置视为不合规。
  Standard,
  // 在 Standard 基础上要求器件相关 timing 已有 vendor 来源。
  Device,
};

const char* to_string(ValidationMode mode);
ValidationMode parse_validation_mode(const std::string& value);

struct ConfigEntry {
  std::string key;
  std::string value;
  std::string section;
  std::string path;
  std::size_t line = 0;
  int layer = 0;
};

struct ConfigDocument {
  std::string path;
  bool sectioned = false;
  // [meta] extends 指向的基础配置。路径相对当前配置文件解析；解析器会先
  // 加载基础配置，再加载当前文档，从而让用例/验证配置保持短小且可直接运行。
  std::vector<std::string> extends;
  std::vector<ConfigEntry> entries;
};

struct Selection {
  std::string standard = "hbm4";
  std::string preset;
};

// v1 平面配置和 v2 分节配置使用同一读取入口。v2 同一 section 内不允许
// 重复 key；不同 layer 重复 key 表示有意覆盖。
ConfigDocument load_document(const std::string& path);

// 递归展开 [meta] extends。返回顺序始终是“最底层基础配置 -> 当前配置”，
// 并拒绝循环继承。一个 cfg 因而可以作为独立入口，而 resolved config 仍能
// 准确记录每个值来自哪一层、哪个文件和哪一行。
std::vector<ConfigDocument> load_document_tree(const std::string& path);

// 从所有配置的 [model]（以及旧平面配置）提取选择项；显式 CLI 选择最终覆盖。
Selection discover_selection(const std::vector<ConfigDocument>& documents,
                             const std::string& cli_standard,
                             const std::string& cli_preset);

// 返回当前 standard/preset 对应的 active entry，并按固定 layer 排序：
// common(10) < family(20) < standard(30) < preset(40) < override(50)。
// 旧平面配置保持原始行顺序，避免改变 timing_override_source 的历史语义。
std::vector<ConfigEntry> resolve_document(const ConfigDocument& document,
                                          const Selection& selection);

// 多个 --config 也必须服从同一套 layer，而不是简单按文件逐个覆盖。
// 同一 layer 内仍保持命令行给出的文档顺序和各文件原始行顺序。
std::vector<ConfigEntry> resolve_documents(const std::vector<ConfigDocument>& documents,
                                           const Selection& selection);

std::vector<std::string> list_presets(const ConfigDocument& document,
                                      const std::string& standard);

std::string normalize_name(std::string value);
std::string canonical_standard(std::string value);

}  // namespace hbm_sim::config
