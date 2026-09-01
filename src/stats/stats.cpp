// Stats 输出层：把仿真统计按稳定 key:value 顺序打印出来。
// 字段名和顺序尽量保持向后兼容，方便 shell/Python/表格工具做批量后处理。
#include "hbm_sim/stats/stats.hpp"

#include <iomanip>
#include <ostream>

namespace hbm_sim {
namespace {

template <typename T>
void print_field(std::ostream& os, const char* key, const T& value) {
  os << std::left << std::setw(kOutputKeyWidth) << key << ": " << std::right << value << '\n';
}

void print_section(std::ostream& os, int index, const char* title) {
  os << "\n# ===== " << std::setfill('0') << std::setw(2) << index << ' '
     << title << " =====\n" << std::setfill(' ');
}

}  // namespace

void print_stats(std::ostream& os, const Stats& stats) {
  // 输出顺序保持稳定，方便 tests/smoke.sh、diff 和外部脚本解析。
  // 如果新增字段，建议只追加到相关分组末尾，避免破坏已有脚本按行读取。
  //
  // 分组顺序大致为：
  // 1. workload completion 和 row-buffer 行为；
  // 2. DRAM 命令计数；
  // 3. refresh/RFM/总线/调度事件；
  // 4. system/controller 分层统计；
  // 5. 数据和接口带宽；
  // 6. 平均延迟、平均队列长度和每 cycle 字节数。
  print_section(os, 7, "CONTROLLER AND ROW BEHAVIOR");
  print_field(os, "cycles", stats.cycles);
  print_field(os, "reads", stats.reads);
  print_field(os, "writes", stats.writes);
  print_field(os, "completed_reads", stats.completed_reads);
  print_field(os, "completed_writes", stats.completed_writes);
  print_field(os, "row_hits", stats.row_hits);
  print_field(os, "row_misses", stats.row_misses);
  print_field(os, "row_conflicts", stats.row_conflicts);
  print_section(os, 8, "COMMANDS AND MAINTENANCE");
  print_field(os, "commands.ACT", stats.act);
  print_field(os, "commands.ACT1", stats.act1);
  print_field(os, "commands.ACT2", stats.act2);
  print_field(os, "commands.PRE", stats.pre);
  print_field(os, "commands.PREpb", stats.prepb);
  print_field(os, "commands.PREab", stats.preab);
  print_field(os, "commands.CAS_RD", stats.cas_rd);
  print_field(os, "commands.CAS_WR", stats.cas_wr);
  print_field(os, "commands.RD", stats.rd);
  print_field(os, "commands.WR", stats.wr);
  print_field(os, "commands.RDA", stats.rda);
  print_field(os, "commands.WRA", stats.wra);
  print_field(os, "commands.REFab", stats.refab);
  print_field(os, "commands.REFpb", stats.refpb);
  print_field(os, "commands.REFdb", stats.refdb);
  print_field(os, "commands.RFMab", stats.rfmab);
  print_field(os, "commands.RFMpb", stats.rfmpb);
  print_field(os, "commands.MRW", stats.mrw);
  print_field(os, "commands.MRR", stats.mrr);
  print_field(os, "commands.WCK_SYNC", stats.wck_sync);
  print_field(os, "commands.WCK_TRAIN", stats.wck_train);
  print_field(os, "commands.DVFS", stats.dvfs);
  print_field(os, "commands.PDE", stats.pde);
  print_field(os, "commands.PDX", stats.pdx);
  print_field(os, "commands.SREFEN", stats.srefen);
  print_field(os, "commands.SREFEX", stats.srefex);
  print_field(os, "commands.ECC_SCRUB", stats.ecc_scrub);
  print_field(os, "commands.RAS_ERR", stats.ras_err);
  print_field(os, "maintenance_requests", stats.maintenance_requests);
  print_field(os, "maintenance_served", stats.maintenance_served);
  print_field(os, "refresh_batches", stats.refresh_batches);
  print_field(os, "refresh_pb_batches", stats.refresh_per_bank_batches);
  print_field(os, "refresh_ab_batches", stats.refresh_all_bank_batches);
  print_field(os, "refresh_postpones", stats.refresh_postpones);
  print_field(os, "refresh_pullins", stats.refresh_pullins);
  print_field(os, "refresh_credit_peak", stats.refresh_credit_peak);
  print_field(os, "rfm_events", stats.rfm_events);
  print_field(os, "rfm_pb_events", stats.rfm_per_bank_events);
  print_field(os, "rfm_ab_events", stats.rfm_all_bank_events);
  print_field(os, "dual_issue_cycles", stats.dual_issue_cycles);
  print_field(os, "row_bus_issues", stats.row_bus_issues);
  print_field(os, "column_bus_issues", stats.column_bus_issues);
  print_field(os, "unified_bus_issues", stats.unified_bus_issues);
  print_field(os, "rising_edge_ticks", stats.rising_edge_ticks);
  print_field(os, "falling_edge_ticks", stats.falling_edge_ticks);
  print_field(os, "write_mode_cycles", stats.write_mode_cycles);
  print_section(os, 9, "DATA, STORAGE AND INTEGRITY");
  print_field(os, "host_requests", stats.host_requests);
  print_field(os, "dram_transactions", stats.dram_transactions);
  print_field(os, "injected_requests", stats.injected_requests);
  print_field(os, "injection_stall_cycles", stats.injection_stall_cycles);
  print_field(os, "read_forwards", stats.read_forwards);
  print_field(os, "write_coalesces", stats.write_coalesces);
  print_field(os, "data_checked_reads", stats.data_checked_reads);
  print_field(os, "data_mismatches", stats.data_mismatches);
  print_field(os, "data_uninitialized_reads", stats.data_uninitialized_reads);
  print_field(os, "data_write_commits", stats.data_write_commits);
  print_field(os, "data_masked_write_commits", stats.data_masked_write_commits);
  print_field(os, "data_forward_checks", stats.data_forward_checks);
  print_field(os, "storage_lines_allocated", stats.storage_lines_allocated);
  print_field(os, "unique_written_lines", stats.unique_written_lines);
  print_field(os, "storage_bytes_allocated", stats.storage_bytes_allocated);
  print_field(os, "storage_topology_lines_scanned", stats.storage_topology_lines_scanned);
  print_field(os, "storage_topology_scan_skipped", stats.storage_topology_scan_skipped);
  print_field(os, "total_addressable_lines", stats.total_addressable_lines);
  print_field(os, "storage_density_pct", stats.storage_density_pct);
  print_field(os, "burst_trace_lines", stats.burst_trace_lines);
  print_field(os, "burst_split_requests", stats.burst_split_requests);
  print_field(os, "burst_read_requests", stats.burst_read_requests);
  print_field(os, "burst_write_requests", stats.burst_write_requests);
  print_field(os, "burst_read_bytes", stats.burst_read_bytes);
  print_field(os, "burst_write_bytes", stats.burst_write_bytes);
  print_field(os, "storage_stacks_touched", stats.storage_stacks_touched);
  print_field(os, "storage_dies_touched", stats.storage_dies_touched);
  print_field(os, "storage_layers_touched", stats.storage_layers_touched);
  print_field(os, "storage_channels_touched", stats.storage_channels_touched);
  print_field(os, "storage_pcs_touched", stats.storage_pseudo_channels_touched);
  print_field(os, "storage_sids_touched", stats.storage_sids_touched);
  print_field(os, "storage_ranks_touched", stats.storage_ranks_touched);
  print_field(os, "storage_bgs_touched", stats.storage_bank_groups_touched);
  print_field(os, "storage_banks_touched", stats.storage_banks_touched);
  print_field(os, "storage_rows_touched", stats.storage_rows_touched);
  print_field(os, "storage_columns_touched", stats.storage_columns_touched);
  print_field(os, "storage_subarrays_touched", stats.storage_subarrays_touched);
  print_field(os, "storage_mats_touched", stats.storage_mats_touched);
  print_field(os, "storage_cells_touched", stats.storage_cells_touched);
  print_field(os, "storage_microbumps_touched", stats.storage_microbumps_touched);
  print_field(os, "floorplan_tiles_touched", stats.floorplan_tiles_touched);
  print_field(os, "thermal_tiles_touched", stats.thermal_tiles_touched);
  print_field(os, "thermal_grid_cells_touched", stats.thermal_grid_cells_touched);
  print_field(os, "storage_read_line_accesses", stats.storage_read_line_accesses);
  print_field(os, "storage_write_line_accesses", stats.storage_write_line_accesses);
  print_field(os, "rowbuf_activations", stats.rowbuf_activations);
  print_field(os, "rowbuf_precharges", stats.rowbuf_precharges);
  print_field(os, "rowbuf_dirty_writebacks", stats.rowbuf_dirty_writebacks);
  print_field(os, "rowbuf_clean_precharges", stats.rowbuf_clean_precharges);
  print_field(os, "rowbuf_hits", stats.rowbuf_hits);
  print_field(os, "rowbuf_misses", stats.rowbuf_misses);
  print_field(os, "rowbuf_lazy_loads", stats.rowbuf_lazy_loads);
  print_field(os, "rowbuf_reads", stats.rowbuf_reads);
  print_field(os, "rowbuf_writes", stats.rowbuf_writes);
  print_field(os, "rowbuf_forced_closes", stats.rowbuf_forced_closes);
  print_field(os, "rowbuf_open_rows", stats.rowbuf_open_rows);
  print_field(os, "rowbuf_dirty_rows", stats.rowbuf_dirty_rows);
  print_section(os, 10, "POWER AND THERMAL");
  print_field(os, "power_events", stats.power_events);
  print_field(os, "thermal_updates", stats.thermal_updates);
  os << std::fixed << std::setprecision(2);
  print_field(os, "power_energy_pJ", stats.power_energy_pj);
  print_field(os, "power_act_energy_pJ", stats.power_act_energy_pj);
  print_field(os, "power_pre_energy_pJ", stats.power_pre_energy_pj);
  print_field(os, "power_read_energy_pJ", stats.power_read_energy_pj);
  print_field(os, "power_write_energy_pJ", stats.power_write_energy_pj);
  print_field(os, "power_refresh_energy_pJ", stats.power_refresh_energy_pj);
  print_field(os, "power_rfm_energy_pJ", stats.power_rfm_energy_pj);
  print_field(os, "power_control_energy_pJ", stats.power_control_energy_pj);
  print_field(os, "thermal_peak_temp_C", stats.thermal_peak_temp_c);
  print_field(os, "thermal_avg_temp_C", stats.thermal_avg_temp_c);
  print_field(os, "thermal_hotspot_layer", stats.thermal_hotspot_layer);
  print_field(os, "thermal_hotspot_x", stats.thermal_hotspot_x);
  print_field(os, "thermal_hotspot_y", stats.thermal_hotspot_y);
  print_field(os, "thermal_lateral_transfers", stats.thermal_lateral_transfers);
  print_field(os, "thermal_vertical_transfers", stats.thermal_vertical_transfers);
  print_field(os, "thermal_tsv_transfers", stats.thermal_tsv_transfers);
  print_field(os, "thermal_coupled_delta_C", stats.thermal_coupled_delta_c);
  print_field(os, "ecc_shadow_updates", stats.ecc_shadow_updates);
  print_field(os, "ecc_checked_reads", stats.ecc_checked_reads);
  print_field(os, "ecc_corrected_errors", stats.ecc_corrected_errors);
  print_field(os, "ecc_uncorrectable_errors", stats.ecc_uncorrectable_errors);
  print_field(os, "ecc_injected_errors", stats.ecc_injected_errors);
  print_field(os, "ecc_parity_repairs", stats.ecc_parity_repairs);
  print_section(os, 11, "PHY AND DFI");
  print_field(os, "dfi_read_beats", stats.dfi_read_beats);
  print_field(os, "dfi_write_beats", stats.dfi_write_beats);
  print_field(os, "dfi_forwarded_read_beats", stats.dfi_forwarded_read_beats);
  print_field(os, "dfi_masked_write_beats", stats.dfi_masked_write_beats);
  print_field(os, "dfi_data_bytes", stats.dfi_data_bytes);
  print_field(os, "dfi_beat_bytes", stats.dfi_beat_bytes);
  print_field(os, "phy_commands", stats.phy_commands);
  print_field(os, "phy_read_requests", stats.phy_read_requests);
  print_field(os, "phy_write_requests", stats.phy_write_requests);
  print_field(os, "phy_read_completions", stats.phy_read_completions);
  print_field(os, "phy_write_completions", stats.phy_write_completions);
  print_field(os, "phy_command_backpressure", stats.phy_command_backpressure);
  print_field(os, "phy_data_backpressure", stats.phy_data_backpressure);
  print_field(os, "phy_reset_cycles", stats.phy_reset_cycles);
  print_field(os, "phy_initialization_cycles", stats.phy_initialization_cycles);
  print_field(os, "phy_training_cycles", stats.phy_training_cycles);
  print_field(os, "phy_ca_edges", stats.phy_ca_edges);
  print_field(os, "phy_hbm_row_commands", stats.phy_hbm_row_commands);
  print_field(os, "phy_hbm_column_commands", stats.phy_hbm_column_commands);
  print_field(os, "phy_lpddr_wck_events", stats.phy_lpddr_wck_events);
  print_field(os, "phy_lpddr_split_act", stats.phy_lpddr_split_act_events);
  print_field(os, "phy_max_command_fifo", stats.phy_max_command_fifo);
  print_field(os, "phy_max_read_fifo", stats.phy_max_read_fifo);
  print_field(os, "phy_max_write_fifo", stats.phy_max_write_fifo);
  print_field(os, "phy_read_service_cycles", stats.phy_total_read_service_cycles);
  print_field(os, "phy_write_service_cycles", stats.phy_total_write_service_cycles);
  print_field(os, "row_policy_ap_upgrades", stats.row_policy_ap_upgrades);
  print_field(os, "row_policy_precharges", stats.row_policy_precharges);
  print_field(os, "wck_syncs", stats.wck_syncs);
  print_field(os, "wck_sync_skips", stats.wck_sync_skips);
  print_field(os, "wck_training_events", stats.wck_training_events);
  print_field(os, "mode_register_ops", stats.mode_register_ops);
  print_field(os, "dvfs_transitions", stats.dvfs_transitions);
  print_field(os, "ras_ecc_events", stats.ras_ecc_events);
  print_field(os, "act2_deadline_forced", stats.act2_deadline_forced);
  print_field(os, "rfm_decrements", stats.rfm_decrements);
  print_field(os, "low_power_entries", stats.low_power_entries);
  print_field(os, "low_power_exits", stats.low_power_exits);
  print_field(os, "low_power_cycles", stats.low_power_cycles);
  print_field(os, "low_power_exit_blocked", stats.low_power_exit_blocked_cycles);
  print_field(os, "interface_command_bits", stats.interface_command_bits);
  print_field(os, "interface_overhead_bits", stats.interface_overhead_bits);
  print_section(os, 12, "SYSTEM COMPLETION AND KEY PERFORMANCE");
  print_field(os, "controller_count", stats.controller_count);
  print_field(os, "active_controllers", stats.active_controllers);
  print_field(os, "stack_count", stats.stack_count);
  print_field(os, "active_stacks", stats.active_stacks);
  print_field(os, "stack_ingress_stalls", stats.stack_ingress_stall_cycles);
  print_field(os, "stack_ingress_peak", stats.stack_ingress_peak);
  print_field(os, "qos_priority_dispatches", stats.qos_priority_dispatches);
  print_field(os, "system_cycles", stats.system_cycles);
  print_field(os, "aggregate_ctrl_cycles", stats.aggregate_controller_cycles);
  print_field(os, "read_bytes", stats.read_bytes);
  print_field(os, "write_bytes", stats.write_bytes);
  print_field(os, "interface_read_bytes", stats.interface_read_bytes);
  print_field(os, "interface_write_bytes", stats.interface_write_bytes);
  os << std::fixed << std::setprecision(3);
  // interface_transfer_rate_gbps 是 pin/link 速率；peak_bandwidth_GBps 是速率乘
  // data_bus_bits 的理论峰值；achieved_* 则来自实际完成请求数。
  print_field(os, "if_xfer_rate_Gbps", stats.interface_transfer_rate_gbps);
  print_field(os, "peak_bandwidth_GBps", stats.peak_bandwidth_GBps);
  print_field(os, "achieved_bw_GBps", stats.achieved_bandwidth_GBps);
  print_field(os, "achieved_if_bw_GBps", stats.achieved_interface_bandwidth_GBps);
  os << std::fixed << std::setprecision(2);
  print_field(os, "bandwidth_util_pct", stats.bandwidth_utilization);
  print_field(os, "payload_efficiency_pct", stats.payload_efficiency);
  // remaining_* 和 hit_cycle_limit 是实验有效性的保护字段。只看 completed_reads
  // 容易忽略 max_cycles 到期导致的未完成请求。
  print_field(os, "remaining_requests", stats.remaining_requests);
  print_field(os, "remaining_pending", stats.remaining_pending);
  print_field(os, "hit_cycle_limit", stats.hit_cycle_limit ? "true" : "false");
  // 浮点指标统一固定小数位，方便不同平台输出保持可比较。
  os << std::fixed << std::setprecision(2);
  print_field(os, "avg_read_latency", stats.avg_read_latency());
  print_field(os, "read_queue_len_avg", stats.read_queue_len_avg());
  print_field(os, "write_queue_len_avg", stats.write_queue_len_avg());
  print_field(os, "priority_queue_avg", stats.priority_queue_len_avg());
  print_field(os, "active_queue_len_avg", stats.active_queue_len_avg());
  print_field(os, "read_q_avg_per_ctrl", stats.read_queue_len_avg_per_controller());
  print_field(os, "write_q_avg_per_ctrl", stats.write_queue_len_avg_per_controller());
  print_field(os, "priority_q_avg_per_ctrl", stats.priority_queue_len_avg_per_controller());
  print_field(os, "active_q_avg_per_ctrl", stats.active_queue_len_avg_per_controller());
  os << std::fixed << std::setprecision(4);
  print_field(os, "read_bytes_per_cycle", stats.read_bytes_per_cycle());
  print_field(os, "write_bytes_per_cycle", stats.write_bytes_per_cycle());
  print_field(os, "total_bytes_per_cycle", stats.total_bytes_per_cycle());
}

}  // namespace hbm_sim
