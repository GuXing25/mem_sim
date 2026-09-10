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

验证夹具也采用 schema 3；不再借旧版本保留与几何矛盾的显式密度或时钟。
单通道 HBM 夹具的 density 是其实际几何按层数折算的值，不是完整 Ramulator 器件的标称密度。
LPDDR 夹具同样按实际几何除以 Channel×Subchannel×Rank 折算 density；当前共同面
得到 256 Gibit，不表示外部 `16Gb` 型号是一颗 256 Gibit 器件。这里比较指定命令面，
并未要求两端完整容量相同。实际数值与外部型号分别记录在
[profile_index.csv](../profile_index.csv) 的数值列和 `reference_model` 列。
关闭刷新/RFM 的共同命令比较不会使用它来生成周期性维护命令。
HBM3 的参考 preset 显式保存 `nRFC=560 nCK`（参考器件 350 ns / 625 ps），
用于时序表和显式命令的外部对照；不再依赖旧的独立 density 输入选中这行时序。
`dramsim3_hbm2_common` 显式保留 1000 ps CK，并以本模型 HBM 的时间公式配置
4000 Mb/s/pin；这是 HBM3 执行语义下的旧协议公共面抽象，不声明真实 HBM2 接口速率。
