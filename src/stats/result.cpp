#include "hbm_sim/stats/result.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace hbm_sim {
namespace {
std::string trim(const std::string& s) {
  auto a = s.find_first_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string quote(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<unsigned>(c);
    else out << c;
  }
  out << '"';
  return out.str();
}
std::string json(const ResultValue& v) {
  if (const auto* s = std::get_if<std::string>(&v)) return quote(*s);
  if (const auto* d = std::get_if<double>(&v); d && !std::isfinite(*d))
    throw std::runtime_error("non-finite result");
  return result_value_text(v);
}
void object(std::ostream& out, const ResultFields& fields) {
  out << '{';
  bool first = true;
  for (const auto& [k, v] : fields) {
    out << (first ? "" : ",") << "\n    " << quote(k) << ": " << json(v);
    first = false;
  }
  out << "\n  }";
}
std::string value(const ResultFields& f, const std::string& k) {
  auto it = f.find(k);
  if (it == f.end() || std::holds_alternative<std::nullptr_t>(it->second)) return "N/A";
  if (const auto* d = std::get_if<double>(&it->second)) {
    std::ostringstream s;
    s << std::setprecision(6) << *d;
    return s.str();
  }
  return result_value_text(it->second);
}
double number(const ResultFields& f, const std::string& k) {
  auto it = f.find(k);
  if (it == f.end()) throw std::runtime_error("missing result field: " + k);
  return std::visit([](const auto& v) -> double {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_arithmetic_v<T>) return static_cast<double>(v);
    else throw std::runtime_error("non-numeric result field");
  }, it->second);
}
bool enabled(const ResultFields& f, const std::string& k) {
  return value(f, k) == "true";
}
void select(ResultFields& target, const ResultFields& source, const char* keys) {
  std::istringstream in(keys);
  std::string key;
  while (in >> key) if (auto it = source.find(key); it != source.end())
    target.emplace(*it);
}
}  // namespace

std::string result_value_text(const ResultValue& v) {
  return std::visit([](const auto& item) -> std::string {
    using T = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<T, std::nullptr_t>) return "null";
    else if constexpr (std::is_same_v<T, std::string>) return item;
    else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
    else {
      std::ostringstream out;
      out << std::setprecision(std::numeric_limits<double>::max_digits10) << item;
      return out.str();
    }
  }, v);
}

ResultReport make_result_report(const ResultFields& f) {
  ResultReport r;
  select(r.model, f, "model_name standard model_conformance stack_count channels "
      "pseudo_channels sids ranks bank_groups banks_per_group rows columns "
      "stack_height density_gb capacity_per_instance_bytes aggregate_capacity_bytes "
      "line_size dram_transaction_bytes scheduler row_policy address_mapping "
      "mem_phy_mode memory_backend supports_refresh supports_rfm ecc_shadow "
      "power_model_enabled thermal_model_enabled lpddr_family memory_system channel_mapper stack_mapping");
  select(r.parameters, f, "data_rate_mbps data_bus_bits tCK_ps tick_duration_ps "
      "tick_multiplier lpddr_wck_ratio pattern requests read_ratio inject_interval "
      "seed random_address_space_bytes effective_random_address_space_bytes "
      "addr_stride read_buffer_size write_buffer_size input_kind");
  select(r.metrics, f, "host_requests dram_transactions completed_reads completed_writes "
      "remaining_requests remaining_pending hit_cycle_limit system_cycles "
      "read_bytes write_bytes achieved_bw_GBps peak_bandwidth_GBps bandwidth_util_pct "
      "storage_lines_allocated storage_bytes_allocated power_energy_pJ "
      "thermal_avg_temp_C thermal_peak_temp_C");
  select(r.validation, f, "cmd_validation dfi_validation cmd_validation_checked "
      "dfi_validation_events data_checked_reads data_mismatches "
      "ecc_uncorrectable_errors golden_verified golden_mismatches");
  for (const char* key : {"cmd_validation", "dfi_validation"})
    if (value(r.validation, key) == "off") r.validation[key] = std::string("not_run");
  if (f.contains("system_cycles")) {
    const double ns = number(f, "tick_duration_ps") / 1000;
    r.metrics["simulation_time_ns"] = number(f, "system_cycles") * ns;
    r.metrics["avg_read_latency_ns"] = number(f, "completed_reads") == 0
        ? ResultValue(nullptr) : ResultValue(number(f, "avg_read_latency") * ns);
    const double classified = number(f, "row_hits") + number(f, "row_misses") +
                              number(f, "row_conflicts");
    r.metrics["row_hit_pct"] = classified == 0 ? ResultValue(nullptr)
        : ResultValue(100 * number(f, "row_hits") / classified);
  }
  if (!enabled(f, "power_model_enabled")) r.metrics.erase("power_energy_pJ");
  if (!enabled(f, "thermal_model_enabled")) {
    r.metrics.erase("thermal_avg_temp_C");
    r.metrics.erase("thermal_peak_temp_C");
  }
  if (enabled(f, "lpddr_family")) {
    if (value(f, "sids") == "1") r.model.erase("sids");
  }
  else {
    // A nontrivial experimental rank dimension must never disappear.
    if (value(f, "ranks") == "1") r.model.erase("ranks");
    r.parameters.erase("lpddr_wck_ratio");
  }
  if (value(f, "input_kind") == "trace") {
    for (const char* key : {"pattern", "read_ratio", "seed",
                           "random_address_space_bytes", "effective_random_address_space_bytes",
                           "addr_stride", "requests"})
      r.parameters.erase(key);
  }
  if (f.contains("stack_count")) {
    const auto count = static_cast<std::size_t>(number(f, "stack_count"));
    for (std::size_t i = 0; i < count; ++i) {
      const auto prefix = "stack_" + std::to_string(i) + "_";
      if (!f.contains(prefix + "reads")) continue;
      ResultFields s;
      record_field(s, "stack", i);
      for (auto [old, key] : {std::pair{"reads", "completed_reads"},
                             {"writes", "completed_writes"}, {"bw_GBps", "achieved_bw_GBps"}})
        s[key] = f.at(prefix + old);
      s["avg_read_latency_ns"] = number(f, prefix + "reads") == 0 ? ResultValue(nullptr)
          : ResultValue(number(f, prefix + "avg_read_latency") *
                        number(f, "tick_duration_ps") / 1000);
      r.stacks.push_back(std::move(s));
    }
  }
  if (value(f, "stats_view") == "diagnostic") r.diagnostics = f;
  return r;
}

// Compare executed values, not file locations or just the [override] layer.
// Timing source sections collapse to one numerical timing namespace; provenance
// belongs in the resolved snapshot, not a false "timing changed" notification.
std::vector<ParameterChange> compare_resolved_parameters(
    const std::string& baseline, const std::string& effective) {
  auto parse = [](const std::string& text) {
    std::map<std::string, std::string> result;
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line.front() == '#') continue;
      if (line.front() == '[') { section = line.substr(1, line.find(']') - 1); continue; }
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      auto k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
      if (section == "meta" || section == "model" || section == "validation" ||
          section == "outputs") continue;
      if (section == "workload" && (k == "pattern" || k == "requests" ||
          k == "read_ratio" || k == "seed" || k == "random_address_space_bytes" ||
          k == "addr_stride" || k == "inject_interval" || k == "stats_view" ||
          k == "progress_interval" || k == "trace")) continue;
      if ((k == "source" && section.starts_with("timing.")) || k.ends_with("_file")) continue;
      auto scope = section.starts_with("timing.") ? "timing" : section;
      result[scope + "." + k] = v;
    }
    return result;
  };
  auto a = parse(baseline), b = parse(effective);
  std::vector<ParameterChange> changes;
  for (const auto& [k, v] : b) {
    auto it = a.find(k);
    if (it == a.end() || it->second != v)
      changes.push_back({k, it == a.end() ? "<unset>" : it->second, v});
  }
  return changes;
}

void print_diagnostics(std::ostream& out, const ResultFields& f) {
  for (const auto& [k, v] : f)
    out << std::left << std::setw(32) << k << ": " << result_value_text(v) << '\n';
}

void print_result(std::ostream& out, const ResultReport& r) {
  auto m = [&](const char* k) { return value(r.model, k); };
  auto p = [&](const char* k) { return value(r.parameters, k); };
  auto s = [&](const char* k) { return value(r.metrics, k); };
  auto v = [&](const char* k) { return value(r.validation, k); };
  const bool lp = enabled(r.model, "lpddr_family");
  out << "# ===== 模型 / MODEL =====\n"
      << "名称 / 标准       : " << m("model_name") << " / " << m("standard") << '\n'
      << "组织              : " << m("stack_count") << (lp ? " 器件 × " : " Stack × ")
      << m("channels") << " Channel × " << m("pseudo_channels")
      << (lp ? " Subchannel × " : " PC × ") << (lp ? m("ranks") : m("sids"))
      << (lp ? " Rank\n" : " SID\n");
  if (!lp && r.model.contains("ranks")) out << "实验 Rank 维度    : " << m("ranks") << '\n';
  if (lp && r.model.contains("sids")) out << "实验 SID 维度     : " << m("sids") << "（非 LPDDR 标准层级）\n";
  out << "Bank / 行列       : 每" << (lp ? " SC/Rank" : " PC/SID")
      << " " << m("bank_groups") << " BG × " << m("banks_per_group")
      << " Bank；每 Bank " << m("rows") << " Row × " << m("columns") << " 事务列\n";
  auto gib = [&](const char* key) {
    std::ostringstream os;
    os << std::setprecision(6) << number(r.model, key) / 1073741824.0;
    return os.str();
  };
  out << "计算容量          : 单实例 " << gib("capacity_per_instance_bytes")
      << " GiB，总计 " << gib("aggregate_capacity_bytes") << " GiB\n"
      << "请求 / 事务粒度   : " << m("line_size") << " / " << m("dram_transaction_bytes") << " B\n"
      << "控制策略          : " << m("scheduler") << " / " << m("row_policy")
      << " / " << m("address_mapping") << "，Channel=" << m("channel_mapper")
      << "，Stack=" << m("stack_mapping") << '\n'
      << "功能              : PHY=" << m("mem_phy_mode") << "，后端=" << m("memory_backend")
      << "，refresh=" << m("supports_refresh") << "，RFM=" << m("supports_rfm")
      << "，payload ECC=" << m("ecc_shadow") << '\n'
      << "# ===== 参数 / PARAMETERS =====\n"
      << "接口 / 时钟       : " << p("data_rate_mbps") << " Mb/s/pin，"
      << p("data_bus_bits") << " bit/实例；CK=" << p("tCK_ps")
      << " ps，tick=" << p("tick_duration_ps") << " ps";
  if (lp) out << "，WCK:CK=" << p("lpddr_wck_ratio");
  out << '\n';
  if (p("input_kind") == "trace") out << "负载              : Trace，注入间隔=" << p("inject_interval") << " tick\n";
  else out << "负载              : " << p("pattern") << "，" << p("requests")
           << " 请求，读 " << p("read_ratio") << "%，注入间隔=" << p("inject_interval") << " tick\n"
           << "随机地址域 / 种子 : " << p("effective_random_address_space_bytes") << " B / " << p("seed")
           << "（顺序负载使用 stride=" << p("addr_stride") << " B）\n";
  out << "读 / 写队列容量   : " << p("read_buffer_size") << " / " << p("write_buffer_size") << '\n'
      << "参数比较基准      : " << r.baseline << "，变化 " << r.changes.size()
      << " 项（含联动；非厂商认证，完整清单见 --stats-json）\n";
  // Printed identity already states these final values. The JSON change list
  // remains complete, including derived effects, with no truncation.
  std::size_t extra = 0;
  const std::regex visible(R"((architecture|organization|geometry)\.(channels|pseudo_channels|sids|ranks|bank_groups|banks_per_group|rows|columns|line_size|dram_transaction_bytes|data_rate_mbps|data_bus_bits|tCK_ps|density_gb)|system\.stack_count|controller\.(scheduler|row_policy|address_mapping|read_buffer_size|write_buffer_size))");
  for (bool timing : {true, false}) {
    for (const auto& c : r.changes) {
      if (c.key.starts_with("timing.") != timing || std::regex_match(c.key, visible)) continue;
      if (extra++ < 2) out << "参数变化          : " << c.key << " " << c.baseline << " → " << c.value << '\n';
    }
  }
  out << "# ===== 结果 / RESULTS =====\n"
      << "运行状态          : " << v("run_status") << '\n'
      << "接收 / 完成量     : " << s("host_requests") << " Host；完成 "
      << s("completed_reads") << " 读 / " << s("completed_writes")
      << " 写事务；提交 " << s("dram_transactions") << " 事务\n"
      << "剩余工作量        : 排队/未注入=" << s("remaining_requests")
      << "，pending=" << s("remaining_pending") << "（前者为现有混合计数，非纯 Host 数）\n"
      << "仿真时间          : " << s("simulation_time_ns") << " ns\n"
      << "吞吐量 / 利用率   : " << s("achieved_bw_GBps") << " GB/s / " << s("bandwidth_util_pct") << "%\n"
      << "平均读事务延迟    : " << s("avg_read_latency_ns") << " ns\n"
      << "行命中率          : " << s("row_hit_pct") << "%（首次调度分类）\n"
      << "检查结果          : 命令=" << v("cmd_validation") << "，DFI=" << v("dfi_validation")
      << "，数据检查=" << v("data_checked_reads") << "，错误=" << v("data_mismatches");
  if (v("data_checked_reads") == "0") out << "（无独立数据检查证据）";
  out << '\n';
  if (r.metrics.contains("power_energy_pJ")) out << "能量              : " << s("power_energy_pJ") << " pJ\n";
  if (r.metrics.contains("thermal_peak_temp_C")) out << "平均 / 峰值温度   : " << s("thermal_avg_temp_C") << " / " << s("thermal_peak_temp_C") << " °C\n";
  if (r.metrics.contains("storage_lines_allocated")) out << "后端分配          : " << s("storage_lines_allocated") << " 行，" << s("storage_bytes_allocated") << " B（数据字节记账）\n";
  if (r.validation.contains("golden_mismatches")) out << "Golden 检查       : " << v("golden_verified") << " 行，错误=" << v("golden_mismatches") << '\n';
  if (v("ecc_uncorrectable_errors") != "0" && v("ecc_uncorrectable_errors") != "N/A")
    out << "WARNING: ECC 不可纠正错误=" << v("ecc_uncorrectable_errors") << '\n';
  const double die_count = lp ? 1 : number(r.model, "stack_height");
  const double derived = number(r.model, "capacity_per_instance_bytes") * 8 /
                         1073741824.0 / die_count;
  if (std::abs(derived - number(r.model, "density_gb")) > 1e-8)
    out << "WARNING: 标称 density_gb 与几何折算密度不一致；仿真容量使用几何值\n";
  if (m("memory_system") == "single_controller") out << "WARNING: 单控制器验证模式，以上组织容量不代表已模拟所有 Channel\n";
  if (r.stacks.size() > 1) {
    out << "Stack  完成读  完成写  带宽(GB/s)  平均读延迟(ns)\n";
    for (const auto& st : r.stacks) out << value(st, "stack") << "  "
        << value(st, "completed_reads") << "  " << value(st, "completed_writes") << "  "
        << value(st, "achieved_bw_GBps") << "  " << value(st, "avg_read_latency_ns") << '\n';
  }
}

void write_result_json(const std::string& path, const ResultReport& r,
                       const std::string& status, const std::string& error) {
  if (path.empty()) return;
  if (status != "completed" && status != "truncated" && status != "failed")
    throw std::invalid_argument("invalid run status");
  std::ostringstream out;
  out << "{\n  \"schema_version\": 2,\n  \"run_status\": " << quote(status)
      << ",\n  \"error\": " << quote(error);
  for (auto [name, fields] : {std::pair{"model", &r.model}, {"parameters", &r.parameters},
                             {"metrics", &r.metrics}, {"validation", &r.validation}}) {
    out << ",\n  " << quote(name) << ": ";
    auto clean = *fields;
    clean.erase("run_status"); // only the envelope owns final completion status
    object(out, clean);
  }
  out << ",\n  \"comparison_baseline\": " << quote(r.baseline) << ",\n  \"changes\": [";
  bool first = true;
  for (const auto& c : r.changes) {
    out << (first ? "" : ",") << "\n    {\"key\": " << quote(c.key)
        << ", \"baseline\": " << quote(c.baseline) << ", \"value\": " << quote(c.value) << "}";
    first = false;
  }
  out << "\n  ],\n  \"stacks\": [";
  first = true;
  for (const auto& st : r.stacks) { if (!first) out << ','; object(out, st); first = false; }
  out << "\n  ]";
  if (!r.diagnostics.empty()) {
    auto diagnostic = r.diagnostics;
    diagnostic["run_status"] = status;
    out << ",\n  \"diagnostics\": ";
    object(out, diagnostic);
  }
  out << "\n}\n";
  auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent);
  std::ofstream file(path, std::ios::trunc);
  if (!file) throw std::runtime_error("cannot open result JSON: " + path);
  file << out.str();
  file.close();
  if (!file) throw std::runtime_error("cannot write result JSON: " + path);
}
}  // namespace hbm_sim
