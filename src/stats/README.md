# 统计实现

本目录把内部统计整理为模型、参数、结果三部分，不参与数据传输或命令调度。

- `stats.cpp`：通过 `finalize_bandwidth_stats()` 共用 Controller、系统与单 Stack 的带宽公式；直接从 Stats 构造公共运行指标，按需收集有明确用途的补充诊断计数。
- `result.cpp`：从生效模型、运行选项和统计对象构造分区结果、参数变化、人读报告及结构化 JSON。
- JSON 不再反解析终端文本；整数和浮点值按类型保存。
- summary 使用英文精简报告（MODEL / PARAMETERS / RESULTS）；指标和提示使用英文，用户自定义名称原样保留。只有 diagnostic 附加内部字段，不能要求普通结果含全部内部计数。
- `row_hit_pct` 的分母是首次调度分类总数；平均队列诊断统一使用 aggregate_ctrl_cycles 归一化。
- 读写事务延迟按各自已完成事务数求平均；无同类完成时公共 JSON 为 null，summary 为 N/A。
- 内部计数被 Controller/MemorySystem/验证测试使用，不等于每一项都应公开。
- 带宽计算保留原浮点运算顺序与作用域边界：零周期时 Controller/系统的空接口效率为 100%，单 Stack 的效率保持默认值；汇总不修改任何 DRAM/PHY 完成时间。
- 新增公共指标须说明独立用途、单位、范围、分母和无数据时的状态，并更新结果契约测试。

接口声明见 [统计接口](../../include/hbm_sim/stats/README.md)。
公共与诊断字段不重复存储；完整配置由 resolved.cfg 承担。配置和结果均不使用格式版本字段。
字段单位、条件输出和使用说明见 [输出指南](../../文档/输出指南.md)。
