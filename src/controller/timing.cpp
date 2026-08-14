// TimingEngine：维护跨 bank/scope 的 timing gate。
// bank-local nRCD/nRP 等仍在 BankState 中；这里负责 channel/PC/SID/rank/BG/bank
// scope 的表驱动 constraint、tFAW 窗口以及 LPDDR WCK ready window。
#include "hbm_sim/controller/timing.hpp"

#include <algorithm>

namespace hbm_sim {

void TimingEngine::reset(const DramSpec& spec) {
  // 每类 scope 都分配独立数组：即使某个 preset 当前不用 SID 或 Rank，
  // scope_count() 也会至少返回 1，使统一索引逻辑不用到处判断空数组。
  //
  // activation/row/column/wck 的 scope 来自 DramSpec，可按标准切换。例如 HBM4
  // column bus 放在 PseudoChannel，LPDDR6 WCK 也可放在 PseudoChannel。
  channel_scopes_.assign(scope_count(spec, TimingScope::Channel), {});
  pseudo_channel_scopes_.assign(scope_count(spec, TimingScope::PseudoChannel), {});
  sid_scopes_.assign(scope_count(spec, TimingScope::Sid), {});
  rank_scopes_.assign(scope_count(spec, TimingScope::Rank), {});
  activation_scopes_.assign(scope_count(spec, spec.activation_scope), {});
  row_scopes_.assign(scope_count(spec, spec.row_bus_scope), {});
  column_scopes_.assign(scope_count(spec, spec.column_bus_scope), {});
  bank_group_scopes_.assign(scope_count(spec, TimingScope::BankGroup), {});
  bank_scopes_.assign(scope_count(spec, TimingScope::Bank), {});
  wck_scopes_.assign(scope_count(spec, spec.wck_scope), {});
}

TimingScopeState& TimingEngine::row_state(const DramSpec& spec, const DecodedAddress& decoded) {
  return row_scopes_[scope_index(spec, spec.row_bus_scope, decoded)];
}

const TimingScopeState& TimingEngine::row_state(const DramSpec& spec, const DecodedAddress& decoded) const {
  return row_scopes_[scope_index(spec, spec.row_bus_scope, decoded)];
}

TimingScopeState& TimingEngine::column_state(const DramSpec& spec, const DecodedAddress& decoded) {
  return column_scopes_[scope_index(spec, spec.column_bus_scope, decoded)];
}

const TimingScopeState& TimingEngine::column_state(const DramSpec& spec, const DecodedAddress& decoded) const {
  return column_scopes_[scope_index(spec, spec.column_bus_scope, decoded)];
}

TimingScopeState& TimingEngine::bank_group_state(const DramSpec& spec, const DecodedAddress& decoded) {
  return bank_group_scopes_[scope_index(spec, TimingScope::BankGroup, decoded)];
}

const TimingScopeState& TimingEngine::bank_group_state(const DramSpec& spec, const DecodedAddress& decoded) const {
  return bank_group_scopes_[scope_index(spec, TimingScope::BankGroup, decoded)];
}

TimingScopeState& TimingEngine::wck_state(const DramSpec& spec, const DecodedAddress& decoded) {
  return wck_scopes_[scope_index(spec, spec.wck_scope, decoded)];
}

const TimingScopeState& TimingEngine::wck_state(const DramSpec& spec, const DecodedAddress& decoded) const {
  return wck_scopes_[scope_index(spec, spec.wck_scope, decoded)];
}

bool TimingEngine::constraint_ready(
    const DramSpec& spec,
    const DecodedAddress& decoded,
    Command cmd,
    Cycle clk) const {
    for (const auto& constraint : spec.timing_constraints) {
      if (constraint.window > 0) {
      // window 型约束例如 tFAW 需要计数历史命令，而不是单个 ready time；
      // 它们在 faw_ready()/record_activate() 路径处理。
        continue;
      }
    if (std::find(constraint.following.begin(), constraint.following.end(), cmd) == constraint.following.end()) {
      continue;
    }
      const TimingScopeState& scope = scope_state(spec, constraint.scope, decoded);
    // next_command[cmd] 是该 scope 中“这条 following 命令最早能发的 tick”。
    // 多条 preceding 规则会取 max，因此任何一个更严格约束都会阻止发射。
      if (clk < scope.next_command[command_index(cmd)]) {
      return false;
    }
  }
  return true;
}

void TimingEngine::apply_constraints(
    const DramSpec& spec,
    const DecodedAddress& decoded,
    Command issued,
    Cycle clk) {
    for (const auto& constraint : spec.timing_constraints) {
    if (constraint.window > 0) {
      continue;
    }
    if (std::find(constraint.preceding.begin(), constraint.preceding.end(), issued) == constraint.preceding.end()) {
      continue;
    }

      TimingScopeState& scope = mutable_scope(spec, constraint.scope, decoded);
      Cycle ready = clk + timing_delay(spec, std::max(0, constraint.latency));
    // 一条 issued 命令可能同时限制多种 following 命令，例如 RD 会限制后续 WR 和 PRE。
    // 用 max 更新可以叠加不同作用域/不同先行命令产生的约束。
      for (Command following : constraint.following) {
      auto idx = command_index(following);
      scope.next_command[idx] = std::max(scope.next_command[idx], ready);
    }
  }
}

void TimingEngine::prune_recent_acts(const DramSpec& spec, Cycle clk) {
  Cycle window = timing_delay(spec, std::max(1, spec.timing.nFAW));
  for (auto& scope : activation_scopes_) {
    // recent_acts 按发射顺序保存；只要队首已经滑出 tFAW 窗口即可弹出。
    // faw_ready() 只看窗口内数量是否小于 4。
    while (!scope.recent_acts.empty() && clk >= scope.recent_acts.front() &&
           clk - scope.recent_acts.front() >= window) {
      scope.recent_acts.pop_front();
    }
  }
}

void TimingEngine::record_activate(const DramSpec& spec, const DecodedAddress& decoded, Cycle clk) {
  if (activation_scopes_.empty()) {
    return;
  }
  // REFPB/REFDB 在当前模型中也会调用 record_activate，因为它们会占用类似 activate
  // 的 row/array 维护窗口；这是一种保守近似，后续可按标准把 refresh window 单独拆出。
  activation_scopes_[scope_index(spec, spec.activation_scope, decoded)].recent_acts.push_back(clk);
}

bool TimingEngine::faw_ready(const DramSpec& spec, const DecodedAddress& decoded) const {
  if (activation_scopes_.empty()) {
    return true;
  }
  return activation_scopes_[scope_index(spec, spec.activation_scope, decoded)].recent_acts.size() < 4;
}

bool TimingEngine::wck_ready_for_data(const DramSpec& spec, const DecodedAddress& decoded, Cycle clk) const {
  if (!spec.lpddr_family) {
    return true;
  }
  if (spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn) {
    return true;
  }
  const TimingScopeState& wck = wck_state(spec, decoded);
  return clk >= wck.wck_ready_at && clk < wck.wck_active_until;
}

std::size_t TimingEngine::scope_count(const DramSpec& spec, TimingScope scope) const {
  const Organization& o = spec.org;
  // scope_count 和 scope_index 必须使用完全相同的层级乘法顺序，否则 decoded 地址
  // 会落到错误的 timing bucket。顺序为 Channel -> PC -> SID -> Rank -> BG -> Bank。
  switch (scope) {
    case TimingScope::Channel:
      return static_cast<std::size_t>(std::max(1, o.channels));
    case TimingScope::PseudoChannel:
      return static_cast<std::size_t>(std::max(1, o.channels) * std::max(1, o.pseudo_channels));
    case TimingScope::Sid:
      return static_cast<std::size_t>(std::max(1, o.channels) * std::max(1, o.pseudo_channels) *
                                      std::max(1, o.sids));
    case TimingScope::Rank:
      return static_cast<std::size_t>(std::max(1, o.channels) * std::max(1, o.pseudo_channels) *
                                      std::max(1, o.sids) * std::max(1, o.ranks));
    case TimingScope::BankGroup:
      return static_cast<std::size_t>(std::max(1, o.channels) * std::max(1, o.pseudo_channels) *
                                      std::max(1, o.sids) * std::max(1, o.ranks) *
                                      std::max(1, o.bank_groups));
    case TimingScope::Bank:
      return static_cast<std::size_t>(std::max(1, o.channels) * std::max(1, o.pseudo_channels) *
                                      std::max(1, o.sids) * std::max(1, o.ranks) *
                                      std::max(1, o.bank_groups) *
                                      std::max(1, o.banks_per_group));
  }
  return 1;
}

std::size_t TimingEngine::scope_index(
    const DramSpec& spec,
    TimingScope scope,
    const DecodedAddress& d) const {
    const Organization& o = spec.org;
    std::size_t x = static_cast<std::size_t>(std::clamp(d.channel, 0, std::max(1, o.channels) - 1));
  // 逐层混合进制编码。每到一个目标 scope 就提前返回，避免把更细层级也算进去。
  // clamp 用于防御错误/手写测试 decoded 越界，退化到最近合法 bucket。
    if (scope == TimingScope::Channel) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.pseudo_channels)) +
      static_cast<std::size_t>(std::clamp(d.pseudo_channel, 0, std::max(1, o.pseudo_channels) - 1));
  if (scope == TimingScope::PseudoChannel) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.sids)) +
      static_cast<std::size_t>(std::clamp(d.sid, 0, std::max(1, o.sids) - 1));
  if (scope == TimingScope::Sid) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.ranks)) +
      static_cast<std::size_t>(std::clamp(d.rank, 0, std::max(1, o.ranks) - 1));
  if (scope == TimingScope::Rank) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.bank_groups)) +
      static_cast<std::size_t>(std::clamp(d.bank_group, 0, std::max(1, o.bank_groups) - 1));
  if (scope == TimingScope::BankGroup) {
    return x;
  }

  x = x * static_cast<std::size_t>(std::max(1, o.banks_per_group)) +
      static_cast<std::size_t>(std::clamp(d.bank, 0, std::max(1, o.banks_per_group) - 1));
  return x;
}

Cycle TimingEngine::timing_delay(const DramSpec& spec, int cycles) const {
  if (cycles <= 0) {
    return 0;
  }
  // Timing 结构保存 nCK，Controller 内部 tick 可以更细。例如 HBM edge pairing
  // 用 tick_multiplier=2 将一个 nCK 拆成 rising/falling 两个 tick。
  return static_cast<Cycle>(cycles) * static_cast<Cycle>(std::max(1, spec.tick_multiplier));
}

TimingScopeState& TimingEngine::mutable_scope(
    const DramSpec& spec,
    TimingScope scope,
    const DecodedAddress& decoded) {
    switch (scope) {
    // 这里集中做 scope enum 到具体数组的分派。Controller 不直接接触这些数组，
    // 因此新增一种作用域时只需要扩展 TimingEngine，而不是散改热路径。
      case TimingScope::Channel:
      return channel_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::PseudoChannel:
      return pseudo_channel_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::Sid:
      return sid_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::Rank:
      return rank_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::BankGroup:
      return bank_group_scopes_[scope_index(spec, TimingScope::BankGroup, decoded)];
    case TimingScope::Bank:
      return bank_scopes_[scope_index(spec, TimingScope::Bank, decoded)];
  }
  return bank_scopes_[scope_index(spec, TimingScope::Bank, decoded)];
}

const TimingScopeState& TimingEngine::scope_state(
    const DramSpec& spec,
    TimingScope scope,
    const DecodedAddress& decoded) const {
  switch (scope) {
    case TimingScope::Channel:
      return channel_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::PseudoChannel:
      return pseudo_channel_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::Sid:
      return sid_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::Rank:
      return rank_scopes_[scope_index(spec, scope, decoded)];
    case TimingScope::BankGroup:
      return bank_group_scopes_[scope_index(spec, TimingScope::BankGroup, decoded)];
    case TimingScope::Bank:
      return bank_scopes_[scope_index(spec, TimingScope::Bank, decoded)];
  }
  return bank_scopes_[scope_index(spec, TimingScope::Bank, decoded)];
}

}  // namespace hbm_sim
