// CommandExecutor：执行一条已经通过调度和 timing gate 的 DRAM 命令。
// 这里集中更新 bank-local 状态、scope timing、WCK 窗口、RFM 计数和命令统计，
// 对应 Ramulator command lambda / state transition 的职责。
#include "hbm_sim/controller/executor.hpp"

#include <algorithm>

namespace hbm_sim {

CommandExecutor::CommandExecutor(const DramSpec& spec,
                                 std::vector<BankState>& banks,
                                 TimingEngine& timing_engine,
                                 RfmManager& rfm_manager,
                                 Stats& stats)
    : spec_(spec),
      banks_(banks),
      timing_engine_(timing_engine),
      rfm_manager_(rfm_manager),
      stats_(stats) {}

CommandExecutionResult CommandExecutor::issue(Request& req, Command issued, Cycle clk) {
  CommandExecutionResult result;
  // Executor 入口只接收已经通过 state_ok/timing_ok/bus_matches 的命令。
  // 因此这里不再做合法性判断，而是专注于副作用：bank 状态、scope gate、统计和 RFM。
  BankState& bank = banks_[req.decoded.flat_bank(spec_)];
  TimingScopeState& row = timing_engine_.row_state(spec_, req.decoded);
  TimingScopeState& col = timing_engine_.column_state(spec_, req.decoded);
  TimingScopeState& bg = timing_engine_.bank_group_state(spec_, req.decoded);
  TimingScopeState& wck = timing_engine_.wck_state(spec_, req.decoded);
  const Timing& t = spec_.timing;

  switch (issued) {
      case Command::ACT:
        // HBM-like 单阶段 activate：直接打开目标 row，并设置后续 RD/WR/PRE/ACT gate。
        bank.open_row = req.decoded.row;
      // next_rd/next_wr 是 bank-local nRCD gate；row/bank-group scope 的 nRRD
      // 则写到 row.next_act/bg.next_act 和 TimingEngine constraint 中。
        bank.next_rd = clk + timing_delay(t.nRCDRD);
      bank.next_wr = clk + timing_delay(t.nRCDWR);
      bank.next_pre = clk + timing_delay(t.nRAS);
      bank.next_act = clk + timing_delay(t.nRC);
      row.next_row = clk + 1;
      row.next_act = clk + timing_delay(t.nRRDS);
      bg.next_act = clk + timing_delay(t.nRRDL);
      timing_engine_.record_activate(spec_, req.decoded, clk);
      result.rfm_command = rfm_manager_.on_activate(spec_, req.decoded, stats_);
      stats_.act++;
      break;
      case Command::ACT1:
        // LPDDR split activate 第一阶段：bank 进入 activating，row 尚不可访问。
        // owning request 必须后续发 ACT2；Controller 用 req.issued_first_activate 追踪所有权。
        req.issued_first_activate = true;
      bank.activating = true;
        bank.next_act2 = clk + timing_delay(std::max(1, t.nAAD));
        bank.act2_deadline = bank.next_act2;
      // 当前模型把 ACT2 deadline 设为最早 ACT2 时间：一旦 ready 就强制优先服务。
      // 这比标准更保守，但能避免 split-activate 请求在 active_buffer 中被饿死。
        bank.next_act = clk + timing_delay(t.nRC);
      bank.next_pre = clk + timing_delay(t.nRAS);
      row.next_row = clk + 1;
      row.next_act = clk + timing_delay(t.nRRDS);
      bg.next_act = clk + timing_delay(t.nRRDL);
      timing_engine_.record_activate(spec_, req.decoded, clk);
      result.rfm_command = rfm_manager_.on_activate(spec_, req.decoded, stats_);
      stats_.act1++;
      break;
      case Command::ACT2: {
        // LPDDR split activate 第二阶段：完成 row 打开。nRCD 的剩余部分从 ACT2 后继续计。
        bank.activating = false;
        bank.open_row = req.decoded.row;
        bank.act2_deadline = 0;
      // nRCD 在标准语义上从 activate 序列起点约束到 RD/WR。这里将 nAAD 已消耗的
      // 部分扣掉，得到 ACT2 后还需要等待的最小周期数。
        int remaining_rd = std::max(1, t.nRCDRD - t.nAAD);
      int remaining_wr = std::max(1, t.nRCDWR - t.nAAD);
      bank.next_rd = clk + timing_delay(remaining_rd);
      bank.next_wr = clk + timing_delay(remaining_wr);
      row.next_row = clk + 1;
      stats_.act2++;
      break;
    }
      case Command::PRE:
      case Command::PREPB:
      // PREpb 关闭目标 bank，并用 nRP 推迟下一次 ACT。open_row 清零后，
      // row policy 和 maintenance manager 都会把该 bank 视为 idle。
        bank.open_row = -1;
      bank.activating = false;
      bank.next_act = clk + timing_delay(t.nRP);
      row.next_row = clk + 1;
      if (issued == Command::PREPB) {
        stats_.prepb++;
      } else {
        stats_.pre++;
      }
      break;
    case Command::CASRD:
      // CAS_RD/CAS_WR 不搬运数据，只建立 WCK2CK 同步窗口。
      // 后续 RD/WR 必须落在 wck_ready_at 和 wck_active_until 之间。
      col.next_col = clk + 1;
      wck.next_cas = clk + 1;
      wck.wck_ready_at = clk + timing_delay(std::max(1, t.nWCK2CK));
      wck.wck_active_until = clk + timing_delay(std::max(t.nWCKPST, t.nWCK2CK + 1));
      req.cas_sync_issued = true;
      stats_.cas_rd++;
      stats_.wck_syncs++;
      break;
      case Command::CASWR:
      // CAS_WR 和 CAS_RD 的状态更新完全对称，只是统计字段不同。二者都建立
      // WCK ready window，后续真正的数据命令还要通过 wck_ready_for_data() 检查。
        col.next_col = clk + 1;
      wck.next_cas = clk + 1;
      wck.wck_ready_at = clk + timing_delay(std::max(1, t.nWCK2CK));
      wck.wck_active_until = clk + timing_delay(std::max(t.nWCKPST, t.nWCK2CK + 1));
      req.cas_sync_issued = true;
      stats_.cas_wr++;
      stats_.wck_syncs++;
      break;
      case Command::RD:
      case Command::RDA:
      // RDA/WRA 是 auto-precharge 数据命令：数据命令发出后立即把 row 视为关闭，
      // 但下一次 ACT 仍要等到读恢复/写恢复和 nRP 路径结束。
        bank.next_any_col = clk + timing_delay(t.nCCDL);
      // bank.next_any_col 是同 bank 内保守列间隔；跨 bank-group 和 PC/SID 的列间隔
      // 还会通过 col/bg scope 与 timing_constraints 叠加。
        bank.next_rd = clk + timing_delay(t.nCCDL);
      bank.next_pre = std::max(bank.next_pre, clk + timing_delay(t.nRTP));
      if (issued == Command::RDA) {
        Cycle pre_at = clk + timing_delay(t.nRTP);
        bank.open_row = -1;
        bank.next_act = std::max(bank.next_act, pre_at + timing_delay(t.nRP));
      }
      col.next_col = clk + timing_delay(t.nCCDS);
      bg.next_col = clk + timing_delay(t.nCCDL);
        if (spec_.lpddr_family) {
          if (!req.cas_sync_issued) {
          // 正常 LPDDR 路径应先发 CAS_RD/CAS_WR。若 WCK 窗口仍有效，后续 row-hit
          // 数据命令可以复用该窗口，此时统计为 skip 而不是错误。
            stats_.wck_sync_skips++;
        }
        wck.wck_active_until =
            std::max(wck.wck_active_until, clk + timing_delay(t.nCL + t.nBL + t.nWCKPST));
        req.cas_sync_issued = false;
      }
      if (issued == Command::RDA) {
        stats_.rda++;
      } else {
        stats_.rd++;
      }
      break;
      case Command::WR:
      case Command::WRA:
      // 写路径的主要差异是 PRE gate 包含 nCWL + nBL + nWR，表达写数据和写恢复。
        bank.next_any_col = clk + timing_delay(t.nCCDL);
      bank.next_wr = clk + timing_delay(t.nCCDL);
      bank.next_pre = std::max(bank.next_pre, clk + timing_delay(t.nCWL + t.nBL + t.nWR));
      if (issued == Command::WRA) {
        Cycle pre_at = clk + timing_delay(t.nCWL + t.nBL + t.nWR);
        bank.open_row = -1;
        bank.next_act = std::max(bank.next_act, pre_at + timing_delay(t.nRP));
      }
      col.next_col = clk + timing_delay(t.nCCDS);
      bg.next_col = clk + timing_delay(t.nCCDL);
      if (spec_.lpddr_family) {
        if (!req.cas_sync_issued) {
          stats_.wck_sync_skips++;
        }
        wck.wck_active_until =
            std::max(wck.wck_active_until, clk + timing_delay(t.nCWL + t.nBL + t.nWCKPST));
        req.cas_sync_issued = false;
      }
      if (issued == Command::WRA) {
        stats_.wra++;
      } else {
        stats_.wr++;
      }
      break;
      case Command::REFPB:
      // Per-bank refresh 不打开 row，但会阻止目标 bank 在 nRFCpb 内再次 ACT。
      // 这里也记录一次 activate-window 事件，保守表达 refresh 对 array 的占用。
        bank.next_act = clk + timing_delay(t.nRFCpb);
      row.next_row = clk + 1;
      timing_engine_.record_activate(spec_, req.decoded, clk);
      stats_.refpb++;
      break;
    case Command::REFDB:
      // LPDDR6 dual-bank refresh 同时约束目标 bank 和配对 bank。配对规则当前用 bank^1
      // 近似表达，后续可按目标器件 bank-pair 表继续细化。
      bank.next_act = clk + timing_delay(t.nRFCpb);
      {
        DecodedAddress partner = dual_bank_partner(req.decoded);
        BankState& partner_bank = banks_[partner.flat_bank(spec_)];
        partner_bank.next_act = std::max(partner_bank.next_act, clk + timing_delay(t.nRFCpb));
      }
      row.next_row = clk + 1;
      timing_engine_.record_activate(spec_, req.decoded, clk);
      stats_.refdb++;
      break;
      case Command::RFMPB:
      // RFMpb 完成后，RfmManager 按 spec.rfm_decrement 递减对应 bank 的 RAA/PRAC 计数。
        bank.next_act = clk + timing_delay(t.nRFMpb > 0 ? t.nRFMpb : t.nRFCpb);
      row.next_row = clk + 1;
      rfm_manager_.on_rfmpb(spec_, req.decoded, stats_);
      stats_.rfmpb++;
      break;
    case Command::PREAB:
      // all-bank 命令在本模型中按 channel 作用域执行。多 controller 模式下一个
      // Controller 本来就只拥有一个 channel；单 controller 调试时也不会误关其他 channel。
      close_all_banks(req.decoded, clk);
      row.next_row = clk + 1;
      stats_.preab++;
      break;
    case Command::REFAB:
      // REFab 不需要逐个修改 open_row，因为 state_ok/timing_ok 已要求 channel idle；
      // 它只需要把所有 bank 的 next_act 推迟到 nRFC 后。
      gate_all_banks_after(req.decoded, clk + timing_delay(t.nRFC));
      row.next_row = clk + 1;
      stats_.refab++;
      break;
    case Command::RFMAB:
      // RFMab 和 REFab 类似，但完成后会递减所有 bank 的 RAA/PRAC 计数并清除 pending 标记。
      gate_all_banks_after(req.decoded, clk + timing_delay(t.nRFMab > 0 ? t.nRFMab : t.nRFC));
      rfm_manager_.on_rfmab(spec_, stats_);
      row.next_row = clk + 1;
      stats_.rfmab++;
      break;
    case Command::MRW:
      row.next_row = clk + timing_delay(std::max(1, t.nMRW));
      stats_.mrw++;
      stats_.mode_register_ops++;
      break;
    case Command::MRR:
      row.next_row = clk + timing_delay(std::max(1, t.nMRR));
      stats_.mrr++;
      stats_.mode_register_ops++;
      break;
    case Command::WCKSYNC:
      col.next_col = clk + 1;
      wck.next_cas = clk + timing_delay(std::max(1, t.nWCKSYNC));
      wck.wck_ready_at = clk + timing_delay(std::max(1, t.nWCKSYNC));
      wck.wck_active_until = std::max(wck.wck_active_until,
                                      clk + timing_delay(std::max(t.nWCKPST, t.nWCKSYNC + 1)));
      stats_.wck_sync++;
      stats_.wck_syncs++;
      break;
    case Command::WCKTRAIN:
      row.next_row = clk + timing_delay(std::max(1, t.nWCKTRAIN));
      wck.next_cas = clk + timing_delay(std::max(1, t.nWCKTRAIN));
      wck.wck_ready_at = row.next_row;
      wck.wck_active_until = row.next_row + timing_delay(std::max(1, t.nWCKPST));
      stats_.wck_train++;
      stats_.wck_training_events++;
      break;
    case Command::DVFS:
      row.next_row = clk + timing_delay(std::max(1, t.nDVFS));
      col.next_col = row.next_row;
      wck.next_cas = row.next_row;
      stats_.dvfs++;
      stats_.dvfs_transitions++;
      break;
    case Command::PDE:
      row.next_row = clk + 1;
      stats_.pde++;
      break;
    case Command::PDX:
      row.next_row = clk + timing_delay(std::max(1, t.nPDEX));
      stats_.pdx++;
      break;
    case Command::SREFEN:
      row.next_row = clk + 1;
      stats_.srefen++;
      break;
    case Command::SREFEX:
      row.next_row = clk + timing_delay(std::max(1, t.nSREFEX));
      stats_.srefex++;
      break;
    case Command::ECCSCRUB:
      row.next_row = clk + timing_delay(std::max(1, t.nECCSCRUB));
      stats_.ecc_scrub++;
      stats_.ras_ecc_events++;
      break;
    case Command::RASERR:
      row.next_row = clk + timing_delay(std::max(1, t.nRASERR));
      col.next_col = std::max(col.next_col, clk + timing_delay(std::max(1, t.nLINKRETRY)));
      stats_.ras_err++;
      stats_.ras_ecc_events++;
      break;
    case Command::NOP:
      break;
  }

  // bank-local gate 只覆盖同 bank 状态；最后把表驱动 scope 约束统一交给
  // TimingEngine，例如 PC/SID/BG 范围的 nCCD/nRRD/nRFC 等。
  timing_engine_.apply_constraints(spec_, req.decoded, issued, clk);
  return result;
}

Cycle CommandExecutor::timing_delay(int cycles) const {
  if (cycles <= 0) {
    return 0;
  }
  return static_cast<Cycle>(cycles) * static_cast<Cycle>(std::max(1, spec_.tick_multiplier));
}

DecodedAddress CommandExecutor::dual_bank_partner(const DecodedAddress& decoded) const {
  DecodedAddress partner = decoded;
  // REFdb partner 目前采用 bank^1 的最小可解释近似，适合 banks_per_group 为偶数的
  // 测试配置。若后续导入完整 LPDDR6 bank-pair 表，应从这里替换。
  int banks_per_group = std::max(1, spec_.org.banks_per_group);
  if (banks_per_group > 1) {
    partner.bank = decoded.bank ^ 1;
    if (partner.bank >= banks_per_group) {
      partner.bank = decoded.bank;
    }
  }
  return partner;
}

void CommandExecutor::close_all_banks(const DecodedAddress& decoded, Cycle clk) {
  Cycle next_act = clk + timing_delay(spec_.timing.nRPab > 0 ? spec_.timing.nRPab : spec_.timing.nRP);
  // all-bank 命令作用域按 channel 切分：单个 Controller 拥有一个 channel，
  // MemorySystem 合并多个 Controller 时不会让某个 channel 的 PREab 影响另一个 channel。
  int banks_per_channel = std::max(1, spec_.banks_per_channel());
  int channel = std::clamp(decoded.channel, 0, std::max(1, spec_.org.channels) - 1);
  int begin = channel * banks_per_channel;
  int end = std::min(static_cast<int>(banks_.size()), begin + banks_per_channel);
  for (int i = begin; i < end; i++) {
    BankState& bank = banks_[static_cast<std::size_t>(i)];
    bank.open_row = -1;
    bank.activating = false;
    bank.next_act = std::max(bank.next_act, next_act);
  }
}

void CommandExecutor::gate_all_banks_after(const DecodedAddress& decoded, Cycle ready_at) {
  // REFab/RFMab 不改变 open_row，因为前置状态已经要求 idle；只需要为 channel 内
  // 每个 bank 写入恢复时间，阻止过早 ACT。
  int banks_per_channel = std::max(1, spec_.banks_per_channel());
  int channel = std::clamp(decoded.channel, 0, std::max(1, spec_.org.channels) - 1);
  int begin = channel * banks_per_channel;
  int end = std::min(static_cast<int>(banks_.size()), begin + banks_per_channel);
  for (int i = begin; i < end; i++) {
    BankState& bank = banks_[static_cast<std::size_t>(i)];
    bank.next_act = std::max(bank.next_act, ready_at);
  }
}

}  // namespace hbm_sim
