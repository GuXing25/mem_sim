#pragma once

// 标准/供应商 timing profile 数据库入口。
// 配置流程是：
// 1. make_spec_draft() 只应用标准 traits；
// 2. CLI 设置 speed_bin/density/stack_height/mode/vendor profile 选择项；
// 3. apply_standard_timing_profile() 展开完整 organization/timing；
// 4. 用户逐项 override 覆盖 profile；
// 5. finalize_spec() 只执行一次，生成 constraints/table。

#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

// 根据 DramSpec 中的 timing_profile/vendor_profile/speed_bin_mbps/density_gb/
// stack_height/mode_profile/lpddr_dvfs_mode 等字段，更新 timing、组织参数和来源元数据。
// generic profile 只给“标准意识”的默认表，不冒充具体 vendor；vendor_profile 非 generic
// 时，展开的行/列 timing 会标记为 Vendor，供 strict_timing_table 使用。本函数不
// 重建派生表；调用方完成显式覆盖后必须调用 finalize_spec()。
void apply_standard_timing_profile(DramSpec& spec);

}  // namespace hbm_sim
