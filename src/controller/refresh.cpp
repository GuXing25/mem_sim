// RefreshManager：维护 JEDEC refresh 节奏和目标 bank/dual-bank/all-bank 轮转。
// 它只生成维护命令请求；是否需要 PRE、何时真正发 REF，仍由 Controller 的
// priority path、CommandState 和 TimingEngine 统一裁决。
#include "hbm_sim/controller/refresh.hpp"

#include <algorithm>

namespace hbm_sim {

void RefreshManager::reset(const DramSpec& spec, Cycle clk) {
  sid_cursor_ = 0;
  flat_bank_cursor_ = 0;
  refresh_credit_ = 0;
  postpone_count_ = 0;
  pullin_count_ = 0;
  // per-bank refresh 用 nREFIpb；all-bank refresh 用 nREFI。若某个 preset 没有
  // 给 nREFIpb，则回退到 nREFI，保证 refresh manager 不因未建 per-bank 表而崩溃。
  int interval = refresh_interval(spec);
  next_refresh_cycle_ = clk + timing_delay(spec, interval);
}

RefreshTickResult RefreshManager::tick(const DramSpec& spec, Cycle clk, bool prefer_postpone, bool allow_pull_in) {
  RefreshTickResult result;
  int interval = refresh_interval(spec);
  if (!spec.supports_refresh || interval <= 0) {
    return result;
  }

  const int credit_limit = std::max(0, spec.refresh_credit_limit);
  const bool due = clk >= next_refresh_cycle_;
  const bool can_postpone = spec.refresh_postpone_limit > 0 &&
                            postpone_count_ < spec.refresh_postpone_limit &&
                            (credit_limit == 0 || refresh_credit_ < credit_limit);
  const bool can_pull_in = spec.refresh_pullin_limit > 0 &&
                           pullin_count_ < spec.refresh_pullin_limit &&
                           allow_pull_in &&
                           clk + timing_delay(spec, interval / 2) >= next_refresh_cycle_;

  if (!due && !can_pull_in) {
    // 还没到 refresh deadline 且不允许 pull-in 时不产生命令。
    result.credit = refresh_credit_;
    return result;
  }

  if (due && prefer_postpone && can_postpone) {
    refresh_credit_++;
    postpone_count_++;
    next_refresh_cycle_ += timing_delay(spec, interval);
    result.postponed = true;
    result.credit = refresh_credit_;
    return result;
  }

  result.started_batch = true;
  result.pulled_in = !due;
  result.commands = seed_refresh_batch(spec);
  if (refresh_credit_ > 0) {
    refresh_credit_--;
  } else if (result.pulled_in) {
    refresh_credit_--;
    pullin_count_++;
  }
  if (due || result.pulled_in) {
    next_refresh_cycle_ += timing_delay(spec, interval);
  }
  if (refresh_credit_ <= 0) {
    postpone_count_ = 0;
  }
  if (refresh_credit_ >= 0) {
    pullin_count_ = 0;
  }
  result.credit = refresh_credit_;
  return result;
}

Cycle RefreshManager::timing_delay(const DramSpec& spec, int cycles) const {
  if (cycles <= 0) {
    return 0;
  }
  return static_cast<Cycle>(cycles) * static_cast<Cycle>(std::max(1, spec.tick_multiplier));
}

int RefreshManager::refresh_interval(const DramSpec& spec) const {
  int interval = spec.refresh_policy == MaintenancePolicyKind::AllBank
                     ? spec.timing.nREFI
                     : (spec.timing.nREFIpb > 0 ? spec.timing.nREFIpb : spec.timing.nREFI);
  if (spec.refresh_temperature_mode != RefreshTemperatureMode::Normal) {
    interval = std::max(1, interval / std::max(1, spec.refresh_high_temp_multiplier));
  }
  return interval;
}

std::vector<MaintenanceCommand> RefreshManager::seed_refresh_batch(const DramSpec& spec) {
  std::vector<MaintenanceCommand> commands;
  if (spec.refresh_policy == MaintenancePolicyKind::AllBank) {
    DecodedAddress d;
    d.rank = 0;
    // all-bank refresh 在每个 Controller 的 channel 内执行。这里 decoded.channel 先为 0；
    // MemorySystem 多 controller 模式下每个 channel controller 各自拥有本地坐标。
    commands.push_back(MaintenanceCommand{Command::REFAB, d});
    return commands;
  }

  int banks_per_sid = std::max(1, spec.org.bank_groups * spec.org.banks_per_group);
  int sid_count = std::max(1, spec.org.sids);
  int flat = flat_bank_cursor_ % banks_per_sid;
  int sid = sid_cursor_ % sid_count;
  int banks_per_group = std::max(1, spec.org.banks_per_group);

  // per-bank/dual-bank refresh 轮转顺序：
  // 1. 在当前 SID 内按 flat bank 递增；
  // 2. 一个 SID 内所有 bank 轮完后切到下一个 SID；
  // 3. 每个 refresh tick 对所有 channel/pseudo-channel 的同一 bank 位置发一批命令。
  //
  // 这种设计让 refresh 行为可预测，便于测试，也接近 JEDEC 表中按 bank 地址轮转的思路。
  for (int ch = 0; ch < std::max(1, spec.org.channels); ch++) {
    for (int pc = 0; pc < std::max(1, spec.org.pseudo_channels); pc++) {
      DecodedAddress d;
      d.channel = ch;
      d.pseudo_channel = pc;
      d.sid = sid;
      d.rank = 0;
      d.bank_group = flat / banks_per_group;
      d.bank = flat % banks_per_group;
      commands.push_back(MaintenanceCommand{
          spec.lpddr_dual_bank_refresh ? Command::REFDB : Command::REFPB,
          d,
      });
    }
  }

  flat_bank_cursor_++;
  if (flat_bank_cursor_ >= banks_per_sid) {
    flat_bank_cursor_ = 0;
    sid_cursor_ = (sid_cursor_ + 1) % sid_count;
  }
  return commands;
}

}  // namespace hbm_sim
