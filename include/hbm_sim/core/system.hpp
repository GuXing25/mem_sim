#pragma once

// Core 层多控制器系统：负责 frontend 注入和 channel-to-controller 分发。
// 它对应 Ramulator2.1 GenericDRAMSystem 的轻量化骨架。

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "hbm_sim/controller/controller.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/core/request.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/stats/stats.hpp"

namespace hbm_sim {

enum class StackQosPolicy {
  Fcfs,
  StrictPriority,
};

struct MemorySystemOptions {
  // 每个 channel controller 使用同一份 ControllerOptions。后续如果要研究
  // asymmetric channel，可以把这里扩展成 vector<ControllerOptions>。
  ControllerOptions controller;
  // channel_mapper 决定请求进入哪个 channel controller。Decoded 表示信任
  // AddressMapper 的 decoded.channel；RoundRobin/Xor 用于实验性分散流量。
  ChannelMapperKind channel_mapper = ChannelMapperKind::Decoded;
  // stack_count=1 保持历史单-stack行为。多 stack 时，每颗 stack 拥有独立的
  // MemoryImage 和完整 channel-controller 集合，彼此不共享 JEDEC 状态。
  int stack_count = 1;
  StackMappingKind stack_mapping = StackMappingKind::Interleaved;
  std::uint64_t stack_interleave_bytes = 256;
  // CLI 可预先创建/加载各 stack 的镜像；为空时 MemorySystem 自动创建。
  std::vector<std::shared_ptr<MemoryImage>> stack_memory_images;
  // 每颗 stack 独立入口队列提供反压隔离；dispatch_width 表示每拍最多向该
  // stack 的 channel controllers 分发多少请求。
  std::size_t stack_ingress_buffer_size = 256;
  std::size_t stack_dispatch_width = 4;
  StackQosPolicy stack_qos_policy = StackQosPolicy::StrictPriority;
};

// Ramulator2.1 GenericDRAMSystem 风格的轻量 memory system：
// - 每个 stack 的每个 channel 对应一个 Controller
// - 系统地址先选择 stack，再用 stack-local 地址选择 channel/bank
// - 每个 system cycle 并行 tick 所有 stack/channel controller
class MemorySystem {
 public:
  // 按 stack_count * spec.org.channels 创建 channel-local Controller。每个
  // Controller 的 channel 数会被 localize 成 1，因此内部 bank 下标是局部视角。
  explicit MemorySystem(DramSpec spec, MemorySystemOptions options = {});
  // 兼容旧代码的便捷构造：只传 ControllerOptions 时使用 decoded channel mapper。
  explicit MemorySystem(DramSpec spec, ControllerOptions controller_options);

  // 按 Request::inject_cycle 注入请求，并在每个 system cycle 并行 tick 所有 controller。
  void run(std::vector<Request> requests, Cycle max_cycles);
  void run(RequestSource& source, Cycle max_cycles);

  const Stats& stats() const { return stats_; }
  const DramSpec& spec() const { return spec_; }
  const std::vector<Controller>& controllers() const { return controllers_; }
  const std::vector<IssuedCommand>& issued_commands() const { return issued_; }
  std::size_t controller_count() const { return controllers_.size(); }
  int stack_count() const { return options_.stack_count; }
  const std::vector<std::shared_ptr<MemoryImage>>& stack_memory_images() const {
    return memory_images_;
  }
  const std::vector<Stats>& per_stack_stats() const { return per_stack_stats_; }

 private:
  // 全局 spec 保留原始 channel 数，用于带宽、输出和 channel mapper。
  DramSpec spec_;
  MemorySystemOptions options_;
  std::vector<std::shared_ptr<MemoryImage>> memory_images_;
  // 按 [stack][channel] 压平；每个元素代表一个 channel-local Controller。
  std::vector<Controller> controllers_;
  // 记录 workload 是否真正打到某个 controller，用于 active_controllers 输出。
  std::vector<bool> active_controller_seen_;
  std::vector<bool> active_stack_seen_;
  std::vector<std::deque<Request>> stack_ingress_queues_;
  std::vector<Stats> per_stack_stats_;
  std::uint64_t next_system_sequence_ = 0;
  // 合并后的全局命令 trace；collect_issued_commands() 会把局部 channel 写回全局坐标。
  std::vector<IssuedCommand> issued_;
  Stats stats_;
  // 外层 memory system cycle；所有 controller 在同一个 clk_ 上各 tick 一次。
  Cycle clk_ = 0;
  // frontend_stall_cycles_ 统计请求想注入但目标 stack ingress 满的 cycle。
  std::uint64_t frontend_stall_cycles_ = 0;
  std::uint64_t stack_ingress_stall_cycles_ = 0;
  std::uint64_t stack_ingress_peak_ = 0;
  std::uint64_t qos_priority_dispatches_ = 0;
  std::vector<std::uint64_t> per_stack_ingress_stalls_;
  std::vector<std::uint64_t> per_stack_ingress_peak_;
  std::vector<std::uint64_t> per_stack_qos_dispatches_;
  // max_cycles 结束时尚未注入的 frontend 请求数。
  std::uint64_t remaining_frontend_requests_ = 0;
  // RoundRobin mapper 的内部游标。
  std::vector<std::uint64_t> next_round_robin_channel_;

  // 把 full-stack spec 转成单 channel controller spec。
  static DramSpec make_channel_spec(const DramSpec& spec);
  // 选择目标 channel；可能使用 decoded.channel，也可能按 mapper 覆盖。
  int target_channel(const Request& req);
  int target_stack(const Request& req) const;
  std::size_t controller_index(int stack, int channel) const;
  // 把系统地址转换成 stack-local 地址，并把全局 channel 转成本地 channel=0。
  Request localize_request(Request req, int stack, int channel) const;
  bool enqueue(Request req);
  void dispatch_stack_ingress();
  bool done() const;
  void tick();
  void finalize_run_stats();
  void collect_issued_commands();
};

}  // namespace hbm_sim
