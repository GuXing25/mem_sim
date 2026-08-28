# Config module

`document.cpp` 只实现配置文档的语法、standard/preset 选择、section 激活和固定
layer 排序，不直接修改 `DramSpec`。字段到模型成员的一对一映射仍集中在
`src/cli/main.cpp::apply_option()` 与 `apply_spec_overrides()`，便于审计。

schema-v2 layer 为：meta/model 0、公共 section 10、family 20、standard 30、preset 40、
override 50；命令行由 CLI 记录为 60。inactive standard/preset section 不进入
解析结果。旧平面 `key=value` 文档保持原始顺序，以兼容外部 timing profile。

新增 section 或简写键时，应同步：

1. 更新 `is_common_section()`/`canonical_entry_key()`；
2. 在 CLI 的显式白名单中接线到真实字段；
3. 更新两个家族主配置及 `configs/README.md`；
4. 为 active/inactive section、错误诊断和 resolved-config 重载增加测试。

配置不能创建新的算法实现。scheduler、row policy、PHY mode 和 backend 的陌生名称
必须失败；增加名称时还要实现对应 C++ 行为和回归测试。
