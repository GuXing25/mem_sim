#include "hbm_sim/stats/result.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/controller/controller.hpp"
#include "hbm_sim/frontend/traffic.hpp"

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

ResultReport make_result_report(const DramSpec& spec, const Stats& stats,
    const ControllerOptions& controller, const StorageModelOptions& storage,
    const TrafficOptions& traffic, const ResultContext& context,
    std::span<const Stats> stacks) {
  ResultReport r;
  auto put = [](ResultFields& fields, const char* key, auto value) {
    record_field(fields, key, value);
  };
  put(r.model, "model_name", context.model_name);
  put(r.model, "model_conformance", context.model_conformance);
  put(r.model, "standard", spec.name);
  put(r.model, "timing_profile", spec.timing_profile);
  put(r.model, "mode_profile", spec.mode_profile);
  put(r.model, "stack_count", traffic.stack_count);
  put(r.model, "channels", spec.org.channels);
  put(r.model, "pseudo_channels", spec.org.pseudo_channels);
  if (!spec.lpddr_family || spec.org.sids != 1) put(r.model, "sids", spec.org.sids);
  if (spec.lpddr_family || spec.org.ranks != 1) put(r.model, "ranks", spec.org.ranks);
  put(r.model, "bank_groups", spec.org.bank_groups);
  put(r.model, "banks_per_group", spec.org.banks_per_group);
  put(r.model, "rows", spec.org.rows);
  put(r.model, "columns", spec.org.columns);
  put(r.model, "line_size", spec.org.line_size);
  put(r.model, "dram_transaction_bytes", spec.transaction_bytes());
  put(r.model, "stack_height", spec.stack_height);
  put(r.model, "density_gb", spec.density_gb);
  put(r.model, "capacity_per_instance_bytes", spec.addressable_capacity_bytes());
  put(r.model, "aggregate_capacity_bytes", spec.addressable_capacity_bytes() *
      static_cast<std::uint64_t>(traffic.stack_count));
  put(r.model, "scheduler", to_string(controller.scheduler));
  put(r.model, "row_policy", to_string(controller.row_policy));
  put(r.model, "address_mapping", to_string(spec.address_mapping));
  put(r.model, "channel_mapper", context.channel_mapper);
  put(r.model, "stack_mapping", context.stack_mapping);
  put(r.model, "mem_phy_mode", to_string(controller.phy.mode));
  put(r.model, "phy_protocol", spec.lpddr_family ? "lpddr" : "hbm");
  put(r.model, "memory_backend", to_string(storage.memory_backend.kind));
  put(r.model, "memory_system", context.single_controller ? "single_controller" : "multi_controller");
  put(r.model, "supports_refresh", spec.supports_refresh);
  put(r.model, "supports_rfm", spec.supports_rfm);
  put(r.model, "ecc_shadow", storage.ecc_shadow_enabled);
  put(r.model, "power_model_enabled", storage.power_enabled);
  put(r.model, "thermal_model_enabled", storage.thermal_enabled);
  put(r.model, "lpddr_family", spec.lpddr_family);
  put(r.parameters, "data_rate_mbps", spec.data_rate_mbps);
  put(r.parameters, "data_bus_bits", spec.data_bus_bits);
  put(r.parameters, "tCK_ps", spec.timing.tCK_ps);
  put(r.parameters, "tick_multiplier", spec.tick_multiplier);
  const double tick_ps = spec.timing.tCK_ps / spec.tick_multiplier;
  put(r.parameters, "tick_duration_ps", tick_ps);
  if (spec.lpddr_family) put(r.parameters, "lpddr_wck_ratio", spec.lpddr_wck_ratio);
  put(r.parameters, "input_kind", traffic.trace_path.empty() ? "synthetic" : "trace");
  put(r.parameters, "inject_interval", traffic.inject_interval);
  put(r.parameters, "read_buffer_size", controller.read_buffer_size);
  put(r.parameters, "write_buffer_size", controller.write_buffer_size);
  if (traffic.trace_path.empty()) {
    put(r.parameters, "pattern", traffic.pattern);
    put(r.parameters, "requests", traffic.requests);
    put(r.parameters, "read_ratio", traffic.read_ratio);
    put(r.parameters, "seed", traffic.seed);
    put(r.parameters, "addr_stride", traffic.addr_stride);
    put(r.parameters, "random_address_space_bytes", traffic.random_address_space_bytes);
    put(r.parameters, "effective_random_address_space_bytes", traffic.random_address_space_bytes
        ? traffic.random_address_space_bytes : spec.addressable_capacity_bytes() *
            static_cast<std::uint64_t>(traffic.stack_count));
  }
  r.metrics = collect_metrics(stats, tick_ps / 1000, storage.power_enabled, storage.thermal_enabled);
  put(r.validation, "cmd_validation", "not_run");
  put(r.validation, "dfi_validation", "not_run");
  put(r.validation, "cmd_validation_checked", std::uint64_t{0});
  put(r.validation, "dfi_validation_events", std::uint64_t{0});
  put(r.validation, "data_checked_reads", stats.data_checked_reads);
  put(r.validation, "data_mismatches", stats.data_mismatches);
  put(r.validation, "ecc_uncorrectable_errors", stats.ecc_uncorrectable_errors);
  for (std::size_t i = 0; i < stacks.size(); ++i) {
    const auto& s = stacks[i];
    ResultFields fields;
    put(fields, "stack", i);
    put(fields, "completed_reads", s.completed_reads);
    put(fields, "completed_writes", s.completed_writes);
    put(fields, "achieved_bw_GBps", s.achieved_bandwidth_GBps);
    fields["avg_read_latency_ns"] = s.completed_reads
        ? ResultValue(s.avg_read_latency() * tick_ps / 1000) : ResultValue(nullptr);
    fields["avg_write_latency_ns"] = s.completed_writes
        ? ResultValue(s.avg_write_latency() * tick_ps / 1000) : ResultValue(nullptr);
    r.stacks.push_back(std::move(fields));
  }
  if (context.diagnostic) r.diagnostics = collect_diagnostics(stats, spec, storage);
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

ResultFields result_fields(const ResultReport& report) {
  ResultFields fields;
  for (const auto* section : {&report.model, &report.parameters, &report.metrics,
                              &report.validation, &report.diagnostics})
    fields.insert(section->begin(), section->end());
  return fields;
}

void print_diagnostics(std::ostream& out, const ResultReport& report,
                       const std::string& run_status) {
  auto fields = result_fields(report);
  fields["run_status"] = run_status;
  for (const auto& [k, v] : fields)
    out << std::left << std::setw(32) << k << ": " << result_value_text(v) << '\n';
}

void print_result(std::ostream& out, const ResultReport& r) {
  auto m = [&](const char* k) { return value(r.model, k); };
  auto p = [&](const char* k) { return value(r.parameters, k); };
  auto s = [&](const char* k) { return value(r.metrics, k); };
  auto v = [&](const char* k) { return value(r.validation, k); };
  const bool lp = enabled(r.model, "lpddr_family");
  out << "# ===== MODEL =====\n"
      << "Model / Standard         : " << m("model_name") << " / " << m("standard") << '\n'
      << "Organization             : " << m("stack_count") << (lp ? " Device x " : " Stack x ")
      << m("channels") << " Channel x " << m("pseudo_channels")
      << (lp ? " Subchannel x " : " PC x ") << (lp ? m("ranks") : m("sids"))
      << (lp ? " Rank\n" : " SID\n");
  if (!lp && r.model.contains("ranks")) out << "Experimental Ranks       : " << m("ranks") << '\n';
  if (lp && r.model.contains("sids")) out << "Experimental SIDs        : " << m("sids") << " (nonstandard LPDDR dimension)\n";
  out << "Bank / Row / Column      : " << m("bank_groups") << " BG x " << m("banks_per_group")
      << " Bank per " << (lp ? "SC/Rank" : "PC/SID")
      << "; " << m("rows") << " Row x " << m("columns") << " transaction columns per Bank\n";
  auto gib = [&](const char* key) {
    std::ostringstream os;
    os << std::setprecision(6) << number(r.model, key) / 1073741824.0;
    return os.str();
  };
  out << "Capacity                 : " << gib("capacity_per_instance_bytes")
      << " GiB/instance; total=" << gib("aggregate_capacity_bytes") << " GiB\n"
      << "Host / Transaction Bytes : " << m("line_size") << " / " << m("dram_transaction_bytes") << " B\n"
      << "Policies                 : " << m("scheduler") << " / " << m("row_policy")
      << " / " << m("address_mapping") << "; Channel=" << m("channel_mapper")
      << "; Stack=" << m("stack_mapping") << '\n'
      << "Features                 : PHY=" << m("mem_phy_mode") << "; Backend=" << m("memory_backend")
      << "; Refresh=" << m("supports_refresh") << "; RFM=" << m("supports_rfm")
      << "; Payload ECC=" << m("ecc_shadow") << '\n'
      << "# ===== PARAMETERS =====\n"
      << "Interface / Clock        : " << p("data_rate_mbps") << " Mb/s/pin; "
      << p("data_bus_bits") << " bit/instance; CK=" << p("tCK_ps")
      << " ps; tick=" << p("tick_duration_ps") << " ps";
  if (lp) out << "; WCK:CK=" << p("lpddr_wck_ratio");
  out << '\n';
  if (p("input_kind") == "trace") out << "Workload                 : Trace; inject_interval=" << p("inject_interval") << " tick\n";
  else out << "Workload                 : " << p("pattern") << "; " << p("requests")
           << " requests; reads=" << p("read_ratio") << "%; inject_interval=" << p("inject_interval") << " tick\n"
           << "Address Space / Seed     : " << p("effective_random_address_space_bytes") << " B / " << p("seed")
           << " (stream uses stride=" << p("addr_stride") << " B)\n";
  out << "Read / Write Queues      : " << p("read_buffer_size") << " / " << p("write_buffer_size") << '\n'
      << "Comparison Baseline      : " << r.baseline << "; " << r.changes.size()
      << " changes (including derived values; not vendor-certified; full list: --stats-json)\n";
  // Printed identity already states these final values. The JSON change list
  // remains complete, including derived effects, with no truncation.
  std::size_t extra = 0;
  const std::regex visible(R"((architecture|organization|geometry)\.(channels|pseudo_channels|sids|ranks|bank_groups|banks_per_group|rows|columns|line_size|dram_transaction_bytes|data_rate_mbps|data_bus_bits|tCK_ps|density_gb)|system\.stack_count|controller\.(scheduler|row_policy|address_mapping|read_buffer_size|write_buffer_size))");
  for (bool timing : {true, false}) {
    for (const auto& c : r.changes) {
      if (c.key.starts_with("timing.") != timing || std::regex_match(c.key, visible)) continue;
      if (extra++ < 2) out << "Parameter Change         : " << c.key << " " << c.baseline << " -> " << c.value << '\n';
    }
  }
  out << "# ===== RESULTS =====\n"
      << "Run Status               : " << v("run_status") << '\n'
      << "Requests / Transactions  : " << s("host_requests") << " Host; completed="
      << s("completed_reads") << " reads / " << s("completed_writes")
      << " writes; submitted=" << s("dram_transactions") << " transactions\n"
      << "Remaining Work           : queued/uninjected=" << s("remaining_requests")
      << "; pending=" << s("remaining_pending") << " (mixed work count, not Host-only)\n"
      << "Simulation Time          : " << s("simulation_time_ns") << " ns\n"
      << "Bandwidth / Utilization  : " << s("achieved_bw_GBps") << " GB/s / " << s("bandwidth_util_pct") << "%\n"
      << "Read Transaction Latency : " << s("avg_read_latency_ns") << " ns\n"
      << "Write Transaction Latency: " << s("avg_write_latency_ns") << " ns\n"
      << "Row Hit Rate             : " << s("row_hit_pct") << "% (first-scheduling classification)\n"
      << "Validation               : Command=" << v("cmd_validation") << "; DFI=" << v("dfi_validation")
      << "; checked_reads=" << v("data_checked_reads") << "; data_errors=" << v("data_mismatches");
  if (v("data_checked_reads") == "0") out << " (no independent data-check evidence)";
  out << '\n';
  if (r.metrics.contains("power_energy_pJ")) out << "Energy                   : " << s("power_energy_pJ") << " pJ\n";
  if (r.metrics.contains("self_refresh_time_ns")) {
    out << "Self-refresh Residency   : " << s("self_refresh_time_ns")
        << " ns (sum over controllers)\n";
    if (r.metrics.contains("self_refresh_energy_pJ"))
      out << "Self-refresh Energy      : " << s("self_refresh_energy_pJ") << " pJ\n";
  }
  if (r.metrics.contains("thermal_peak_temp_C")) out << "Mean / Peak Temperature  : " << s("thermal_avg_temp_C") << " / " << s("thermal_peak_temp_C") << " degC\n";
  if (r.metrics.contains("storage_lines_allocated")) out << "Backend Allocation       : " << s("storage_lines_allocated") << " lines; " << s("storage_bytes_allocated") << " B (data-byte accounting)\n";
  if (r.validation.contains("golden_mismatches")) out << "Golden Check             : " << v("golden_verified") << " lines; errors=" << v("golden_mismatches") << '\n';
  if (v("ecc_uncorrectable_errors") != "0" && v("ecc_uncorrectable_errors") != "N/A")
    out << "WARNING: Uncorrectable ECC errors=" << v("ecc_uncorrectable_errors") << '\n';
  const double derived = density_gbit_from_geometry(
      number(r.model, "capacity_per_instance_bytes"), lp,
      static_cast<int>(number(r.model, "stack_height")),
      static_cast<int>(number(r.model, "channels")),
      static_cast<int>(number(r.model, "pseudo_channels")),
      lp ? static_cast<int>(number(r.model, "ranks")) : 1);
  if (std::abs(derived - number(r.model, "density_gb")) > 1e-9 * std::max(1.0, derived))
    out << "WARNING: Nominal density_gb differs from geometry-derived density; simulation capacity uses geometry\n";
  if (m("memory_system") == "single_controller") out << "WARNING: Single-controller validation; organization capacity does not imply all Channels are simulated\n";
  if (r.stacks.size() > 1) {
    out << "Stack  Completed_Reads  Completed_Writes  Bandwidth(GB/s)  Read_Latency(ns)  Write_Latency(ns)\n";
    for (const auto& st : r.stacks) out << value(st, "stack") << "  "
        << value(st, "completed_reads") << "  " << value(st, "completed_writes") << "  "
        << value(st, "achieved_bw_GBps") << "  " << value(st, "avg_read_latency_ns")
        << "  " << value(st, "avg_write_latency_ns") << '\n';
  }
}

void write_result_json(const std::string& path, const ResultReport& r,
                       const std::string& status, const std::string& error) {
  if (path.empty()) return;
  if (status != "completed" && status != "truncated" && status != "failed")
    throw std::invalid_argument("invalid run status");
  std::ostringstream out;
  out << "{\n  \"run_status\": " << quote(status)
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
    diagnostic.erase("run_status");
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
