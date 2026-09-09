# Config module

`document.cpp` 只实现配置文档的语法、standard/preset 选择、section 激活和固定
layer 排序，不直接修改 `DramSpec`。`model.cpp` 保存模型字段的一对一映射、
`build_model()` 和联动输入解析；CLI 的 `apply_option()` 只分发平台选项与模型输入。
外部前端可通过 `include/hbm_sim/config/model.hpp` 复用同一模型构建入口。

schema-v2 layer 为：meta/model 0、公共 section 10、family 20、standard 30、preset 40、
override 50；命令行由 CLI 记录为 60。inactive standard/preset section 不进入
解析结果。旧平面 `key=value` 文档保持原始顺序，以兼容外部 timing profile。

新增 section 或简写键时，应同步：

1. 更新 `is_common_section()`/`canonical_entry_key()`；
2. 在配置模块的模型白名单或 CLI 平台选项中接线到真实字段；
3. 更新两个家族主配置及 `configs/README.md`；
4. 为 active/inactive section、错误诊断和 resolved-config 重载增加测试。

配置不能创建新的算法实现。scheduler、row policy、PHY mode 和 backend 的陌生名称
必须失败；增加名称时还要实现对应 C++ 行为和回归测试。

schema 3 的 `resolve_coupled_inputs()` 在 profile 展开前核对几何、密度、速率和
时钟；`model_config_tests` 检查小容量、部分 channel、分数密度、溢出和矛盾输入。
新模板迁移与旧版本兼容状态见 [实施记录](../../文档/配置重构实施记录.md)。
