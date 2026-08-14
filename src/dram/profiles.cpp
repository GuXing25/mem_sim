// Timing profile 数据库：按 standard/speed-bin/density/stack-height/mode
// 展开 HBM3/HBM4/LPDDR5/LPDDR6 的可配置 timing 表。
//
// 这里的 generic profile 不是具体厂商保证值；它用于把 JEDEC/vendor 表的维度
// 正式建模出来。真正做数值级对比时，应设置 vendor_profile 并继续把本文件中
// 对应 profile 行替换为目标 datasheet 的数值。
#include "hbm_sim/dram/profiles.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>

#include "hbm_sim/dram/jedec.hpp"

namespace hbm_sim {
namespace {

int tck_ps_for_speed(int data_rate_mbps) {
  if (data_rate_mbps <= 0) {
    return 0;
  }
  return static_cast<int>(std::llround(4000000.0 / static_cast<double>(data_rate_mbps)));
}

bool vendor_calibrated(const DramSpec& spec) {
  return spec.vendor_profile != "generic" && spec.vendor_profile != "jedec" && !spec.vendor_profile.empty();
}

void mark(DramSpec& spec, const char* name, TimingValueSource source, const std::string& note) {
  set_timing_source(spec, name, source, note);
}

void mark_many(DramSpec& spec,
               std::initializer_list<const char*> names,
               TimingValueSource source,
               const std::string& note) {
  for (const char* name : names) {
    mark(spec, name, source, note);
  }
}

bool mode_contains(const DramSpec& spec, const std::string& needle) {
  std::string mode = spec.mode_profile;
  std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return mode.find(needle) != std::string::npos;
}

std::string trim_profile_value(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string normalize_profile_key(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
    if (c == '-') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return key;
}

bool parse_profile_bool(std::string value) {
  value = normalize_profile_key(std::move(value));
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  throw std::invalid_argument("invalid bool in timing profile file: " + value);
}

TimingValueSource parse_profile_source(std::string value) {
  value = normalize_profile_key(std::move(value));
  if (value == "jedec" || value == "standard") return TimingValueSource::JEDEC;
  if (value == "vendor" || value == "datasheet") return TimingValueSource::Vendor;
  if (value == "derived") return TimingValueSource::Derived;
  if (value == "research" || value == "research_default") return TimingValueSource::ResearchDefault;
  throw std::invalid_argument("invalid timing source in profile file: " + value);
}

std::string profile_timing_name_for_key(const std::string& key) {
  if (key == "nbl") return "nBL";
  if (key == "ncl") return "nCL";
  if (key == "ncwl") return "nCWL";
  if (key == "nrcdrd" || key == "trcdrd_ns" || key == "trcd_rd_ns") return "nRCDRD";
  if (key == "nrcdwr" || key == "trcdwr_ns" || key == "trcd_wr_ns") return "nRCDWR";
  if (key == "nrp" || key == "trp_ns" || key == "trppb_ns") return "nRP";
  if (key == "nrpab" || key == "trpab_ns") return "nRPab";
  if (key == "nras" || key == "tras_ns") return "nRAS";
  if (key == "nrc" || key == "trc_ns") return "nRC";
  if (key == "nrtp" || key == "trtp_ns") return "nRTP";
  if (key == "nwr" || key == "twr_ns" || key == "twtp_ns") return "nWR";
  if (key == "nccds" || key == "tccds_ns") return "nCCDS";
  if (key == "nccdl" || key == "tccdl_ns") return "nCCDL";
  if (key == "nrrds" || key == "trrds_ns" || key == "trrd_ns") return "nRRDS";
  if (key == "nrrdl" || key == "trrdl_ns") return "nRRDL";
  if (key == "nfaw" || key == "tfaw_ns") return "nFAW";
  if (key == "naad" || key == "taad_ns") return "nAAD";
  if (key == "nwck2ck" || key == "twck2ck_ns") return "nWCK2CK";
  if (key == "nwckpst" || key == "twckpst_ns") return "nWCKPST";
  if (key == "ncas" || key == "tcas_ns") return "nCAS";
  if (key == "ncs" || key == "tcs_ns") return "nCS";
  if (key == "nppd" || key == "tppd_ns") return "nPPD";
  if (key == "nwtrs" || key == "twtrs_ns" || key == "twtr_s_ns") return "nWTRS";
  if (key == "nwtrl" || key == "twtrl_ns" || key == "twtr_l_ns") return "nWTRL";
  if (key == "nrtw" || key == "trtw_ns") return "nRTW";
  if (key == "nccdr" || key == "tccdr_ns") return "nCCDR";
  if (key == "nrfc" || key == "trfc_ns" || key == "trfcab_ns") return "nRFC";
  if (key == "nrfcpb" || key == "trfcpb_ns" || key == "trfcdb_ns") return "nRFCpb";
  if (key == "nrfmab" || key == "trfmab_ns" || key == "trrfab_ns") return "nRFMab";
  if (key == "nrfmpb" || key == "trfmpb_ns" || key == "trrfpb_ns") return "nRFMpb";
  if (key == "nrrefd" || key == "trrefd_ns") return "nRREFD";
  if (key == "nrefdb2act" || key == "tdbr2act_ns" || key == "trefdb2act_ns") return "nREFDB2ACT";
  if (key == "nrefdb2refdbs" || key == "tdbr2dbr_s_ns" || key == "trefdb2refdb_s_ns") return "nREFDB2REFDBS";
  if (key == "nrefdb2refdbl" || key == "tdbr2dbr_l_ns" || key == "trefdb2refdb_l_ns") return "nREFDB2REFDBL";
  if (key == "nrefi" || key == "trefi_us") return "nREFI";
  if (key == "nrefipb" || key == "trefipb_us" || key == "trefipb_ns" || key == "trefidb_ns") return "nREFIpb";
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

void set_profile_timing(DramSpec& spec,
                        const std::string& name,
                        int value,
                        TimingValueSource source,
                        const std::string& note) {
  if (name == "nBL") spec.timing.nBL = value;
  else if (name == "nCL") spec.timing.nCL = value;
  else if (name == "nCWL") spec.timing.nCWL = value;
  else if (name == "nRCDRD") spec.timing.nRCDRD = value;
  else if (name == "nRCDWR") spec.timing.nRCDWR = value;
  else if (name == "nRP") spec.timing.nRP = value;
  else if (name == "nRPab") spec.timing.nRPab = value;
  else if (name == "nRAS") spec.timing.nRAS = value;
  else if (name == "nRC") spec.timing.nRC = value;
  else if (name == "nRTP") spec.timing.nRTP = value;
  else if (name == "nWR") spec.timing.nWR = value;
  else if (name == "nCCDS") spec.timing.nCCDS = value;
  else if (name == "nCCDL") spec.timing.nCCDL = value;
  else if (name == "nRRDS") spec.timing.nRRDS = value;
  else if (name == "nRRDL") spec.timing.nRRDL = value;
  else if (name == "nFAW") spec.timing.nFAW = value;
  else if (name == "nAAD") spec.timing.nAAD = value;
  else if (name == "nWCK2CK") spec.timing.nWCK2CK = value;
  else if (name == "nWCKPST") spec.timing.nWCKPST = value;
  else if (name == "nCAS") spec.timing.nCAS = value;
  else if (name == "nCS") spec.timing.nCS = value;
  else if (name == "nPPD") spec.timing.nPPD = value;
  else if (name == "nWTRS") spec.timing.nWTRS = value;
  else if (name == "nWTRL") spec.timing.nWTRL = value;
  else if (name == "nRTW") spec.timing.nRTW = value;
  else if (name == "nCCDR") spec.timing.nCCDR = value;
  else if (name == "nRFC") spec.timing.nRFC = value;
  else if (name == "nRFCpb") spec.timing.nRFCpb = value;
  else if (name == "nRFMab") spec.timing.nRFMab = value;
  else if (name == "nRFMpb") spec.timing.nRFMpb = value;
  else if (name == "nRREFD") spec.timing.nRREFD = value;
  else if (name == "nREFDB2ACT") spec.timing.nREFDB2ACT = value;
  else if (name == "nREFDB2REFDBS") spec.timing.nREFDB2REFDBS = value;
  else if (name == "nREFDB2REFDBL") spec.timing.nREFDB2REFDBL = value;
  else if (name == "nREFI") spec.timing.nREFI = value;
  else if (name == "nREFIpb") spec.timing.nREFIpb = value;
  else if (name == "nMRW") spec.timing.nMRW = value;
  else if (name == "nMRR") spec.timing.nMRR = value;
  else if (name == "nWCKSYNC") spec.timing.nWCKSYNC = value;
  else if (name == "nWCKTRAIN") spec.timing.nWCKTRAIN = value;
  else if (name == "nDVFS") spec.timing.nDVFS = value;
  else if (name == "nPDEX") spec.timing.nPDEX = value;
  else if (name == "nSREFEX") spec.timing.nSREFEX = value;
  else if (name == "nECCSCRUB") spec.timing.nECCSCRUB = value;
  else if (name == "nRASERR") spec.timing.nRASERR = value;
  else if (name == "nLINKRETRY") spec.timing.nLINKRETRY = value;
  else throw std::invalid_argument("unsupported timing profile field: " + name);
  set_timing_source(spec, name, source, note);
}

int profile_nck_from_key(const DramSpec& spec, const std::string& key, const std::string& value) {
  if (key.size() >= 3 && key.rfind("_ns") == key.size() - 3) {
    return jedec::ns_to_nck(std::stod(value), spec.timing.tCK_ps);
  }
  if (key.size() >= 3 && key.rfind("_us") == key.size() - 3) {
    return jedec::us_to_nck(std::stod(value), spec.timing.tCK_ps);
  }
  return std::stoi(value);
}

void apply_profile_file_value(DramSpec& spec,
                              const std::string& key,
                              const std::string& value,
                              TimingValueSource source,
                              const std::string& note) {
  const std::string timing_name = profile_timing_name_for_key(key);
  if (!timing_name.empty()) {
    set_profile_timing(spec, timing_name, profile_nck_from_key(spec, key, value), source, note);
    return;
  }

  if (key == "timing_profile") spec.timing_profile = value;
  else if (key == "vendor_profile") spec.vendor_profile = value;
  else if (key == "mode_profile") spec.mode_profile = value;
  else if (key == "speed_bin_mbps") {
    spec.speed_bin_mbps = std::stoi(value);
    // speed-bin 通常描述外部可见数据速率。除非文件后续显式覆盖
    // data_rate_mbps/tCK_ps，否则保持推导数据速率与 tCK 一致。
    spec.data_rate_mbps = spec.speed_bin_mbps;
    spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.data_rate_mbps));
  }
  else if (key == "density_gb") spec.density_gb = std::stoi(value);
  else if (key == "stack_height") spec.stack_height = std::stoi(value);
  else if (key == "data_rate_mbps") {
    spec.data_rate_mbps = std::stoi(value);
    // 提供有效数据速率时重新计算标称时钟周期；后续显式 tCK_ps 仍优先。
    spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.data_rate_mbps));
  }
  else if (key == "data_bus_bits") spec.data_bus_bits = std::stoi(value);
  else if (key == "prefetch_size" || key == "internal_prefetch_size") spec.internal_prefetch_size = std::stoi(value);
  else if (key == "dfi_phase_count") spec.dfi_phase_count = std::stoi(value);
  else if (key == "dfi_data_lane_bytes") spec.dfi_data_lane_bytes = std::stoi(value);
  else if (key == "dfi_read_latency_nck") spec.dfi_read_latency_nck = std::stoi(value);
  else if (key == "dfi_write_latency_nck") spec.dfi_write_latency_nck = std::stoi(value);
  else if (key == "dfi_read_latency_ns") spec.dfi_read_latency_nck = jedec::ns_to_nck(std::stod(value), spec.timing.tCK_ps);
  else if (key == "dfi_write_latency_ns") spec.dfi_write_latency_nck = jedec::ns_to_nck(std::stod(value), spec.timing.tCK_ps);
  else if (key == "tick_multiplier") spec.tick_multiplier = std::stoi(value);
  else if (key == "tck_ps") spec.timing.tCK_ps = std::stod(value);
  else if (key == "channels") spec.org.channels = std::stoi(value);
  else if (key == "pseudo_channels") spec.org.pseudo_channels = std::stoi(value);
  else if (key == "sids") spec.org.sids = std::stoi(value);
  else if (key == "ranks") spec.org.ranks = std::stoi(value);
  else if (key == "bank_groups") spec.org.bank_groups = std::stoi(value);
  else if (key == "banks_per_group") spec.org.banks_per_group = std::stoi(value);
  else if (key == "rows") spec.org.rows = std::stoi(value);
  else if (key == "columns") spec.org.columns = std::stoi(value);
  else if (key == "line_size") spec.org.line_size = std::stoi(value);
  else if (key == "dram_transaction_bytes" || key == "transaction_size") {
    spec.org.dram_transaction_bytes = std::stoi(value);
  }
  else if (key == "supports_refresh") spec.supports_refresh = parse_profile_bool(value);
  else if (key == "supports_rfm") spec.supports_rfm = parse_profile_bool(value);
  else if (key == "supports_ecc") spec.supports_ecc = parse_profile_bool(value);
  else if (key == "hbm_full_32_channel_stack") spec.hbm_full_32_channel_stack = parse_profile_bool(value);
  else if (key == "hbm_sid_interleave") {
    spec.hbm_sid_interleave = parse_profile_bool(value);
    spec.column_bus_scope = spec.hbm_sid_interleave ? TimingScope::Sid : TimingScope::PseudoChannel;
  } else if (key == "hbm_pc_interleave") spec.hbm_pc_interleave = parse_profile_bool(value);
  else if (key == "hbm_edge_pairing") spec.hbm_edge_pairing = parse_profile_bool(value);
  else if (key == "hbm_strict_edge_pairing") spec.hbm_strict_edge_pairing = parse_profile_bool(value);
  else if (key == "hbm_edge_pairing_matrix") spec.hbm_edge_pairing_matrix = value;
  else if (key == "hbm_sid_mapping") spec.hbm_sid_mapping = value;
  else if (key == "hbm_ecc_scheme") spec.hbm_ecc_scheme = value;
  else if (key == "hbm_ras_policy") spec.hbm_ras_policy = value;
  else if (key == "hbm_link_crc_mode") spec.hbm_link_crc_mode = value;
  else if (key == "hbm_link_retry_enabled") spec.hbm_link_retry_enabled = parse_profile_bool(value);
  else if (key == "hbm_link_crc_bits_per_request") spec.hbm_link_crc_bits_per_request = std::stoi(value);
  else if (key == "hbm_ras_metadata_bits_per_request") spec.hbm_ras_metadata_bits_per_request = std::stoi(value);
  else if (key == "hbm_ecc_bits_per_request") spec.hbm_ecc_bits_per_request = std::stoi(value);
  else if (key == "lpddr_link_protection") spec.lpddr_link_protection = parse_profile_bool(value);
  else if (key == "lpddr_mode_register_profile") spec.lpddr_mode_register_profile = value;
  else if (key == "lpddr_wck_training_mode") spec.lpddr_wck_training_mode = value;
  else if (key == "lpddr_dvfs_transition_policy") spec.lpddr_dvfs_transition_policy = value;
  else if (key == "lpddr_link_protection_mode") spec.lpddr_link_protection_mode = value;
  else if (key == "lpddr_low_power_state_policy") spec.lpddr_low_power_state_policy = value;
  else if (key == "lpddr_wck_training_required") spec.lpddr_wck_training_required = parse_profile_bool(value);
  else if (key == "lpddr_dbi_enabled") spec.lpddr_dbi_enabled = parse_profile_bool(value);
  else if (key == "lpddr_link_ecc_enabled") spec.lpddr_link_ecc_enabled = parse_profile_bool(value);
  else if (key == "lpddr_ca_parity_enabled") spec.lpddr_ca_parity_enabled = parse_profile_bool(value);
  else if (key == "lpddr_dbi_bits_per_request") spec.lpddr_dbi_bits_per_request = std::stoi(value);
  else if (key == "lpddr_link_ecc_bits_per_request") spec.lpddr_link_ecc_bits_per_request = std::stoi(value);
  else if (key == "lpddr_ca_parity_bits_per_command") spec.lpddr_ca_parity_bits_per_command = std::stoi(value);
  else if (key == "refresh_postpone_limit") spec.refresh_postpone_limit = std::stoi(value);
  else if (key == "refresh_pullin_limit") spec.refresh_pullin_limit = std::stoi(value);
  else if (key == "refresh_credit_limit") spec.refresh_credit_limit = std::stoi(value);
  else if (key == "refresh_high_temp_multiplier") spec.refresh_high_temp_multiplier = std::stoi(value);
  else throw std::invalid_argument("unsupported key in timing profile file: " + key);
}

void apply_external_timing_profile_file(DramSpec& spec) {
  if (spec.timing_profile_file.empty()) {
    return;
  }
  std::ifstream in(spec.timing_profile_file);
  if (!in) {
    throw std::runtime_error("failed to open timing profile file: " + spec.timing_profile_file);
  }

  TimingValueSource source = TimingValueSource::JEDEC;
  std::string note = "Loaded from timing profile file " + spec.timing_profile_file + ".";
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    lineno++;
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    line = trim_profile_value(line);
    if (line.empty()) {
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      throw std::runtime_error("timing profile file line " + std::to_string(lineno) + " missing '='");
    }
    std::string key = normalize_profile_key(trim_profile_value(line.substr(0, eq)));
    std::string value = trim_profile_value(line.substr(eq + 1));
    if (key == "source" || key == "timing_source") {
      source = parse_profile_source(value);
      continue;
    }
    if (key == "note" || key == "timing_note") {
      note = value;
      continue;
    }
    if (key == "profile_file_format") {
      continue;
    }
    apply_profile_file_value(spec, key, value, source, note);
  }
}

struct TimingNs {
  double value = 0.0;
  bool standard_defined = true;
};

TimingNs hbm3_trfcab_ns(int density_gb, int stack_height) {
  // JESD238B.01 表 93 按裸片密度和堆叠高度给出 tRFCab。标准中标记 TBD 的项
  // 使用最近的非 TBD 研究值，使仿真器可运行，同时保留密度和堆叠高度输入。
  const int stack = std::max(4, stack_height);
  if (density_gb <= 8) {
    if (stack <= 4) return {260.0, false};
    if (stack <= 8) return {260.0, true};
    if (stack <= 12) return {310.0, true};
    return {350.0, true};
  }
  if (density_gb <= 16) {
    if (stack <= 4) return {260.0, true};
    if (stack <= 8) return {350.0, true};
    if (stack <= 12) return {410.0, true};
    return {450.0, true};
  }
  if (density_gb <= 24) {
    if (stack <= 4) return {310.0, true};
    if (stack <= 8) return {410.0, true};
    if (stack <= 12) return {450.0, true};
    return {450.0, false};
  }
  if (stack <= 8) return {450.0, false};
  return {520.0, false};
}

TimingNs hbm3_trfcpb_ns(int density_gb) {
  // JESD238B.01 表 93 给出 16Gb 和 24Gb tRFCpb；8Gb/32Gb 仍为 TBD。
  if (density_gb <= 8) return {200.0, false};
  if (density_gb <= 16) return {200.0, true};
  if (density_gb <= 24) return {240.0, true};
  return {240.0, false};
}

struct Lpddr6CoreTimingNs {
  double rcd_write = 0.0;
  double rcd_read = 0.0;
  double tacu = 0.0;
  double rp_ab = 0.0;
  double rp_pb = 0.0;
  double rtp_tail = 0.0;
  double wtp = 0.0;
  double wtr_s = 0.0;
  double wtr_l = 0.0;
};

int lpddr6_nacu_for_speed(int data_rate_mbps, bool dvfsl_enabled) {
  // JESD209-6 表 415 和 420 按数据速率区间给出 nACU。10667Mbps 以上仍为
  // TBD，因此沿用最后一个已定义区间。
  struct Band {
    int upper_mbps;
    int nacu_no_dvfsl;
    int nacu_dvfsl;
  };
  static constexpr Band bands[] = {
      {1067, 6, 7},    {1600, 9, 11},   {2133, 12, 14},  {2750, 16, 18},
      {3200, 18, 21},  {3750, 21, 24},  {4267, 24, 27},  {4800, 27, 31},
      {5500, 31, 35},  {6400, 36, 41},  {7500, 42, 48},  {8533, 47, 54},
      {9600, 53, 61},  {10667, 59, 68}, {14400, 59, 68},
  };
  for (const auto& band : bands) {
    if (data_rate_mbps <= band.upper_mbps) {
      return dvfsl_enabled ? band.nacu_dvfsl : band.nacu_no_dvfsl;
    }
  }
  return dvfsl_enabled ? 68 : 59;
}

Lpddr6CoreTimingNs lpddr6_core_timing_ns(bool dvfsl_enabled, bool link_protection, bool efficiency_mode) {
  // JESD209-6 表 414/416/417/418/419/421/422/423 按 DVFSL、链路保护和
  // 静态/动态 efficiency mode 划分核心时序。这里只映射控制器可见时序。
  if (!dvfsl_enabled) {
    Lpddr6CoreTimingNs t{8.0, 18.0, 22.0, 21.0, 18.0, 1.25, 12.0, 6.25, 12.0};
    if (!link_protection && efficiency_mode) {
      t.wtp = 14.0;
      t.wtr_s = 8.25;
      t.wtr_l = 14.0;
    } else if (link_protection && !efficiency_mode) {
      t.wtp = 16.0;
      t.wtr_s = 10.25;
      t.wtr_l = 16.0;
    } else if (link_protection && efficiency_mode) {
      t.wtp = 18.0;
      t.wtr_s = 12.25;
      t.wtr_l = 18.0;
    }
    return t;
  }

  Lpddr6CoreTimingNs t{9.2, 20.7, 25.3, 24.2, 20.7, 1.5, 13.8, 7.2, 13.8};
  if (!link_protection && efficiency_mode) {
    t.wtp = 16.1;
    t.wtr_s = 9.5;
    t.wtr_l = 16.1;
  } else if (link_protection && !efficiency_mode) {
    t.wtp = 18.4;
    t.wtr_s = 11.8;
    t.wtr_l = 18.4;
  } else if (link_protection && efficiency_mode) {
    t.wtp = 20.7;
    t.wtr_s = 14.1;
    t.wtr_l = 20.7;
  }
  return t;
}

TimingNs lpddr6_trfcab_ns(int density_gb) {
  // JESD209-6 表 302 按每两个子通道的密度给出刷新恢复时间。
  if (density_gb <= 4) return {210.0, false};
  if (density_gb <= 8) return {210.0, true};
  if (density_gb <= 16) return {280.0, true};
  if (density_gb <= 32) return {380.0, true};
  return {380.0, false};
}

TimingNs lpddr6_trfcdb_ns(int density_gb) {
  if (density_gb <= 4) return {140.0, false};
  if (density_gb <= 8) return {140.0, true};
  if (density_gb <= 16) return {160.0, true};
  if (density_gb <= 32) return {210.0, true};
  return {210.0, false};
}

void apply_hbm4_profile(DramSpec& spec) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 8000;
  if (spec.density_gb <= 0) spec.density_gb = 32;
  if (spec.stack_height <= 0) spec.stack_height = 8;

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.name = "HBM4";
  spec.data_rate_mbps = spec.speed_bin_mbps;
  spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.speed_bin_mbps));
  spec.full_stack_model = true;
  spec.hbm_full_32_channel_stack = true;
  spec.org.channels = 32;
  spec.org.pseudo_channels = 2;
  spec.org.sids = std::max(1, spec.stack_height / 4);
  spec.org.ranks = 1;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 8;
  spec.org.rows = 1 << 14;
  spec.org.columns = 32;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 32;
  spec.data_bus_bits = 2048;
  spec.internal_prefetch_size = 8;
  spec.tick_multiplier = 2;
  spec.hbm_sid_mapping = spec.stack_height >= 16 ? "sid_per_4hi_slice" : "sid_pair_8hi";
  spec.hbm_ras_policy = spec.hbm_ras_metadata_bits_per_request > 0 ? "ras_metadata_sideband" : "counter_only";
  if (spec.rfm_act_threshold <= 0) {
    spec.rfm_act_threshold = 256;
  }
  if (spec.rfm_decrement <= 0) {
    spec.rfm_decrement = spec.rfm_act_threshold;
  }
  if (spec.hbm_link_crc_mode == "crc16" && spec.hbm_link_crc_bits_per_request == 0) {
    spec.hbm_link_crc_bits_per_request = 16;
  }
  spec.column_bus_scope = spec.hbm_sid_interleave ? TimingScope::Sid : TimingScope::PseudoChannel;
  if (spec.supports_ecc && spec.hbm_ecc_bits_per_request == 0 && spec.ecc_bits_per_request == 0) {
    spec.hbm_ecc_bits_per_request = 16;
  }
  if (spec.hbm_link_crc_bits_per_request == 0 && spec.mode_profile.find("link_crc") != std::string::npos) {
    spec.hbm_link_crc_bits_per_request = 16;
    spec.hbm_link_crc_mode = "crc16";
  }
  spec.hbm_ecc_scheme =
      spec.hbm_ecc_bits_per_request > 0 ? "metadata_16b_per_32B_transaction" : "none";

  const int speed = spec.speed_bin_mbps;
  if (speed <= 6400) {
    spec.timing.nCL = 26;
    spec.timing.nCWL = 8;
    spec.timing.nRCDRD = 26;
    spec.timing.nRCDWR = 14;
    spec.timing.nRP = 26;
    spec.timing.nRAS = 48;
    spec.timing.nRTP = 6;
    spec.timing.nWR = 24;
    spec.timing.nRRDS = 8;
    spec.timing.nRRDL = 8;
    spec.timing.nFAW = 24;
    spec.timing.nRTW = 18;
    spec.timing.nWTRS = 8;
    spec.timing.nWTRL = 12;
  } else if (speed <= 8000) {
    spec.timing.nCL = 30;
    spec.timing.nCWL = 10;
    spec.timing.nRCDRD = 30;
    spec.timing.nRCDWR = 16;
    spec.timing.nRP = 30;
    spec.timing.nRAS = 54;
    spec.timing.nRTP = 8;
    spec.timing.nWR = 28;
    spec.timing.nRRDS = 8;
    spec.timing.nRRDL = 10;
    spec.timing.nFAW = 28;
    spec.timing.nRTW = 25;
    spec.timing.nWTRS = 9;
    spec.timing.nWTRL = 13;
  } else {
    spec.timing.nCL = 34;
    spec.timing.nCWL = 12;
    spec.timing.nRCDRD = 34;
    spec.timing.nRCDWR = 18;
    spec.timing.nRP = 34;
    spec.timing.nRAS = 60;
    spec.timing.nRTP = 8;
    spec.timing.nWR = 32;
    spec.timing.nRRDS = 10;
    spec.timing.nRRDL = 12;
    spec.timing.nFAW = 32;
    spec.timing.nRTW = 28;
    spec.timing.nWTRS = 10;
    spec.timing.nWTRL = 14;
  }

  spec.timing.nBL = 2;
  spec.timing.nRC = spec.timing.nRAS + spec.timing.nRP;
  spec.timing.nCCDS = 2;
  spec.timing.nCCDL = jedec::hbm_tccdl_nck(spec.timing.tCK_ps);
  spec.timing.nCCDR = spec.hbm_sid_interleave ? spec.timing.nCCDS + 1 : spec.timing.nCCDS;
  spec.timing.nAAD = 8;
  spec.timing.nWCK2CK = 1;
  spec.timing.nWCKPST = 8;
  spec.timing.nCAS = 0;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nRPab = 20;

  double trfc_ab_ns = 450.0;
  double trfc_pb_ns = 280.0;
  if (spec.density_gb <= 24) {
    trfc_ab_ns = 380.0;
    trfc_pb_ns = 240.0;
  } else if (spec.density_gb >= 48) {
    trfc_ab_ns = 560.0;
    trfc_pb_ns = 350.0;
  }
  spec.timing.nRFC = jedec::ns_to_nck(trfc_ab_ns, spec.timing.tCK_ps);
  spec.timing.nRFCpb = jedec::ns_to_nck(trfc_pb_ns, spec.timing.tCK_ps);
  spec.timing.nRFMab = spec.timing.nRFC;
  spec.timing.nRFMpb = spec.timing.nRFCpb;
  spec.timing.nRREFD = jedec::max_ns_or_nck(8.0, 3, spec.timing.tCK_ps);
  spec.timing.nREFI = jedec::us_to_nck(3.9, spec.timing.tCK_ps);
  int rotation_banks = std::max(1, spec.stack_height) * 4;
  spec.timing.nREFIpb = jedec::hbm_trefipb_nck(spec.timing.tCK_ps, rotation_banks);
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  // 这些字段对 HBM 核心命令约束不生效，但保留旧 preset 的控制/低功耗默认值，
  // 使重构前后的输出和低功耗实验保持一致。
  spec.timing.nWCKSYNC = 8;
  spec.timing.nWCKTRAIN = 64;
  spec.timing.nDVFS = 128;
  spec.timing.nPDEX = 8;
  spec.timing.nSREFEX = 256;
  spec.timing.nECCSCRUB = spec.supports_ecc ? jedec::ns_to_nck(64.0, spec.timing.tCK_ps) : 0;
  spec.timing.nRASERR = spec.hbm_link_retry_enabled ? jedec::ns_to_nck(32.0, spec.timing.tCK_ps) : 0;
  spec.timing.nLINKRETRY = spec.hbm_link_retry_enabled ? jedec::ns_to_nck(16.0, spec.timing.tCK_ps) : 0;

  const bool vendor = vendor_calibrated(spec);
  const std::string note = "HBM4 profile=" + spec.timing_profile + ", vendor=" + spec.vendor_profile +
                           ", speed=" + std::to_string(spec.speed_bin_mbps) +
                           ", density=" + std::to_string(spec.density_gb) +
                           "Gb, stack=" + std::to_string(spec.stack_height) + "Hi.";
  mark_many(spec,
            {"nBL", "nCCDS", "nCCDL", "nRREFD", "nREFI", "nREFIpb", "nRFC", "nRFCpb", "nRFMab", "nRFMpb"},
            TimingValueSource::JEDEC,
            note);
  mark_many(spec, {"nMRW", "nMRR", "nECCSCRUB"},
            vendor ? TimingValueSource::Vendor : TimingValueSource::ResearchDefault,
            "HBM4 control/RAS/link timing requires device mode table or vendor reliability guide.");
  if (spec.hbm_link_retry_enabled) {
    mark_many(spec, {"nRASERR", "nLINKRETRY"},
              vendor ? TimingValueSource::Vendor : TimingValueSource::ResearchDefault,
              "HBM4 enabled link-retry timing requires a vendor reliability guide.");
  }
  if (vendor) {
    mark_many(spec,
              {"nCL", "nCWL", "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC", "nRTP", "nWR",
               "nRRDS", "nRRDL", "nFAW", "nRTW", "nWTRS", "nWTRL", "nCCDR", "nRPab"},
              TimingValueSource::Vendor,
              note);
  }
}

void apply_hbm3_profile(DramSpec& spec) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 6400;
  if (spec.density_gb <= 0) spec.density_gb = 16;
  if (spec.stack_height <= 0) spec.stack_height = 8;

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.name = "HBM3";
  spec.data_rate_mbps = spec.speed_bin_mbps;
  spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.speed_bin_mbps));
  spec.full_stack_model = true;
  spec.org.channels = 16;
  spec.org.pseudo_channels = 2;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 4;
  spec.org.banks_per_group = 4;
  spec.org.rows = 1 << 15;
  spec.org.columns = 1 << 5;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 0;
  spec.data_bus_bits = 1024;
  spec.internal_prefetch_size = 8;
  spec.tick_multiplier = 2;
  spec.timing.nBL = 2;
  spec.timing.nCL = spec.speed_bin_mbps <= 5600 ? 24 : 26;
  spec.timing.nCWL = spec.speed_bin_mbps <= 5600 ? 7 : 8;
  spec.timing.nRCDRD = spec.timing.nCL;
  spec.timing.nRCDWR = spec.timing.nCWL + 6;
  spec.timing.nRP = spec.timing.nRCDRD;
  spec.timing.nRAS = 48;
  spec.timing.nRC = spec.timing.nRAS + spec.timing.nRP;
  spec.timing.nRTP = 6;
  spec.timing.nWR = 24;
  spec.timing.nCCDS = 2;
  spec.timing.nCCDL = jedec::hbm_tccdl_nck(spec.timing.tCK_ps);
  spec.timing.nCCDR = spec.timing.nCCDS + 1;
  spec.timing.nRRDS = 8;
  spec.timing.nRRDL = 8;
  spec.timing.nFAW = 24;
  spec.timing.nAAD = 8;
  spec.timing.nWCK2CK = 1;
  spec.timing.nWCKPST = 8;
  spec.timing.nCAS = 0;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nRPab = 20;
  spec.timing.nWTRS = 8;
  spec.timing.nWTRL = 12;
  spec.timing.nRTW = 18;
  const TimingNs trfc_ab = hbm3_trfcab_ns(spec.density_gb, spec.stack_height);
  const TimingNs trfc_pb = hbm3_trfcpb_ns(spec.density_gb);
  spec.timing.nRFC = jedec::ns_to_nck(trfc_ab.value, spec.timing.tCK_ps);
  spec.timing.nRFCpb = jedec::ns_to_nck(trfc_pb.value, spec.timing.tCK_ps);
  spec.timing.nRFMab = 0;
  spec.timing.nRFMpb = 0;
  spec.timing.nRREFD = jedec::max_ns_or_nck(8.0, 3, spec.timing.tCK_ps);
  spec.timing.nREFDB2ACT = 0;
  spec.timing.nREFDB2REFDBS = 0;
  spec.timing.nREFDB2REFDBL = 0;
  spec.timing.nREFI = jedec::us_to_nck(3.9, spec.timing.tCK_ps);
  spec.timing.nREFIpb = jedec::hbm_trefipb_nck(spec.timing.tCK_ps, std::max(1, spec.stack_height) * 4);
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  spec.timing.nWCKSYNC = 8;
  spec.timing.nWCKTRAIN = 64;
  spec.timing.nDVFS = 128;
  spec.timing.nPDEX = 8;
  spec.timing.nSREFEX = 256;
  spec.timing.nECCSCRUB = 0;
  spec.timing.nRASERR = 0;
  spec.timing.nLINKRETRY = 0;
  const std::string note = "HBM3 JESD238B.01 Table 93 profile=" + spec.timing_profile +
                           ", density=" + std::to_string(spec.density_gb) +
                           "Gb, stack=" + std::to_string(spec.stack_height) + "Hi.";
  mark_many(spec,
            {"nBL", "nCCDS", "nCCDL", "nRREFD", "nREFI", "nREFIpb"},
            TimingValueSource::JEDEC,
            note);
  mark_many(spec,
            {"nRFC", "nRFCpb"},
            (trfc_ab.standard_defined && trfc_pb.standard_defined) ? TimingValueSource::JEDEC
                                                                   : TimingValueSource::ResearchDefault,
            note);
  if (vendor_calibrated(spec)) {
    mark_many(spec,
              {"nCL", "nCWL", "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC", "nRFC", "nRFCpb"},
              TimingValueSource::Vendor,
              "HBM3 vendor profile " + spec.vendor_profile);
  }
}

void apply_lpddr6_profile(DramSpec& spec) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 10667;
  if (spec.density_gb <= 0) spec.density_gb = 16;
  if (spec.lpddr_dvfs_mode == LpddrDvfsMode::Low) {
    spec.data_rate_mbps = std::max(1, spec.lpddr_low_data_rate_mbps);
  } else if (spec.lpddr_dvfs_mode == LpddrDvfsMode::Disabled) {
    spec.data_rate_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : spec.speed_bin_mbps;
  } else {
    spec.data_rate_mbps = spec.speed_bin_mbps;
  }

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.data_rate_mbps));
  spec.name = "LPDDR6";
  spec.org.channels = 1;
  spec.org.pseudo_channels = 2;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 4;
  spec.org.banks_per_group = 4;
  spec.org.rows = 1 << 16;
  spec.org.columns = 1 << 6;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 32;
  spec.data_bus_bits = 24;
  // JESD209-6 x12 subchannel uses BL24: 256 data bits + 32 non-data bits.
  // The project stores the burst beat count here and the data payload in
  // dram_transaction_bytes.
  spec.internal_prefetch_size = 24;
  spec.tick_multiplier = 1;
  spec.full_stack_model = false;
  if (spec.rfm_act_threshold <= 0) {
    spec.rfm_act_threshold = 512;
  }
  if (spec.rfm_decrement <= 0) {
    spec.rfm_decrement = spec.rfm_act_threshold;
  }

  const int speed = spec.data_rate_mbps;
  const bool dvfsl_enabled = spec.lpddr_dvfs_mode != LpddrDvfsMode::Disabled;
  const bool mode_requests_link = mode_contains(spec, "linkprot_on") || mode_contains(spec, "link_on") ||
                                  mode_contains(spec, "link_ecc_on");
  const bool ca_parity_requested = spec.lpddr_ca_parity_enabled ||
                                   mode_contains(spec, "ca_parity_on") ||
                                   mode_contains(spec, "ca_parity");
  const bool link_protection = spec.lpddr_link_protection || spec.lpddr_link_ecc_enabled || mode_requests_link;
  const bool efficiency_mode = spec.lpddr_efficiency_mode != LpddrEfficiencyMode::Normal ||
                               mode_contains(spec, "eff_on") || mode_contains(spec, "efficiency_on") ||
                               mode_contains(spec, "efficiency_enabled");
  const Lpddr6CoreTimingNs core = lpddr6_core_timing_ns(dvfsl_enabled, link_protection, efficiency_mode);

  spec.lpddr_link_protection = link_protection;
  spec.lpddr_link_ecc_enabled = spec.lpddr_link_ecc_enabled || link_protection;
  spec.lpddr_ca_parity_enabled = ca_parity_requested;
  spec.timing.nBL = speed >= 10000 ? 6 : 4;
  spec.timing.nCL = speed >= 10000 ? 62 : (speed >= 8533 ? 54 : 46);
  spec.timing.nCWL = speed >= 10000 ? 26 : (speed >= 8533 ? 22 : 18);
  spec.timing.nRCDRD = jedec::max_ns_or_nck(core.rcd_read, 2, spec.timing.tCK_ps);
  spec.timing.nRCDWR = jedec::max_ns_or_nck(core.rcd_write, 2, spec.timing.tCK_ps);
  const int nACU = lpddr6_nacu_for_speed(speed, dvfsl_enabled);
  spec.timing.nRP = nACU + jedec::max_ns_or_nck(core.rp_pb, 4, spec.timing.tCK_ps);
  spec.timing.nRPab = nACU + jedec::max_ns_or_nck(core.rp_ab, 4, spec.timing.tCK_ps);
  spec.timing.nRAS = jedec::max_ns_or_nck(20.0, 4, spec.timing.tCK_ps);
  spec.timing.nRC = spec.timing.nRAS + spec.timing.nRP;
  spec.timing.nRTP = spec.timing.nBL + jedec::ns_to_nck(core.rtp_tail, spec.timing.tCK_ps);
  spec.timing.nWR = jedec::max_ns_or_nck(core.wtp, 6, spec.timing.tCK_ps);
  spec.timing.nCCDS = speed >= 10000 ? 6 : 4;
  spec.timing.nCCDL = speed >= 10000 ? 10 : 8;
  spec.timing.nRRDS = jedec::max_ns_or_nck(3.75, 4, spec.timing.tCK_ps);
  spec.timing.nRRDL = spec.timing.nRRDS;
  spec.timing.nFAW = 4 * spec.timing.nRRDS;
  spec.timing.nAAD = 8;
  spec.timing.nWCK2CK = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : (speed >= 10000 ? 22 : 18);
  spec.timing.nWCKPST = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : 3;
  spec.timing.nCAS = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : spec.timing.nWCK2CK;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nWTRS = jedec::max_ns_or_nck(core.wtr_s, 6, spec.timing.tCK_ps);
  spec.timing.nWTRL = jedec::max_ns_or_nck(core.wtr_l, 6, spec.timing.tCK_ps);
  spec.timing.nRTW = 16;
  spec.timing.nCCDR = 2;
  spec.timing.nRREFD = jedec::ns_to_nck(7.5, spec.timing.tCK_ps);
  spec.timing.nREFDB2ACT = spec.timing.nRREFD;
  spec.timing.nREFDB2REFDBS = jedec::ns_to_nck(47.0, spec.timing.tCK_ps);
  spec.timing.nREFDB2REFDBL = jedec::ns_to_nck(90.0, spec.timing.tCK_ps);
  const TimingNs trfc_ab = lpddr6_trfcab_ns(spec.density_gb);
  const TimingNs trfc_db = lpddr6_trfcdb_ns(spec.density_gb);
  spec.timing.nRFC = jedec::ns_to_nck(trfc_ab.value, spec.timing.tCK_ps);
  spec.timing.nRFCpb = jedec::ns_to_nck(trfc_db.value, spec.timing.tCK_ps);
  spec.timing.nRFMab = spec.timing.nRFC;
  spec.timing.nRFMpb = spec.timing.nRFCpb;
  spec.timing.nREFI = jedec::us_to_nck(3.906, spec.timing.tCK_ps);
  spec.timing.nREFIpb = jedec::ns_to_nck(spec.refresh_temperature_mode == RefreshTemperatureMode::Normal ? 488.0 : 244.0,
                                         spec.timing.tCK_ps);
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  spec.timing.nWCKSYNC = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : spec.timing.nWCK2CK;
  spec.timing.nWCKTRAIN = spec.lpddr_wck_training_required ? jedec::ns_to_nck(64.0, spec.timing.tCK_ps) : 0;
  spec.timing.nDVFS = spec.lpddr_dvfs_mode == LpddrDvfsMode::Disabled ? 0 : jedec::ns_to_nck(128.0, spec.timing.tCK_ps);
  spec.timing.nPDEX = spec.low_power_exit_cycles > 0 ? spec.low_power_exit_cycles : 8;
  spec.timing.nSREFEX = spec.self_refresh_exit_cycles > 0 ? spec.self_refresh_exit_cycles : 256;
  spec.timing.nSREFEX = std::max(spec.timing.nPDEX, spec.timing.nSREFEX);
  spec.timing.nECCSCRUB = spec.lpddr_link_ecc_enabled ? jedec::ns_to_nck(32.0, spec.timing.tCK_ps) : 0;
  spec.timing.nRASERR = spec.lpddr_link_protection ? jedec::ns_to_nck(24.0, spec.timing.tCK_ps) : 0;
  spec.timing.nLINKRETRY = spec.lpddr_link_protection ? jedec::ns_to_nck(16.0, spec.timing.tCK_ps) : 0;

  if (spec.lpddr_link_protection && spec.lpddr_link_ecc_bits_per_request == 0) {
    spec.lpddr_link_ecc_bits_per_request = 16;
  }
  if (spec.lpddr_dbi_enabled && spec.lpddr_dbi_bits_per_request == 0) {
    spec.lpddr_dbi_bits_per_request = 8;
  }
  if (spec.lpddr_ca_parity_enabled && spec.lpddr_ca_parity_bits_per_command <= 0) {
    spec.lpddr_ca_parity_bits_per_command = 1;
  }
  spec.lpddr_link_protection_mode =
      spec.lpddr_link_protection ?
      (spec.lpddr_ca_parity_enabled ? "link_ecc_crc_retry_ca_parity" : "link_ecc_crc_retry") :
      (spec.lpddr_ca_parity_enabled ? "ca_parity_only" : "off");
  spec.lpddr_dvfs_transition_policy = dvfsl_enabled ? "idle_channel_nacu_guarded" : "disabled";
  spec.lpddr_wck_training_mode =
      spec.lpddr_wck_training_required ? "startup_and_dvfs_retrain" : "cas_sync_only";
  spec.lpddr_low_power_state_policy =
      spec.low_power_mode == LowPowerMode::SelfRefresh ? "self_refresh" :
      (spec.low_power_mode == LowPowerMode::PowerDown ? "power_down" : "controller_idle");

  mark_many(spec,
            {"nBL", "nCL", "nCWL", "nRCDRD", "nRCDWR", "nRP", "nRPab", "nRAS",
             "nRTP", "nWR", "nCCDS", "nCCDL", "nRRDS", "nRRDL", "nAAD",
             "nWCK2CK", "nWCKPST", "nCAS", "nWTRS", "nWTRL", "nRREFD", "nRFC",
             "nREFDB2ACT", "nREFDB2REFDBS", "nREFDB2REFDBL",
             "nRFCpb", "nRFMab", "nRFMpb", "nREFI", "nREFIpb"},
            vendor_calibrated(spec) ? TimingValueSource::Vendor : TimingValueSource::JEDEC,
            "LPDDR6 profile=" + spec.timing_profile + ", mode=" + spec.mode_profile +
                ", vendor=" + spec.vendor_profile +
                ", JESD209-6 Tables 300/301/302/414-423.");
  mark_many(spec,
            {"nMRW", "nMRR", "nWCKSYNC", "nWCKTRAIN", "nDVFS", "nPDEX", "nSREFEX"},
            vendor_calibrated(spec) ? TimingValueSource::Vendor : TimingValueSource::JEDEC,
            "LPDDR6 control/link timing profile for MR programming, WCK training, DVFS and link protection.");
  if (spec.lpddr_link_protection || spec.lpddr_link_ecc_enabled) {
    mark_many(spec, {"nECCSCRUB", "nRASERR", "nLINKRETRY"},
              vendor_calibrated(spec) ? TimingValueSource::Vendor : TimingValueSource::ResearchDefault,
              "LPDDR6 enabled link-protection recovery timing requires a vendor reliability guide.");
  }
  if (!vendor_calibrated(spec) && (!trfc_ab.standard_defined || !trfc_db.standard_defined)) {
    mark_many(spec, {"nRFC", "nRFCpb"}, TimingValueSource::ResearchDefault,
              "LPDDR6 density falls on a JESD209-6 TBD refresh row; using closest defined research value.");
  }
}

void apply_lpddr5_profile(DramSpec& spec) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 6400;
  if (spec.density_gb <= 0) spec.density_gb = 16;

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.name = "LPDDR5";
  spec.data_rate_mbps = spec.speed_bin_mbps;
  // LPDDR5 preset 的 CK:WCK 关系与 HBM/LPDDR6 数据率换算不同；6400Mbps
  // 对应当前模型使用的 1.25ns CK。
  spec.timing.tCK_ps = 8000000.0 / static_cast<double>(spec.data_rate_mbps);
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 4;
  spec.org.banks_per_group = 4;
  spec.org.rows = 1 << 15;
  spec.org.columns = 1 << 10;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 0;
  spec.data_bus_bits = 16;
  spec.internal_prefetch_size = 16;
  spec.tick_multiplier = 1;
  spec.full_stack_model = false;

  spec.timing.nBL = 2;
  spec.timing.nCL = 17;
  spec.timing.nCWL = 9;
  spec.timing.nRCDRD = 15;
  spec.timing.nRCDWR = 15;
  spec.timing.nRP = 15;
  spec.timing.nRAS = 34;
  spec.timing.nRC = 49;
  spec.timing.nRTP = 8;
  spec.timing.nWR = 28;
  spec.timing.nCCDS = 2;
  spec.timing.nCCDL = 4;
  spec.timing.nRRDS = 4;
  spec.timing.nRRDL = 4;
  spec.timing.nFAW = 16;
  spec.timing.nAAD = 8;
  spec.timing.nWCK2CK = 1;
  spec.timing.nWCKPST = 8;
  spec.timing.nCAS = 0;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nRPab = 17;
  spec.timing.nWTRS = 5;
  spec.timing.nWTRL = 10;
  spec.timing.nRTW = 16;
  spec.timing.nCCDR = 2;
  spec.timing.nRFC = 224;
  spec.timing.nRFCpb = 128;
  spec.timing.nRFMab = 0;
  spec.timing.nRFMpb = 0;
  spec.timing.nRREFD = 0;
  spec.timing.nREFDB2ACT = 0;
  spec.timing.nREFDB2REFDBS = 0;
  spec.timing.nREFDB2REFDBL = 0;
  spec.timing.nREFI = 3125;
  spec.timing.nREFIpb = 390;
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  spec.timing.nWCKSYNC = spec.timing.nWCK2CK;
  spec.timing.nWCKTRAIN = 0;
  spec.timing.nDVFS = 0;
  spec.timing.nPDEX = 8;
  spec.timing.nSREFEX = 256;
  spec.timing.nECCSCRUB = 64;
  spec.timing.nRASERR = 64;
  spec.timing.nLINKRETRY = 16;

  mark_many(spec,
            {"nBL", "nCCDS", "nCCDL", "nRRDS", "nRRDL", "nAAD",
             "nWCK2CK", "nWCKPST", "nCAS", "nCS", "nPPD", "nREFI", "nREFIpb"},
            TimingValueSource::JEDEC,
            "LPDDR5 generic profile; select a vendor profile for device-specific RL/WL and row timing.");
  if (vendor_calibrated(spec)) {
    mark_many(spec,
              {"nCL", "nCWL", "nRCDRD", "nRCDWR", "nRP", "nRPab", "nRAS", "nRC",
               "nRTP", "nWR", "nWTRS", "nWTRL", "nRFC", "nRFCpb"},
              TimingValueSource::Vendor,
              "LPDDR5 vendor profile " + spec.vendor_profile);
  }
}

}  // namespace

void apply_standard_timing_profile(DramSpec& spec) {
  // 重新应用 profile 时丢弃上一轮 profile/external-file 的来源标记，避免
  // vendor -> generic 切换后遗留 Vendor 标签。配置的逐项 source override
  // 总是在本函数之后应用。
  spec.timing_source_overrides.clear();

  switch (spec.standard) {
    case DramStandard::Hbm4:
      apply_hbm4_profile(spec);
      break;
    case DramStandard::Hbm3:
      apply_hbm3_profile(spec);
      break;
    case DramStandard::Lpddr6:
      apply_lpddr6_profile(spec);
      break;
    case DramStandard::Lpddr5:
      apply_lpddr5_profile(spec);
      break;
    case DramStandard::Unknown:
      throw std::invalid_argument("cannot apply timing profile without standard traits");
  }

  apply_external_timing_profile_file(spec);
}

}  // namespace hbm_sim
