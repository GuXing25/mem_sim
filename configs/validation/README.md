# 验证元数据

本目录只保留不参与模型解析的审计清单：

- `project_identity.csv`：项目原创能力及其验证证据绑定。
- `vendor_parameters.csv`：器件级结论仍缺少的厂商参数。

所有可执行验证配置已并入 `configs/hbm.cfg`、`configs/lpddr.cfg` 的命名 preset；
工具统一通过 `tools/config_selection.py` 选择它们。这里不再放 `.cfg`，从而避免
单通道差分参数和两份主配置发生漂移。
