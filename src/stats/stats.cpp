// Public results and a bounded set of useful diagnostic counters.
#include "hbm_sim/stats/stats.hpp"
#include "hbm_sim/stats/result.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/core/utils.hpp"

namespace hbm_sim {

void finalize_bandwidth_stats(Stats& s, double cycles_per_second,
                              double peak_bandwidth_GBps, bool per_stack) {
  s.peak_bandwidth_GBps = peak_bandwidth_GBps;
  const bool elapsed = s.cycles > 0 && cycles_per_second > 0.0;
  if (elapsed) {
    const double bytes = static_cast<double>(s.read_bytes + s.write_bytes);
    const double interface_bytes = static_cast<double>(
        s.interface_read_bytes + s.interface_write_bytes +
        rounded_bytes_from_bits(s.interface_command_bits));
    const double seconds = static_cast<double>(s.cycles) / cycles_per_second;
    // Preserve the original per-stack division and Controller/system guards,
    // including their floating-point evaluation order.
    s.achieved_bandwidth_GBps = !per_stack && seconds <= 0.0
                                  ? 0.0 : bytes / seconds / 1.0e9;
    s.achieved_interface_bandwidth_GBps = !per_stack && seconds <= 0.0
        ? 0.0 : interface_bytes / seconds / 1.0e9;
  }
  s.bandwidth_utilization = s.peak_bandwidth_GBps <= 0.0
      ? 0.0 : 100.0 * s.achieved_bandwidth_GBps / s.peak_bandwidth_GBps;
  if (!per_stack || elapsed) {
    const auto interface_bytes = s.interface_read_bytes + s.interface_write_bytes +
                                rounded_bytes_from_bits(s.interface_command_bits);
    s.payload_efficiency = interface_bytes == 0 ? 100.0
        : 100.0 * static_cast<double>(s.read_bytes + s.write_bytes) /
              static_cast<double>(interface_bytes);
  }
}

ResultFields collect_metrics(const Stats& s, double tick_ns, bool power, bool thermal) {
  ResultFields f;
  auto put = [&](const char* key, auto value) { record_field(f, key, value); };
  put("host_requests", s.host_requests);
  put("dram_transactions", s.dram_transactions);
  put("completed_reads", s.completed_reads);
  put("completed_writes", s.completed_writes);
  put("remaining_requests", s.remaining_requests);
  put("remaining_pending", s.remaining_pending);
  put("hit_cycle_limit", s.hit_cycle_limit);
  put("system_cycles", s.system_cycles);
  put("simulation_time_ns", s.system_cycles * tick_ns);
  put("read_bytes", s.read_bytes);
  put("write_bytes", s.write_bytes);
  put("interface_read_bytes", s.interface_read_bytes);
  put("interface_write_bytes", s.interface_write_bytes);
  put("achieved_bw_GBps", s.achieved_bandwidth_GBps);
  put("achieved_if_bw_GBps", s.achieved_interface_bandwidth_GBps);
  put("peak_bandwidth_GBps", s.peak_bandwidth_GBps);
  put("bandwidth_util_pct", s.bandwidth_utilization);
  put("payload_efficiency_pct", s.payload_efficiency);
  f["avg_read_latency_ns"] = s.completed_reads
      ? ResultValue(s.avg_read_latency() * tick_ns) : ResultValue(nullptr);
  f["avg_write_latency_ns"] = s.completed_writes
      ? ResultValue(s.avg_write_latency() * tick_ns) : ResultValue(nullptr);
  const double classified = static_cast<double>(s.row_hits) + s.row_misses + s.row_conflicts;
  f["row_hit_pct"] = classified ? ResultValue(100.0 * s.row_hits / classified) : ResultValue(nullptr);
  put("read_q_avg_per_ctrl", s.read_queue_len_avg_per_controller());
  put("write_q_avg_per_ctrl", s.write_queue_len_avg_per_controller());
  put("injection_stall_cycles", s.injection_stall_cycles);
  put("phy_command_backpressure", s.phy_command_backpressure);
  put("phy_data_backpressure", s.phy_data_backpressure);
  put("storage_lines_allocated", s.storage_lines_allocated);
  put("storage_bytes_allocated", s.storage_bytes_allocated);
  if (power) put("power_energy_pJ", s.power_energy_pj);
  if (thermal) {
    put("thermal_avg_temp_C", s.thermal_avg_temp_c);
    put("thermal_peak_temp_C", s.thermal_peak_temp_c);
  }
  if (s.self_refresh_cycles) {
    put("self_refresh_cycles", s.self_refresh_cycles);
    put("self_refresh_time_ns", s.self_refresh_time_ns);
    if (power) put("self_refresh_energy_pJ", s.self_refresh_energy_pj);
  }
  return f;
}

ResultFields collect_diagnostics(const Stats& s, const DramSpec& spec,
                                 const StorageModelOptions& storage) {
  ResultFields f;
  auto put = [&](const char* key, auto value) { record_field(f, key, value); };
  put("row_hits", s.row_hits);
  put("row_misses", s.row_misses);
  put("row_conflicts", s.row_conflicts);
  put("aggregate_ctrl_cycles", s.aggregate_controller_cycles);
  put("controller_count", s.controller_count);
  put("active_controllers", s.active_controllers);
  put("active_stacks", s.active_stacks);
  put("stack_ingress_stalls", s.stack_ingress_stall_cycles);
  put("dispatch_budget_exhausted", s.dispatch_budget_exhausted);
  put("controller_refused_dispatches", s.controller_refused_dispatches);
  put("qos_priority_dispatches", s.qos_priority_dispatches);
  put("priority_q_avg_per_ctrl", s.priority_queue_len_avg_per_controller());
  put("active_q_avg_per_ctrl", s.active_queue_len_avg_per_controller());
  put("read_forwards", s.read_forwards);
  put("write_coalesces", s.write_coalesces);
  put("data_write_commits", s.data_write_commits);
  put("data_uninitialized_reads", s.data_uninitialized_reads);
  put("unique_written_lines", s.unique_written_lines);
  put("storage_topology_scan_skipped", s.storage_topology_scan_skipped);
  put("storage_layers_touched", s.storage_layers_touched);
  put("storage_banks_touched", s.storage_banks_touched);
  put("storage_subarrays_touched", s.storage_subarrays_touched);
  put("storage_cells_touched", s.storage_cells_touched);
  put("rowbuf_dirty_writebacks", s.rowbuf_dirty_writebacks);
  put("rowbuf_hits", s.rowbuf_hits);
  put("rowbuf_misses", s.rowbuf_misses);
  put("burst_trace_lines", s.burst_trace_lines);
  put("burst_split_requests", s.burst_split_requests);
  put("burst_read_requests", s.burst_read_requests);
  put("burst_write_requests", s.burst_write_requests);
  put("dfi_read_beats", s.dfi_read_beats);
  put("dfi_write_beats", s.dfi_write_beats);
  put("dfi_data_bytes", s.dfi_data_bytes);
  if (spec.supports_refresh) {
    put("refresh_pb_batches", s.refresh_per_bank_batches);
    put("refresh_ab_batches", s.refresh_all_bank_batches);
  }
  if (spec.supports_rfm) put("rfm_events", s.rfm_events);
  if (storage.ecc_shadow_enabled) {
    put("ecc_checked_reads", s.ecc_checked_reads);
    put("ecc_corrected_errors", s.ecc_corrected_errors);
  }
  if (storage.power_enabled) {
    put("power_events", s.power_events);
    put("power_act_energy_pJ", s.power_act_energy_pj);
    put("power_pre_energy_pJ", s.power_pre_energy_pj);
    put("power_read_energy_pJ", s.power_read_energy_pj);
    put("power_write_energy_pJ", s.power_write_energy_pj);
    put("power_refresh_energy_pJ", s.power_refresh_energy_pj);
    put("power_rfm_energy_pJ", s.power_rfm_energy_pj);
  }
  if (storage.thermal_enabled) {
    put("thermal_updates", s.thermal_updates);
    put("thermal_grid_cells_touched", s.thermal_grid_cells_touched);
    put("thermal_vertical_transfers", s.thermal_vertical_transfers);
    put("thermal_tsv_transfers", s.thermal_tsv_transfers);
  }
  // Command counts are workload observations; unobserved commands are omitted.
  for (auto [name, count] : {
      std::pair{"ACT", s.act}, {"ACT1", s.act1}, {"ACT2", s.act2},
      {"RD", s.rd}, {"WR", s.wr}, {"RDA", s.rda}, {"WRA", s.wra},
      {"PRE", s.pre}, {"PREpb", s.prepb}, {"PREab", s.preab}, {"REFpb", s.refpb},
      {"REFab", s.refab}, {"REFdb", s.refdb}, {"RFMpb", s.rfmpb}, {"RFMab", s.rfmab},
      {"CAS_RD", s.cas_rd}, {"CAS_WR", s.cas_wr}, {"MRW", s.mrw}, {"MRR", s.mrr},
      {"WCK_TRAIN", s.wck_train}, {"WCK_SYNC", s.wck_sync}, {"DVFS", s.dvfs},
      {"ECC_SCRUB", s.ecc_scrub}, {"RAS_ERR", s.ras_err},
      {"PDE", s.pde}, {"PDX", s.pdx}, {"SREFEN", s.srefen}, {"SREFEX", s.srefex}}) {
    if (count) record_field(f, std::string("commands.") + name, count);
  }
  put("maintenance_served", s.maintenance_served);
  put("phy_read_requests", s.phy_read_requests);
  put("phy_write_requests", s.phy_write_requests);
  put("phy_read_completions", s.phy_read_completions);
  put("phy_write_completions", s.phy_write_completions);
  put("phy_max_command_fifo", s.phy_max_command_fifo);
  put("phy_max_read_fifo", s.phy_max_read_fifo);
  put("phy_max_write_fifo", s.phy_max_write_fifo);
  return f;
}
}  // namespace hbm_sim
