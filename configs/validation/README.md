# 验证元数据

本目录保存四个标准的可执行验证参数集，以及不参与模型解析的来源清单：

- `hbm3.cfg`：Ramulator2 HBM3 共同面和 DRAMsim3 较老 HBM 辅助面。
- `hbm4.cfg`：项目内 HBM4 单通道路径和 Ramulator2 HBM4 共同面。
- `lpddr5.cfg`：Ramulator2 LPDDR5 共同面。
- `lpddr6.cfg`：Ramulator2 LPDDR6 共同面。
- `project_identity.csv`：项目原创能力及其验证证据绑定。
- `vendor_parameters.csv`：器件级结论仍缺少的厂商参数。

四份 cfg 都用 `[meta] extends` 继承标准主配置，只保存比较时需要改变的组织、Timing、
维护、PHY 和 workload 条件。配置本身不会运行外部仿真器；`tools/config_selection.py`
负责选择它们，`ramulator2_differential.py` 和 `dramsim3_aux_validation.py` 才会分别
执行外部模型并保存外部版本、入口、原始结果和比较判据。
