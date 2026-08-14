# 统计接口

stats 字段已经覆盖真实存储区和满负载路径：allocated/written lines、storage density、burst split、row buffer、power/thermal、ECC shadow、DFI beats 和接口带宽。新增字段必须保持 CLI 输出 key 稳定，方便脚本和论文表格复用。

本目录声明稳定统计结构。统计结构用于 controller、memory system、CLI 输出和测试断言共享。

主要头文件：

- `stats.hpp`：`Stats` 字段和统计辅助函数声明。

修改建议：

- 新增统计字段时要同时更新 `src/stats/stats.cpp` 的文本输出。
- 输出字段尽量保持稳定顺序，便于 smoke test、脚本和对比工具解析。
- 需要区分 system-level、controller aggregate、payload bandwidth 和 interface bandwidth。
- 真实堆叠存储相关字段按族组织：`data_*` 描述 payload 校验，`storage_*` 描述稀疏存储区和物理坐标覆盖，`rowbuf_*` 描述行缓冲，`floorplan_*` 描述 bank tile 覆盖，`power_*` 描述命令能量，`thermal_*` 描述 tile/grid 热点温度和 TSV-aware 耦合，`ecc_*` 描述 SECDED shadow，`dfi_*` 描述数据 beat 视图。

## stats 字段设计原则

统计字段尽量分成三类：

- 原始事件计数：请求数、命令数、refresh/RFM 次数、WCK/DVFS/低功耗事件。
- 累积量：字节数、队列长度积分、延迟总和、接口开销 bit。
- 派生指标：平均延迟、bytes/cycle、带宽、带宽利用率、payload efficiency。

原始事件和累积量应在仿真过程中更新；派生指标尽量在输出时计算，避免不同模块重复计算。

## 新增字段清单

- 字段是否属于 system-level 还是 controller-level。
- 多 controller 合并时是求和、取最大、取平均还是重新计算。
- 字段是否需要进入 `tools/compare_stats.py` 的对比集合。
- 字段名称是否稳定、短、能被脚本解析。
- 输出是否保持 `name : value` 对齐格式。
