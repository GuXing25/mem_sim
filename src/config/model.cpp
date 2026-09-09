// Auditable model construction shared by CLI and external library frontends.
// Document loading/layering is separate from DRAM construction and execution.
#include "hbm_sim/config/model.hpp"
#include "hbm_sim/dram/profiles.hpp"
#include "hbm_sim/dram/jedec.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <set>
#include <map>
#include <limits>
#include <iomanip>
#include <sstream>

namespace hbm_sim::config {
namespace {
int parse_int(const std::string& value) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
      })) {
    throw std::invalid_argument("invalid integer: " + value);
  }
  std::size_t parsed = 0;
  const int result = std::stoi(value, &parsed, 10);
  if (parsed != value.size()) {
    throw std::invalid_argument("invalid integer: " + value);
  }
  return result;
}

double parse_double(const std::string& value) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
      })) {
    throw std::invalid_argument("invalid finite number: " + value);
  }
  std::size_t parsed = 0;
  const double result = std::stod(value, &parsed);
  if (parsed != value.size() || !std::isfinite(result)) {
    throw std::invalid_argument("invalid finite number: " + value);
  }
  return result;
}

bool parse_bool(const std::string& value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }
  throw std::invalid_argument("invalid bool value: " + value);
}

std::string lower_value(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    if (c == '-') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

// 配置文件中的 timing 覆盖值不总是 vendor 数据：校准模板经常会显式写出
// research_default 数字，方便审计和替换。这个解析函数让配置能声明后续
// timing override 的来源，避免 strict_timing_table 被模板值“骗过”。
hbm_sim::TimingValueSource parse_timing_value_source(std::string value) {
  value = lower_value(std::move(value));
  if (value == "jedec" || value == "standard") return hbm_sim::TimingValueSource::JEDEC;
  if (value == "vendor" || value == "datasheet") return hbm_sim::TimingValueSource::Vendor;
  if (value == "derived") return hbm_sim::TimingValueSource::Derived;
  if (value == "external_reference" || value == "reference" || value == "ramulator2" ||
      value == "dramsim3") return hbm_sim::TimingValueSource::ExternalReference;
  if (value == "research" || value == "research_default") return hbm_sim::TimingValueSource::ResearchDefault;
  throw std::invalid_argument("invalid timing source: " + value);
}

hbm_sim::AddressMappingKind parse_address_mapping(std::string value) {
  value = lower_value(std::move(value));
  if (value == "default" || value == "legacy") return hbm_sim::AddressMappingKind::Default;
  if (value == "robaracoch" || value == "ro_ba_ra_co_ch") return hbm_sim::AddressMappingKind::RoBaRaCoCh;
  if (value == "chrabaroco" || value == "ch_ra_ba_ro_co") return hbm_sim::AddressMappingKind::ChRaBaRoCo;
  if (value == "rocorabach" || value == "ro_co_ra_ba_ch") return hbm_sim::AddressMappingKind::RoCoRaBaCh;
  throw std::invalid_argument("invalid address_mapping: " + value);
}

hbm_sim::LpddrEfficiencyMode parse_lpddr_efficiency(std::string value) {
  value = lower_value(std::move(value));
  if (value == "normal" || value == "off") return hbm_sim::LpddrEfficiencyMode::Normal;
  if (value == "static" || value == "seff") return hbm_sim::LpddrEfficiencyMode::Static;
  if (value == "dynamic" || value == "deff") return hbm_sim::LpddrEfficiencyMode::Dynamic;
  throw std::invalid_argument("invalid lpddr_efficiency_mode: " + value);
}

hbm_sim::MaintenancePolicyKind parse_maintenance_policy(std::string value) {
  value = lower_value(std::move(value));
  if (value == "per_bank" || value == "perbank" || value == "pb") {
    return hbm_sim::MaintenancePolicyKind::PerBank;
  }
  if (value == "all_bank" || value == "allbank" || value == "ab") {
    return hbm_sim::MaintenancePolicyKind::AllBank;
  }
  throw std::invalid_argument("invalid maintenance policy: " + value);
}

hbm_sim::LpddrDvfsMode parse_lpddr_dvfs_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "nominal" || value == "on") return hbm_sim::LpddrDvfsMode::Nominal;
  if (value == "low" || value == "low_power") return hbm_sim::LpddrDvfsMode::Low;
  if (value == "disabled" || value == "off") return hbm_sim::LpddrDvfsMode::Disabled;
  throw std::invalid_argument("invalid lpddr_dvfs_mode: " + value);
}

hbm_sim::LpddrWckMode parse_lpddr_wck_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "cas_sync" || value == "cas") return hbm_sim::LpddrWckMode::CasSync;
  if (value == "always_on" || value == "alwayson") return hbm_sim::LpddrWckMode::AlwaysOn;
  throw std::invalid_argument(
      "invalid or unimplemented lpddr_wck_mode: " + value +
      " (implemented: cas_sync, always_on)");
}

hbm_sim::LowPowerMode parse_low_power_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "off" || value == "none") return hbm_sim::LowPowerMode::Off;
  if (value == "power_down" || value == "powerdown" || value == "pd") return hbm_sim::LowPowerMode::PowerDown;
  if (value == "self_refresh" || value == "selfrefresh" || value == "sr") return hbm_sim::LowPowerMode::SelfRefresh;
  throw std::invalid_argument("invalid low_power_mode: " + value);
}

hbm_sim::RefreshTemperatureMode parse_refresh_temperature_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "normal" || value == "1x") return hbm_sim::RefreshTemperatureMode::Normal;
  if (value == "high" || value == "2x") return hbm_sim::RefreshTemperatureMode::High;
  if (value == "extended" || value == "ext") return hbm_sim::RefreshTemperatureMode::Extended;
  throw std::invalid_argument("invalid refresh_temperature_mode: " + value);
}

std::string canonical_timing_name_for_key(const std::string& key) {
  // TimingTableEntry 使用统一的内部名称，例如 nRCDRD。用户配置可以写 nrcdrd，
  // 也可以写更接近手册的 trcd_rd_ns。这个映射只负责“名字归一化”，真正的
  // 数值换算在 apply_spec_overrides() 中根据 key 类型完成。
  //
  // 一旦某个 timing 被用户覆盖，配置模块通过 set_timing_source() 记录来源。
  // 默认来源是研究假设；只有显式提供来源的覆盖才可标记为 JEDEC/vendor。
  if (key == "nbl") return "nBL";
  if (key == "ncl") return "nCL";
  if (key == "ncwl") return "nCWL";
  if (key == "nrcdrd" || key == "trcdrd_ns" || key == "trcd_rd_ns") return "nRCDRD";
  if (key == "nrcdwr" || key == "trcdwr_ns" || key == "trcd_wr_ns") return "nRCDWR";
  if (key == "nrp" || key == "trp_ns" || key == "trppb_ns") return "nRP";
  if (key == "nras" || key == "tras_ns") return "nRAS";
  if (key == "nrc" || key == "trc_ns") return "nRC";
  if (key == "nrtp" || key == "trtp_ns") return "nRTP";
  if (key == "nwr" || key == "twr_ns" || key == "twtp_ns") return "nWR";
  if (key == "nccds" || key == "tccds_ns") return "nCCDS";
  if (key == "nccdl" || key == "tccdl_ns") return "nCCDL";
  if (key == "nrrds" || key == "trrds_ns" || key == "trrd_s_ns" ||
      key == "trrd_ns") return "nRRDS";
  if (key == "nrrdl" || key == "trrdl_ns" || key == "trrd_l_ns") return "nRRDL";
  if (key == "nfaw" || key == "tfaw_ns") return "nFAW";
  if (key == "naadmin" || key == "taad_min_ns") return "nAADMin";
  if (key == "naad" || key == "naadmax" || key == "taad_ns" ||
      key == "taad_max_ns") return "nAADMax";
  if (key == "nwck2ck" || key == "twck2ck_ns") return "nWCK2CK";
  if (key == "nwckpst" || key == "twckpst_ns") return "nWCKPST";
  if (key == "ncas" || key == "tcas_ns") return "nCAS";
  if (key == "ncs" || key == "tcs_ns") return "nCS";
  if (key == "nppd" || key == "tppd_ns") return "nPPD";
  if (key == "nrpab" || key == "trpab_ns") return "nRPab";
  if (key == "nwtrs" || key == "twtrs_ns" || key == "twtr_s_ns") return "nWTRS";
  if (key == "nwtrl" || key == "twtrl_ns" || key == "twtr_l_ns") return "nWTRL";
  if (key == "nrtw" || key == "trtw_ns") return "nRTW";
  if (key == "nccdr" || key == "tccdr_ns") return "nCCDR";
  if (key == "nrfc" || key == "trfc_ns" || key == "trfcab_ns") return "nRFC";
  if (key == "nrfcpb" || key == "trfcpb_ns" || key == "trfcdb_ns") return "nRFCpb";
  if (key == "nrfmab" || key == "trfmab_ns") return "nRFMab";
  if (key == "nrfmpb" || key == "trfmpb_ns") return "nRFMpb";
  if (key == "nrrefd" || key == "trrefd_ns") return "nRREFD";
  if (key == "nrefdb2act" || key == "tdbr2act_ns" || key == "trefdb2act_ns") return "nREFDB2ACT";
  if (key == "nrefdb2refdbs" || key == "tdbr2dbr_s_ns" || key == "trefdb2refdb_s_ns") return "nREFDB2REFDBS";
  if (key == "nrefdb2refdbl" || key == "tdbr2dbr_l_ns" || key == "trefdb2refdb_l_ns") return "nREFDB2REFDBL";
  if (key == "nrefi" || key == "trefi_us") return "nREFI";
  if (key == "nrefipb" || key == "trefipb_us" ||
      key == "trefipb_ns" || key == "trefidb_ns") return "nREFIpb";
  if (key == "nmrw" || key == "tmrw_ns") return "nMRW";
  if (key == "nmrr" || key == "tmrr_ns") return "nMRR";
  if (key == "nwcksync" || key == "twcksync_ns") return "nWCKSYNC";
  if (key == "nwcktrain" || key == "twcktrain_ns") return "nWCKTRAIN";
  if (key == "ndvfs" || key == "tdvfs_ns") return "nDVFS";
  if (key == "npdex" || key == "tpdex_ns" || key == "txp_ns") return "nPDEX";
  if (key == "nsrefex" || key == "tsrefex_ns" || key == "txs_ns") return "nSREFEX";
  if (key == "neccscrub" || key == "teccscrub_ns") return "nECCSCRUB";
  if (key == "nraserr" || key == "traserr_ns") return "nRASERR";
  if (key == "nlinkretry" || key == "tlinkretry_ns") return "nLINKRETRY";
  return {};
}


}  // namespace
bool is_spec_override_key(const std::string& key) {
  // 配置文件允许把 DramSpec 中的组织结构、开关和 timing 直接覆盖。
  // 这里显式列白名单，而不是把未知 key 静默塞进 spec_overrides，原因有两个：
  // 1. JEDEC/vendor timing 名称很多，拼错一个字符就会让数值对比失真；
  // 2. CLI 仍处在小型研究工具阶段，明确报错比“容忍但忽略”更容易定位配置问题。
  static const char* keys[] = {
      "timing_profile", "timing_profile_file", "vendor_profile", "mode_profile",
      "timing_source", "timing_override_source",
      "speed_bin_mbps", "density_gb", "stack_height",
      "data_rate_mbps", "data_bus_bits", "prefetch_size", "internal_prefetch_size",
      "dfi_phase_count", "dfi_data_lane_bytes", "dfi_read_latency_nck", "dfi_write_latency_nck",
      "dfi_read_latency_ns", "dfi_write_latency_ns",
      "tick_multiplier", "full_stack_model", "supports_refresh", "refresh_policy",
      "lpddr_dual_bank_refresh", "supports_rfm", "rfm_policy", "supports_ecc",
      "hbm_full_32_channel_stack", "hbm_sid_interleave", "hbm_pc_interleave",
      "hbm_edge_pairing", "hbm_strict_edge_pairing", "hbm_link_crc_bits_per_request",
      "hbm_edge_pairing_matrix", "hbm_sid_mapping", "hbm_ecc_scheme", "hbm_ras_policy",
      "hbm_link_crc_mode", "hbm_link_retry_enabled",
      "hbm_ras_metadata_bits_per_request", "hbm_ecc_bits_per_request",
      "rfm_act_threshold", "rfm_decrement",
      "lpddr_link_protection", "lpddr_dynamic_efficiency", "lpddr_efficiency_mode",
      "lpddr_dvfs_mode", "lpddr_low_data_rate_mbps", "lpddr_wck_mode", "lpddr_wck_ratio",
      "lpddr_mode_register_profile", "lpddr_dbi_enabled", "lpddr_link_ecc_enabled",
      "lpddr_wck_training_mode", "lpddr_dvfs_transition_policy", "lpddr_link_protection_mode",
      "lpddr_low_power_state_policy", "lpddr_wck_training_required",
      "lpddr_dbi_bits_per_request", "lpddr_link_ecc_bits_per_request",
      "lpddr_ca_parity_enabled", "lpddr_ca_parity_bits_per_command",
      "low_power_mode", "low_power_entry_cycles", "low_power_exit_cycles", "self_refresh_exit_cycles",
      "refresh_postpone_limit", "refresh_pullin_limit", "refresh_credit_limit",
      "refresh_temperature_mode", "refresh_high_temp_multiplier",
      "address_mapping", "addr_mapping", "metadata_bits_per_request", "ecc_bits_per_request", "channels",
      "pseudo_channels", "sids", "ranks", "bank_groups", "banks_per_group",
      "rows", "columns", "line_size", "dram_transaction_bytes", "transaction_size",
      "nbl", "ncl", "ncwl", "nrcdrd",
      "nrcdwr", "nrp", "nras", "nrc", "nrtp", "nwr", "nccds", "nccdl",
      "nrrds", "nrrdl", "nfaw", "naad", "naadmin", "naadmax",
      "nwck2ck", "nwckpst", "ncas",
      "ncs", "nppd", "nrpab", "nwtrs", "nwtrl", "nrtw", "nccdr",
      "nrfc", "nrfcpb", "nrfmab", "nrfmpb", "nrrefd", "nrefi",
      "nrefdb2act", "nrefdb2refdbs", "nrefdb2refdbl",
      "nrefipb", "nmrw", "nmrr", "nwcksync", "nwcktrain", "ndvfs",
      "npdex", "nsrefex", "neccscrub", "nraserr", "nlinkretry",
      "tck_ps", "trc_ns", "tras_ns", "trcdrd_ns", "trcd_rd_ns",
      "trcdwr_ns", "trcd_wr_ns", "trp_ns", "trpab_ns", "trppb_ns",
      "trtp_ns", "twr_ns", "twtp_ns", "trrds_ns", "trrd_s_ns",
      "trrdl_ns", "trrd_l_ns", "trrd_ns",
      "tfaw_ns", "twtrs_ns", "twtr_s_ns", "twtrl_ns", "twtr_l_ns",
      "trtw_ns", "tccds_ns", "tccdl_ns", "tccdr_ns", "trfc_ns",
      "trfcab_ns", "trfcpb_ns", "trfcdb_ns", "trfmab_ns", "trfmpb_ns", "trrefd_ns",
      "tdbr2act_ns", "trefdb2act_ns", "tdbr2dbr_s_ns", "trefdb2refdb_s_ns",
      "tdbr2dbr_l_ns", "trefdb2refdb_l_ns",
      "trefi_us", "trefipb_us", "trefipb_ns", "trefidb_ns",
      "twck2ck_ns", "twckpst_ns",
      "tcas_ns", "tcs_ns", "tppd_ns", "taad_ns", "taad_min_ns",
      "taad_max_ns", "tmrw_ns", "tmrr_ns",
      "twcksync_ns", "twcktrain_ns", "tdvfs_ns", "tpdex_ns", "txp_ns",
      "tsrefex_ns", "txs_ns", "teccscrub_ns", "traserr_ns", "tlinkretry_ns"};
  return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
}

void apply_spec_overrides(hbm_sim::DramSpec& spec,
                          const std::vector<std::pair<std::string, std::string>>& overrides,
                          bool resolved_clock) {
  // 第一遍只收集会影响 profile 展开的选择项。组织/timing 的显式覆盖留到
  // profile 之后，保证最终优先级固定为 traits < profile < config/CLI。
  for (const auto& [key, value] : overrides) {
    if (key == "timing_profile") {
      spec.timing_profile = value;
    } else if (key == "timing_profile_file") {
      spec.timing_profile_file = value;
    } else if (key == "vendor_profile") {
      spec.vendor_profile = value;
    } else if (key == "mode_profile") {
      spec.mode_profile = value;
    } else if (key == "speed_bin_mbps") {
      spec.speed_bin_mbps = parse_int(value);
    } else if (key == "data_rate_mbps" && resolved_clock) {
      spec.data_rate_mbps = parse_int(value);
    } else if (key == "density_gb") {
      spec.density_gb = parse_double(value);
    } else if (key == "stack_height") {
      spec.stack_height = parse_int(value);
    } else if (key == "lpddr_dvfs_mode") {
      spec.lpddr_dvfs_mode = parse_lpddr_dvfs_mode(value);
    } else if (key == "lpddr_efficiency_mode") {
      spec.lpddr_efficiency_mode = parse_lpddr_efficiency(value);
    } else if (key == "lpddr_dynamic_efficiency") {
      spec.lpddr_dynamic_efficiency = parse_bool(value);
      if (spec.lpddr_dynamic_efficiency) {
        spec.lpddr_efficiency_mode = hbm_sim::LpddrEfficiencyMode::Dynamic;
      }
    } else if (key == "lpddr_link_protection") {
      spec.lpddr_link_protection = parse_bool(value);
    } else if (key == "lpddr_link_ecc_enabled") {
      spec.lpddr_link_ecc_enabled = parse_bool(value);
    } else if (key == "lpddr_dbi_enabled") {
      spec.lpddr_dbi_enabled = parse_bool(value);
    } else if (key == "lpddr_ca_parity_enabled") {
      spec.lpddr_ca_parity_enabled = parse_bool(value);
    } else if (key == "lpddr_wck_training_required") {
      spec.lpddr_wck_training_required = parse_bool(value);
    } else if (key == "lpddr_low_data_rate_mbps") {
      spec.lpddr_low_data_rate_mbps = parse_int(value);
    } else if (key == "lpddr_wck_mode") {
      spec.lpddr_wck_mode = parse_lpddr_wck_mode(value);
    } else if (key == "refresh_temperature_mode") {
      spec.refresh_temperature_mode = parse_refresh_temperature_mode(value);
    } else if (key == "low_power_mode") {
      spec.low_power_mode = parse_low_power_mode(value);
    } else if (key == "low_power_exit_cycles") {
      spec.low_power_exit_cycles = parse_int(value);
    } else if (key == "self_refresh_exit_cycles") {
      spec.self_refresh_exit_cycles = parse_int(value);
    } else if (key == "hbm_sid_interleave") {
      spec.hbm_sid_interleave = parse_bool(value);
    } else if (key == "hbm_link_crc_mode") {
      spec.hbm_link_crc_mode = value;
    } else if (key == "hbm_link_retry_enabled") {
      spec.hbm_link_retry_enabled = parse_bool(value);
    }
  }

  double profile_tck = 0;
  if (resolved_clock) {
    for (const auto& [key, value] : overrides)
      if (key == "tck_ps") profile_tck = parse_double(value);
    if (!spec.timing_profile_file.empty())
      throw std::invalid_argument("schema 3 requires inline timing overrides; use schema 2 for legacy timing_profile_file");
  }
  hbm_sim::apply_standard_timing_profile(spec, profile_tck);

  // ns/us 覆盖项依赖最终 tCK。显式 tCK 的优先级高于 profile，并且与配置
  // 文件中的书写顺序无关。
  for (const auto& [key, value] : overrides) {
    if (key == "tck_ps") {
      spec.timing.tCK_ps = parse_double(value);
    }
  }

  hbm_sim::TimingValueSource timing_override_source = hbm_sim::TimingValueSource::ResearchDefault;
  for (const auto& [key, value] : overrides) {
    const std::string timing_name = canonical_timing_name_for_key(key);
    if (key == "timing_source" || key == "timing_override_source") {
      timing_override_source = parse_timing_value_source(value);
      continue;
    }
    // 下方长 if/else 列表看起来朴素，但有意保持“每个配置 key 到字段”的
    // 一对一关系。对标准模型来说，可审计性比过度抽象更重要：读者能直接查到
    // tRFCpb_ns 最终改的是 spec.timing.nRFCpb，并且换算使用当前 tCK_ps。
    if (key == "timing_profile") spec.timing_profile = value;
    else if (key == "timing_profile_file") spec.timing_profile_file = value;
    else if (key == "vendor_profile") spec.vendor_profile = value;
    else if (key == "mode_profile") spec.mode_profile = value;
    else if (key == "speed_bin_mbps") spec.speed_bin_mbps = parse_int(value);
    else if (key == "density_gb") spec.density_gb = parse_double(value);
    else if (key == "stack_height") spec.stack_height = parse_int(value);
    else if (key == "data_rate_mbps") spec.data_rate_mbps = parse_int(value);
    else if (key == "data_bus_bits") spec.data_bus_bits = parse_int(value);
    else if (key == "prefetch_size" || key == "internal_prefetch_size") spec.internal_prefetch_size = parse_int(value);
    else if (key == "dfi_phase_count") spec.dfi_phase_count = parse_int(value);
    else if (key == "dfi_data_lane_bytes") spec.dfi_data_lane_bytes = parse_int(value);
    else if (key == "dfi_read_latency_nck") spec.dfi_read_latency_nck = parse_int(value);
    else if (key == "dfi_write_latency_nck") spec.dfi_write_latency_nck = parse_int(value);
    else if (key == "dfi_read_latency_ns") spec.dfi_read_latency_nck = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "dfi_write_latency_ns") spec.dfi_write_latency_nck = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tick_multiplier") spec.tick_multiplier = parse_int(value);
    else if (key == "full_stack_model") spec.full_stack_model = parse_bool(value);
    else if (key == "supports_refresh") spec.supports_refresh = parse_bool(value);
    else if (key == "refresh_policy") spec.refresh_policy = parse_maintenance_policy(value);
    else if (key == "lpddr_dual_bank_refresh") spec.lpddr_dual_bank_refresh = parse_bool(value);
    else if (key == "supports_rfm") spec.supports_rfm = parse_bool(value);
    else if (key == "rfm_policy") spec.rfm_policy = parse_maintenance_policy(value);
    else if (key == "supports_ecc") spec.supports_ecc = parse_bool(value);
    else if (key == "hbm_full_32_channel_stack") spec.hbm_full_32_channel_stack = parse_bool(value);
    else if (key == "hbm_sid_interleave") {
      spec.hbm_sid_interleave = parse_bool(value);
      spec.column_bus_scope = spec.hbm_sid_interleave ? hbm_sim::TimingScope::Sid
                                                      : hbm_sim::TimingScope::PseudoChannel;
    }
    else if (key == "hbm_pc_interleave") spec.hbm_pc_interleave = parse_bool(value);
    else if (key == "hbm_edge_pairing") spec.hbm_edge_pairing = parse_bool(value);
    else if (key == "hbm_strict_edge_pairing") spec.hbm_strict_edge_pairing = parse_bool(value);
    else if (key == "hbm_edge_pairing_matrix") spec.hbm_edge_pairing_matrix = value;
    else if (key == "hbm_sid_mapping") spec.hbm_sid_mapping = value;
    else if (key == "hbm_ecc_scheme") spec.hbm_ecc_scheme = value;
    else if (key == "hbm_ras_policy") spec.hbm_ras_policy = value;
    else if (key == "hbm_link_crc_mode") spec.hbm_link_crc_mode = value;
    else if (key == "hbm_link_retry_enabled") spec.hbm_link_retry_enabled = parse_bool(value);
    else if (key == "hbm_link_crc_bits_per_request") spec.hbm_link_crc_bits_per_request = parse_int(value);
    else if (key == "hbm_ras_metadata_bits_per_request") spec.hbm_ras_metadata_bits_per_request = parse_int(value);
    else if (key == "hbm_ecc_bits_per_request") spec.hbm_ecc_bits_per_request = parse_int(value);
    else if (key == "rfm_act_threshold") spec.rfm_act_threshold = parse_int(value);
    else if (key == "rfm_decrement") spec.rfm_decrement = parse_int(value);
    else if (key == "lpddr_link_protection") spec.lpddr_link_protection = parse_bool(value);
    else if (key == "lpddr_dynamic_efficiency") {
      spec.lpddr_dynamic_efficiency = parse_bool(value);
      if (spec.lpddr_dynamic_efficiency) spec.lpddr_efficiency_mode = hbm_sim::LpddrEfficiencyMode::Dynamic;
    }
    else if (key == "lpddr_efficiency_mode") spec.lpddr_efficiency_mode = parse_lpddr_efficiency(value);
    else if (key == "lpddr_dvfs_mode") spec.lpddr_dvfs_mode = parse_lpddr_dvfs_mode(value);
    else if (key == "lpddr_low_data_rate_mbps") spec.lpddr_low_data_rate_mbps = parse_int(value);
    else if (key == "lpddr_wck_mode") spec.lpddr_wck_mode = parse_lpddr_wck_mode(value);
    else if (key == "lpddr_wck_ratio") spec.lpddr_wck_ratio = parse_int(value);
    else if (key == "lpddr_mode_register_profile") spec.lpddr_mode_register_profile = value;
    else if (key == "lpddr_wck_training_mode") spec.lpddr_wck_training_mode = value;
    else if (key == "lpddr_dvfs_transition_policy") spec.lpddr_dvfs_transition_policy = value;
    else if (key == "lpddr_link_protection_mode") spec.lpddr_link_protection_mode = value;
    else if (key == "lpddr_low_power_state_policy") spec.lpddr_low_power_state_policy = value;
    else if (key == "lpddr_wck_training_required") spec.lpddr_wck_training_required = parse_bool(value);
    else if (key == "lpddr_dbi_enabled") spec.lpddr_dbi_enabled = parse_bool(value);
    else if (key == "lpddr_link_ecc_enabled") spec.lpddr_link_ecc_enabled = parse_bool(value);
    else if (key == "lpddr_ca_parity_enabled") spec.lpddr_ca_parity_enabled = parse_bool(value);
    else if (key == "lpddr_dbi_bits_per_request") spec.lpddr_dbi_bits_per_request = parse_int(value);
    else if (key == "lpddr_link_ecc_bits_per_request") spec.lpddr_link_ecc_bits_per_request = parse_int(value);
    else if (key == "lpddr_ca_parity_bits_per_command") spec.lpddr_ca_parity_bits_per_command = parse_int(value);
    else if (key == "low_power_mode") spec.low_power_mode = parse_low_power_mode(value);
    else if (key == "low_power_entry_cycles") spec.low_power_entry_cycles = parse_int(value);
    else if (key == "low_power_exit_cycles") spec.low_power_exit_cycles = parse_int(value);
    else if (key == "self_refresh_exit_cycles") spec.self_refresh_exit_cycles = parse_int(value);
    else if (key == "refresh_postpone_limit") spec.refresh_postpone_limit = parse_int(value);
    else if (key == "refresh_pullin_limit") spec.refresh_pullin_limit = parse_int(value);
    else if (key == "refresh_credit_limit") spec.refresh_credit_limit = parse_int(value);
    else if (key == "refresh_temperature_mode") spec.refresh_temperature_mode = parse_refresh_temperature_mode(value);
    else if (key == "refresh_high_temp_multiplier") spec.refresh_high_temp_multiplier = parse_int(value);
    else if (key == "address_mapping" || key == "addr_mapping") spec.address_mapping = parse_address_mapping(value);
    else if (key == "metadata_bits_per_request") spec.metadata_bits_per_request = parse_int(value);
    else if (key == "ecc_bits_per_request") spec.ecc_bits_per_request = parse_int(value);
    else if (key == "channels") spec.org.channels = parse_int(value);
    else if (key == "pseudo_channels") spec.org.pseudo_channels = parse_int(value);
    else if (key == "sids") spec.org.sids = parse_int(value);
    else if (key == "ranks") spec.org.ranks = parse_int(value);
    else if (key == "bank_groups") spec.org.bank_groups = parse_int(value);
    else if (key == "banks_per_group") spec.org.banks_per_group = parse_int(value);
    else if (key == "rows") spec.org.rows = parse_int(value);
    else if (key == "columns") spec.org.columns = parse_int(value);
    else if (key == "line_size") spec.org.line_size = parse_int(value);
    else if (key == "dram_transaction_bytes" || key == "transaction_size") {
      spec.org.dram_transaction_bytes = parse_int(value);
    }
    else if (key == "nbl") spec.timing.nBL = parse_int(value);
    else if (key == "ncl") spec.timing.nCL = parse_int(value);
    else if (key == "ncwl") spec.timing.nCWL = parse_int(value);
    else if (key == "nrcdrd") spec.timing.nRCDRD = parse_int(value);
    else if (key == "nrcdwr") spec.timing.nRCDWR = parse_int(value);
    else if (key == "nrp") spec.timing.nRP = parse_int(value);
    else if (key == "nras") spec.timing.nRAS = parse_int(value);
    else if (key == "nrc") spec.timing.nRC = parse_int(value);
    else if (key == "nrtp") spec.timing.nRTP = parse_int(value);
    else if (key == "nwr") spec.timing.nWR = parse_int(value);
    else if (key == "nccds") spec.timing.nCCDS = parse_int(value);
    else if (key == "nccdl") spec.timing.nCCDL = parse_int(value);
    else if (key == "nrrds") spec.timing.nRRDS = parse_int(value);
    else if (key == "nrrdl") spec.timing.nRRDL = parse_int(value);
    else if (key == "nfaw") spec.timing.nFAW = parse_int(value);
    else if (key == "naadmin") spec.timing.nAADMin = parse_int(value);
    else if (key == "naad" || key == "naadmax") spec.timing.nAADMax = parse_int(value);
    else if (key == "nwck2ck") spec.timing.nWCK2CK = parse_int(value);
    else if (key == "nwckpst") spec.timing.nWCKPST = parse_int(value);
    else if (key == "ncas") spec.timing.nCAS = parse_int(value);
    else if (key == "ncs") spec.timing.nCS = parse_int(value);
    else if (key == "nppd") spec.timing.nPPD = parse_int(value);
    else if (key == "nrpab") spec.timing.nRPab = parse_int(value);
    else if (key == "nwtrs") spec.timing.nWTRS = parse_int(value);
    else if (key == "nwtrl") spec.timing.nWTRL = parse_int(value);
    else if (key == "nrtw") spec.timing.nRTW = parse_int(value);
    else if (key == "nccdr") spec.timing.nCCDR = parse_int(value);
    else if (key == "nrfc") spec.timing.nRFC = parse_int(value);
    else if (key == "nrfcpb") spec.timing.nRFCpb = parse_int(value);
    else if (key == "nrfmab") spec.timing.nRFMab = parse_int(value);
    else if (key == "nrfmpb") spec.timing.nRFMpb = parse_int(value);
    else if (key == "nrrefd") spec.timing.nRREFD = parse_int(value);
    else if (key == "nrefdb2act") spec.timing.nREFDB2ACT = parse_int(value);
    else if (key == "nrefdb2refdbs") spec.timing.nREFDB2REFDBS = parse_int(value);
    else if (key == "nrefdb2refdbl") spec.timing.nREFDB2REFDBL = parse_int(value);
    else if (key == "nrefi") spec.timing.nREFI = parse_int(value);
    else if (key == "nrefipb") spec.timing.nREFIpb = parse_int(value);
    else if (key == "nmrw") spec.timing.nMRW = parse_int(value);
    else if (key == "nmrr") spec.timing.nMRR = parse_int(value);
    else if (key == "nwcksync") spec.timing.nWCKSYNC = parse_int(value);
    else if (key == "nwcktrain") spec.timing.nWCKTRAIN = parse_int(value);
    else if (key == "ndvfs") spec.timing.nDVFS = parse_int(value);
    else if (key == "npdex") spec.timing.nPDEX = parse_int(value);
    else if (key == "nsrefex") spec.timing.nSREFEX = parse_int(value);
    else if (key == "neccscrub") spec.timing.nECCSCRUB = parse_int(value);
    else if (key == "nraserr") spec.timing.nRASERR = parse_int(value);
    else if (key == "nlinkretry") spec.timing.nLINKRETRY = parse_int(value);
    else if (key == "tck_ps") spec.timing.tCK_ps = parse_double(value);
    else if (key == "trc_ns") spec.timing.nRC = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tras_ns") spec.timing.nRAS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trcdrd_ns" || key == "trcd_rd_ns") spec.timing.nRCDRD = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trcdwr_ns" || key == "trcd_wr_ns") spec.timing.nRCDWR = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trp_ns" || key == "trppb_ns") spec.timing.nRP = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trpab_ns") spec.timing.nRPab = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trtp_ns") spec.timing.nRTP = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    // LPDDR 文档常把写到预充的约束写作 tWTP；本模型的终端 WR->PRE
    // 路径统一保存在 nWR，因此与外部 profile 解析器保持同一别名语义。
    else if (key == "twr_ns" || key == "twtp_ns") spec.timing.nWR = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trrds_ns" || key == "trrd_s_ns" || key == "trrd_ns") spec.timing.nRRDS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trrdl_ns" || key == "trrd_l_ns") spec.timing.nRRDL = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tfaw_ns") spec.timing.nFAW = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "twtrs_ns" || key == "twtr_s_ns") spec.timing.nWTRS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "twtrl_ns" || key == "twtr_l_ns") spec.timing.nWTRL = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trtw_ns") spec.timing.nRTW = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tccds_ns") spec.timing.nCCDS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tccdl_ns") spec.timing.nCCDL = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tccdr_ns") spec.timing.nCCDR = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trfc_ns" || key == "trfcab_ns") spec.timing.nRFC = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trfcpb_ns" || key == "trfcdb_ns") spec.timing.nRFCpb = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trfmab_ns") spec.timing.nRFMab = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trfmpb_ns") spec.timing.nRFMpb = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trrefd_ns") spec.timing.nRREFD = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tdbr2act_ns" || key == "trefdb2act_ns") {
      spec.timing.nREFDB2ACT = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    }
    else if (key == "tdbr2dbr_s_ns" || key == "trefdb2refdb_s_ns") {
      spec.timing.nREFDB2REFDBS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    }
    else if (key == "tdbr2dbr_l_ns" || key == "trefdb2refdb_l_ns") {
      spec.timing.nREFDB2REFDBL = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    }
    else if (key == "trefi_us") spec.timing.nREFI = hbm_sim::jedec::us_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trefipb_us") spec.timing.nREFIpb = hbm_sim::jedec::us_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "trefipb_ns" || key == "trefidb_ns") spec.timing.nREFIpb = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "twck2ck_ns") spec.timing.nWCK2CK = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "twckpst_ns") spec.timing.nWCKPST = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tcas_ns") spec.timing.nCAS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tcs_ns") spec.timing.nCS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tppd_ns") spec.timing.nPPD = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "taad_min_ns") spec.timing.nAADMin = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "taad_ns" || key == "taad_max_ns") spec.timing.nAADMax = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tmrw_ns") spec.timing.nMRW = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tmrr_ns") spec.timing.nMRR = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "twcksync_ns") spec.timing.nWCKSYNC = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "twcktrain_ns") spec.timing.nWCKTRAIN = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tdvfs_ns") spec.timing.nDVFS = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tpdex_ns" || key == "txp_ns") spec.timing.nPDEX = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tsrefex_ns" || key == "txs_ns") spec.timing.nSREFEX = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "teccscrub_ns") spec.timing.nECCSCRUB = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "traserr_ns") spec.timing.nRASERR = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "tlinkretry_ns") spec.timing.nLINKRETRY = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else throw std::invalid_argument("unknown spec override key: " + key);

    if (!timing_name.empty()) {
      hbm_sim::set_timing_source(
          spec,
          timing_name,
          timing_override_source,
          "Overridden by config/CLI as " + std::string(hbm_sim::to_string(timing_override_source)) + ".");
    }
  }
  if (spec.supports_ecc && spec.hbm_ecc_bits_per_request == 0 && spec.ecc_bits_per_request == 0 &&
      !spec.lpddr_family) {
    spec.hbm_ecc_bits_per_request = spec.standard == hbm_sim::DramStandard::Hbm4 ? 16 : 64;
  }
  if (spec.lpddr_link_protection && spec.lpddr_link_ecc_bits_per_request == 0) {
    spec.lpddr_link_ecc_bits_per_request = 16;
  }
  if (spec.lpddr_dbi_enabled && spec.lpddr_dbi_bits_per_request == 0) {
    spec.lpddr_dbi_bits_per_request = 8;
  }
  hbm_sim::finalize_spec(spec);
}


ResolvedModelInputs resolve_coupled_inputs(const DramSpec& baseline,
                                         const ModelOverrides& inputs) {
  std::map<std::string, std::string> explicit_values;
  for (const auto& [key, value] : inputs) explicit_values[key] = value;
  std::map<std::string, std::string> timing_spellings;
  for (const auto& [key, value] : explicit_values) {
    const auto timing = canonical_timing_name_for_key(key);
    if (timing.empty()) continue;
    auto [position, inserted] = timing_spellings.emplace(timing, key);
    if (!inserted && position->second != key)
      throw std::invalid_argument("ambiguous timing inputs for " + timing + ": " +
          position->second + " and " + key + "; use one nCK/ns spelling");
  }
  auto automatic = [&](const std::string& key) {
    const auto it = explicit_values.find(key);
    return it == explicit_values.end() || lower_value(it->second) == "auto";
  };
  auto positive_int = [&](const std::string& key, int fallback) {
    const int value = automatic(key) ? fallback : parse_int(explicit_values.at(key));
    if (value <= 0) throw std::invalid_argument(key + " must be > 0");
    return value;
  };
  auto format = [](double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
  };
  ResolvedModelInputs result;
  // Only documented dependent fields accept auto. A typo or a meaningless auto
  // on a protocol/algorithm field must reach the normal strict parser and fail.
  const std::set<std::string> dependent{
      "speed_bin_mbps", "data_rate_mbps", "data_bus_bits", "density_gb", "tck_ps"};
  for (const auto& [key, value] : inputs) {
    if (!dependent.contains(key)) result.overrides.emplace_back(key, value);
  }
  auto derive = [&](const std::string& key, double value, const std::string& formula,
                    double tolerance = 1e-9) {
    if (!std::isfinite(value) || value <= 0)
      throw std::invalid_argument("invalid derived " + key);
    if (!automatic(key)) {
      const auto given = parse_double(explicit_values.at(key));
      if (std::abs(given - value) > tolerance * std::max(1.0, std::abs(value)))
        throw std::invalid_argument(key + "=" + explicit_values.at(key) +
                                    " conflicts with derived " + format(value) +
                                    " (" + formula + "); omit it or use auto");
      result.overrides.emplace_back(key, explicit_values.at(key));
    } else {
      result.overrides.emplace_back(key, format(value));
      result.derived.push_back({key, format(value), formula});
    }
  };

  const bool low_dvfs = baseline.standard == DramStandard::Lpddr6 &&
      explicit_values.contains("lpddr_dvfs_mode") &&
      parse_lpddr_dvfs_mode(explicit_values.at("lpddr_dvfs_mode")) == LpddrDvfsMode::Low;
  const int default_rate = low_dvfs
      ? positive_int("lpddr_low_data_rate_mbps", baseline.lpddr_low_data_rate_mbps)
      : baseline.data_rate_mbps;
  const int rate = !automatic("data_rate_mbps")
                       ? positive_int("data_rate_mbps", baseline.data_rate_mbps)
                       : positive_int("speed_bin_mbps", default_rate);
  if (low_dvfs && rate != default_rate)
    throw std::invalid_argument("low DVFS data_rate_mbps must match lpddr_low_data_rate_mbps");
  derive("data_rate_mbps", rate, "selected data rate in Mb/s/pin");
  derive("speed_bin_mbps", rate, "data_rate_mbps (timing speed selector)");
  const int ratio = baseline.standard == DramStandard::Lpddr5
                        ? positive_int("lpddr_wck_ratio", 4) : 2;
  if (baseline.standard == DramStandard::Lpddr6 &&
      !automatic("lpddr_wck_ratio") && positive_int("lpddr_wck_ratio", 2) != 2)
    throw std::invalid_argument("LPDDR6 implements WCK:CK=2:1, not this ratio");
  if (baseline.standard == DramStandard::Lpddr5 && ratio != 2 && ratio != 4)
    throw std::invalid_argument("LPDDR5 WCK:CK must be 2:1 or 4:1");
  // HBM data transfer has four edges per CK; LPDDR uses DDR WCK.
  const double tck_ps = 2000000.0 * ratio / rate;
  // Legacy tables round tCK to integer ps. Explicit values may use that rounding;
  // larger contradictions are rejected instead of creating different time bases.
  derive("tck_ps", tck_ps, "2e6 * WCK_ratio / data_rate (HBM: 4e6/data_rate)",
         0.5 / std::max(1.0, tck_ps));

  DramSpec geometry = baseline;
  auto& org = geometry.org;
  org.channels = positive_int("channels", org.channels);
  org.pseudo_channels = positive_int("pseudo_channels", org.pseudo_channels);
  org.sids = positive_int("sids", org.sids);
  org.ranks = positive_int("ranks", org.ranks);
  org.bank_groups = positive_int("bank_groups", org.bank_groups);
  org.banks_per_group = positive_int("banks_per_group", org.banks_per_group);
  org.rows = positive_int("rows", org.rows);
  org.columns = positive_int("columns", org.columns);
  org.line_size = positive_int("line_size", org.line_size);
  if (!automatic("dram_transaction_bytes")) {
    org.dram_transaction_bytes = parse_int(explicit_values.at("dram_transaction_bytes"));
    if (org.dram_transaction_bytes < 0)
      throw std::invalid_argument("dram_transaction_bytes must be >= 0");
  }
  const auto capacity = geometry.addressable_capacity_bytes();
  if (capacity == 0) throw std::invalid_argument("DRAM geometry capacity overflow");
  // Freeze the very geometry used above. Profile expansion must not silently
  // replace an omitted SID/row dimension when stack_height or speed changes.
  for (const auto& [key, value] : std::initializer_list<std::pair<const char*, int>>{
           {"channels", org.channels}, {"pseudo_channels", org.pseudo_channels},
           {"sids", org.sids}, {"ranks", org.ranks}, {"bank_groups", org.bank_groups},
           {"banks_per_group", org.banks_per_group}, {"rows", org.rows},
           {"columns", org.columns}, {"line_size", org.line_size},
           {"dram_transaction_bytes", org.dram_transaction_bytes}})
    result.overrides.emplace_back(key, std::to_string(value));
  const double density = static_cast<double>(capacity) / 134217728.0 /
      (baseline.lpddr_family
           ? static_cast<double>(org.channels) * org.pseudo_channels * org.ranks
           : positive_int("stack_height", baseline.stack_height));
  derive("density_gb", density,
         baseline.lpddr_family
             ? "geometry_bytes * 8 / 2^30 / (channels * subchannels * ranks)"
             : "geometry_bytes * 8 / 2^30 / stack_height");
  const auto base_lanes = static_cast<std::int64_t>(baseline.org.channels) *
                          baseline.org.pseudo_channels;
  if (base_lanes <= 0 || baseline.data_bus_bits % base_lanes != 0)
    throw std::invalid_argument("baseline interface width is not divisible by channels * PC");
  const auto lanes = static_cast<std::uint64_t>(org.channels) * org.pseudo_channels;
  const auto dq_bits = baseline.data_bus_bits / base_lanes;
  if (dq_bits <= 0 || lanes > static_cast<std::uint64_t>(std::numeric_limits<int>::max() / dq_bits))
    throw std::invalid_argument("derived data_bus_bits overflow");
  const auto width = lanes * dq_bits;
  // Width is an independently configurable research input; only omission/auto
  // follows the selected baseline's per-subchannel DQ width.
  if (automatic("data_bus_bits")) derive("data_bus_bits", static_cast<double>(width),
      "channels * pseudo_channels * baseline DQ bits per subchannel");
  else result.overrides.emplace_back("data_bus_bits", std::to_string(
      positive_int("data_bus_bits", baseline.data_bus_bits)));
  return result;
}

DramSpec build_model(const std::string& standard, const ModelOverrides& overrides,
                     int schema_version, std::vector<ParameterDerivation>* derived) {
  if (schema_version < 1 || schema_version > 3)
    throw std::invalid_argument("unsupported model config schema");
  if (derived) derived->clear();
  DramSpec spec = make_spec_draft(standard);
  if (schema_version == 3) {
    const auto resolved = resolve_coupled_inputs(make_spec(standard), overrides);
    apply_spec_overrides(spec, resolved.overrides, true);
    if (derived) *derived = resolved.derived;
    std::set<std::string> explicit_timings;
    for (const auto& [key, value] : overrides) {
      const auto name = canonical_timing_name_for_key(key);
      if (!name.empty()) explicit_timings.insert(name);
    }
    // Recompute only documented dependencies, not every timing with a plausible
    // algebraic relationship. Explicit secondary constraints remain independent.
    auto derive_timing = [&](const char* name, int& field, int value,
                             std::initializer_list<const char*> dependencies,
                             const char* formula) {
      if (explicit_timings.contains(name)) return;
      if (std::none_of(dependencies.begin(), dependencies.end(),
          [&](const char* dep) { return explicit_timings.contains(dep); })) return;
      field = value;
      TimingValueSource source = TimingValueSource::Derived;
      for (const auto& entry : spec.timing_table.entries) {
        if (std::none_of(dependencies.begin(), dependencies.end(),
                        [&](const char* dep) { return entry.name == dep; })) continue;
        if (entry.source == TimingValueSource::ResearchDefault || entry.vendor_required_for_numeric)
          source = TimingValueSource::ResearchDefault;
        else if (entry.source == TimingValueSource::ExternalReference && source != TimingValueSource::ResearchDefault)
          source = TimingValueSource::ExternalReference;
      }
      set_timing_source(spec, name, source, std::string("Calculated: ") + formula +
                        "; inherits unresolved dependency provenance.");
      if (derived) derived->push_back({name, std::to_string(value), formula});
    };
    const auto rc = static_cast<std::int64_t>(spec.timing.nRAS) + spec.timing.nRP;
    if (rc > std::numeric_limits<int>::max())
      throw std::invalid_argument("derived nRC overflow");
    derive_timing("nRC", spec.timing.nRC, static_cast<int>(rc), {"nRAS", "nRP"}, "nRAS + nRP");
    // LPDDR6 has an independent JEDEC RFM duration (Tables 366/367).
    if (spec.standard != DramStandard::Lpddr6) {
      derive_timing("nRFMab", spec.timing.nRFMab, spec.timing.nRFC, {"nRFC"}, "project default: nRFMab = nRFC");
      derive_timing("nRFMpb", spec.timing.nRFMpb, spec.timing.nRFCpb, {"nRFCpb"}, "project default: nRFMpb = nRFCpb");
    }
    finalize_spec(spec);
  } else {
    apply_spec_overrides(spec, overrides);
  }
  return spec;
}
}  // namespace hbm_sim::config
