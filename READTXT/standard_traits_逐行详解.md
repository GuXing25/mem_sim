# standard_traits.cpp 逐行代码详解

本文档按符号和职责解释 `src/dram/standard_traits.cpp`，说明每段代码“做什么”以及
“为什么这样设计”。章节中的行号只作相对导航，以符号名和当前源码为准。

配套头文件：`include/hbm_sim/dram/spec.hpp`（其中 `DramStandard`/`DramFamily` 枚举和 `StandardTraits` 结构体的定义在本文件第 18-29 行和第 241-270 行）。

---

## 零、文件在构建管道中的位置

`standard_traits.cpp` 是 DRAM 规格构建**三阶段管道的第一阶段**：

```
第一阶段: standard_traits.cpp
  find_standard_traits("hbm4")      → 查目录，返回 HBM4 的 traits
  make_spec_draft("hbm4")           → 生成 DramSpec "骨架"
      （只有身份 + 协议能力 + profile 名字，Timing/Org 全是默认值）

第二阶段: profiles.cpp
  apply_standard_timing_profile()   → 按 speed_bin/density/stack_height
      → 填充完整的 Timing 数值和 Organization

第三阶段: spec.cpp
  finalize_spec()                   → 从填充后的 Timing 构建 timing_table
                                      和 timing_constraints
```

**一句话定位**：`standard_traits.cpp` 回答"HBM4 是什么、支持什么、默认用哪个 profile"，**不回答**"HBM4 的时序数值是多少"。

---

## 一、文件头注释与头文件（第 1-8 行）

```cpp
// 标准 traits 目录：只保存协议身份、能力和默认 profile 选择。
// 组织结构和 timing 数值由 profiles.cpp 唯一负责。
#include "hbm_sim/dram/spec.hpp"

#include <algorithm>   // std::transform — 用于 normalized_name 的小写化
#include <array>       // std::array — kStandardTraits 固定大小数组
#include <cctype>      // std::tolower — 大小写不敏感的标准名匹配
#include <stdexcept>   // std::invalid_argument — 未支持标准时报错
```

### 文件头注释的契约

这两行注释是整个文件的设计契约：

1. **traits 目录只保存三样东西**：协议身份（`standard`/`family`）、协议能力（`supports_rfm`/`supports_ecc` 等开关）、默认 profile 选择（profile 名字 + speed/density/stack 默认值）。
2. **组织和时序数值的唯一负责人是 profiles.cpp**。traits 里一个 `nCL`、一个 `channels` 都不出现。

这个契约保证了"身份"和"数值"不会混淆——如果以后要新增一个 48GB 的 HBM4 变体，只需要新增一个 profile 文件，不需要动 traits。

### 为什么只需要 4 个头文件

这个文件是纯"数据查询"层：一个静态数组 + 三个查询函数 + 一个工厂函数。不涉及文件 I/O、不涉及系统调用、不涉及复杂 STL，所以 include 极少。

---

## 二、命名空间与 C++20 特性（第 10-11 行）

```cpp
namespace hbm_sim {
namespace {
```

`namespace {` 匿名命名空间：`kStandardTraits` 数组和 `normalized_name()` 是翻译单元私有的，外部不可见。只有三个公开函数（`find_standard_traits`、`apply_standard_traits`、`make_spec_draft`、`supported_specs`）在匿名命名空间之外（第 90-166 行），通过 `spec.hpp` 中的声明暴露。

---

## 三、kStandardTraits — 标准目录（第 13-79 行）

这是整个文件的核心数据结构：一个 `constexpr std::array<StandardTraits, 4>`，编译期构建，零运行时初始化开销。

```cpp
constexpr std::array<StandardTraits, 4> kStandardTraits{{
    {
        .standard = DramStandard::Hbm4,
        .family = DramFamily::Hbm,
        .canonical_key = "hbm4",
        .display_name = "HBM4",
        .aliases = {"hbm4", "hbm", ""},
        .alias_count = 2,
        .default_timing_profile = "hbm4_jedec_8g_32gb_8hi",
        .default_speed_bin_mbps = 8000,
        .default_density_gb = 32,
        .default_stack_height = 8,
        .supports_rfm = true,
        .supports_ecc = true,
        .hbm_edge_pairing_matrix = "hbm4_jedec270_4a_row_col_pre_matrix",
        .hbm_sid_mapping = "sid_pair_8hi",
        .hbm_ecc_scheme = "metadata_16b_per_32B_transaction",
        .hbm_ras_policy = "counter_only",
    },
    { ... HBM3 ... },
    { ... LPDDR6 ... },
    { ... LPDDR5 ... },
}};
```

### 3.1 为什么是 constexpr？

`constexpr` 意味着这个数组在**编译期**就完全确定：
- 没有运行时初始化（没有静态初始化顺序问题，没有构造函数调用）
- 编译器可以把它放在只读数据段（`.rodata`），甚至可以完全内联到使用处
- 所有字符串都是 `string_view` 指向**静态存储期字符串字面量**，零堆分配

这也是为什么用 `std::string_view` 而不是 `std::string`——`std::string` 在 C++20 中还不能方便地用于 constexpr（堆分配在常量求值中受限），而 `string_view` 只是"指针+长度"两个整数，天然 constexpr 友好。

### 3.2 外层双花括号 `{{ }}` 的含义

`std::array<StandardTraits, 4>` 在 C++ 中是"包着一个内置数组的聚合"。外层花括号初始化 `std::array` 本身，内层花括号初始化内部的 `StandardTraits[4]`。双花括号是 std::array 聚合初始化的标准写法。

### 3.3 Designated initializers（C++20 指定初始化器）

`.standard = DramStandard::Hbm4` 这种写法是 C++20 的 **designated initializers**。它有几个严格要求：
1. 字段必须**按声明顺序**初始化（`standard` 必须在 `family` 之前，不能乱序）
2. 未指定的字段使用结构体的默认值（如 HBM3 条目没有写 `default_mode_profile`，就用 `StandardTraits` 中声明的 `"default"`）
3. 不能和常规初始化混用

**为什么用它而不用位置初始化？** 可读性。看 `.hbm_ecc_scheme = "metadata_16b_per_32B_transaction"` 一眼知道这是 ECC 模式；位置初始化 `{4, 0, "hbm4", ...}` 要数到第几个字段才知道是什么。而且字段未指定的默认值自动生效，条目之间可以省略重复字段。

### 3.4 HBM4 条目逐字段（第 14-31 行）

| 字段 | 值 | 含义 |
|------|-----|------|
| `.standard` | `DramStandard::Hbm4` | 枚举身份标识 |
| `.family` | `DramFamily::Hbm` | 家族——HBM 家族自动获得双命令总线、edge pairing |
| `.canonical_key` | `"hbm4"` | 规范名，用于 `supported_specs()` 输出 |
| `.display_name` | `"HBM4"` | 显示名，写入 `spec.name`，最终打印到 CLI 输出 |
| `.aliases` | `{"hbm4", "hbm", ""}` | 可接受的别名：`hbm4`、`hbm` |
| `.alias_count` | `2` | 实际参与匹配的 alias 数量（见 3.6） |
| `.default_timing_profile` | `"hbm4_jedec_8g_32gb_8hi"` | 指向 configs/profiles/ 中的 profile 名 |
| `.default_speed_bin_mbps` | `8000` | 默认速率档位（8.0 Gbps/pin） |
| `.default_density_gb` | `32` | 默认 die 密度（32 Gb） |
| `.default_stack_height` | `8` | 默认堆叠层数（8-high） |
| `.supports_rfm` | `true` | 支持 RFM/PRAC（row hammer 缓解） |
| `.supports_ecc` | `true` | 支持 ECC |
| `.hbm_edge_pairing_matrix` | `"hbm4_jedec270_4a_row_col_pre_matrix"` | HBM4 的 PRE 配对矩阵规则名 |
| `.hbm_sid_mapping` | `"sid_pair_8hi"` | SID→物理层映射策略名 |
| `.hbm_ecc_scheme` | `"metadata_16b_per_32B_transaction"` | ECC 方案：每 32B 事务 16 bit metadata |
| `.hbm_ras_policy` | `"counter_only"` | RAS 策略：只计数 |

注意**哪些字段没写**：`default_mode_profile`（用默认值 `"default"`）、`lpddr_wck_training_required`（用默认值 `true`）、四个 TimingScope（用默认值：activation=Rank，其余=PseudoChannel）。这体现了 designated initializer 的省略能力。

### 3.5 HBM3 条目（第 32-46 行）

```cpp
{
    .standard = DramStandard::Hbm3,
    .family = DramFamily::Hbm,
    .canonical_key = "hbm3",
    .aliases = {"hbm3", "", ""},
    .alias_count = 1,
    .default_timing_profile = "hbm3_generic",
    .default_speed_bin_mbps = 6400,   // 6.4 Gbps/pin
    .default_density_gb = 16,          // 16 Gb die
    .default_stack_height = 8,         // 8-high
    .hbm_edge_pairing_matrix = "hbm3_row_col_pre_pairing",
    .hbm_sid_mapping = "single_sid",
    .hbm_ras_policy = "counter_only",
},
```

与 HBM4 的关键差异：
- **`supports_rfm` 未写**（默认 `false`）——HBM3 不启用 RFM
- **`supports_ecc` 未写**（默认 `false`）——HBM3 不启用 ECC（`hbm_ecc_scheme` 用默认 `"none"`）
- `hbm_sid_mapping = "single_sid"` —— HBM3 单 SID
- `hbm_ecc_scheme` 未写 —— 默认 `"none"`

这些差异全部通过"省略字段用默认值"表达，不需要显式写 `false`。这是这个设计的优雅之处——**默认值即最小能力**，只有增强时才写 `true`。

### 3.6 LPDDR6 条目（第 47-63 行）

```cpp
{
    .standard = DramStandard::Lpddr6,
    .family = DramFamily::Lpddr,
    .canonical_key = "lpddr6",
    .aliases = {"lpddr6", "lpddr", "ldppr"},
    .alias_count = 3,                 // 三个都参与匹配
    .default_timing_profile = "lpddr6_jedec_10667_16gb",
    .default_mode_profile = "dvfsl_linkprot_off_bl24",   // 注意：这里指定了
    .default_speed_bin_mbps = 10667,  // 10.667 Gbps/pin
    .default_density_gb = 16,
    .default_stack_height = 0,        // LPDDR 无堆叠
    .supports_rfm = true,             // LPDDR6 支持 RFM（LPDDR5 不支持！）
    .lpddr_dual_bank_refresh = true,  // 双 bank refresh（REFdb）
    .lpddr_mode_register_profile = "RLSet1_WLSetA",  // mode register 预设
    .activation_scope = TimingScope::PseudoChannel,  // 激活约束按 pseudo-channel
},
```

**三个别名** `{"lpddr6", "lpddr", "ldppr"}` 是兼容性设计——旧配置和用户习惯可能写 `--standard lpddr` 或笔误 `ldppr`，都能映射到 LPDDR6。

`alias_count = 3` 与 HBM4 的 `2`、HBM3/LPDDR5 的 `1` 形成对比——每个标准实际有几个别名，就写几。

### 3.7 LPDDR5 条目（第 64-78 行）

```cpp
{
    .standard = DramStandard::Lpddr5,
    .family = DramFamily::Lpddr,
    .canonical_key = "lpddr5",
    .aliases = {"lpddr5", "", ""},
    .alias_count = 1,
    .default_timing_profile = "lpddr5_generic",
    .default_speed_bin_mbps = 6400,
    .default_density_gb = 16,
    .lpddr_wck_training_required = false,   // 注意：LPDDR5 不需要 WCK training
    .row_bus_scope = TimingScope::Channel,  // 行总线约束按 channel
    .column_bus_scope = TimingScope::Channel,
    .wck_scope = TimingScope::Channel,
},
```

与 LPDDR6 的关键差异：
- `supports_rfm` 未写（默认 false）
- `lpddr_dual_bank_refresh` 未写（默认 false）
- `lpddr_wck_training_required = false`（LPDDR6 默认 true，LPDDR5 显式关掉）
- 三个 TimingScope 改成 `Channel`（LPDDR6 用 `PseudoChannel`）

### 3.8 alias_count 与 aliases 数组的关系（重要设计细节）

```cpp
std::array<std::string_view, 3> aliases{};   // 固定 3 个槽位
std::size_t alias_count = 0;                 // 实际有效的数量
```

为什么既要有数组又要有数量？因为 `std::array` 的大小是编译期固定的，但每个标准实际别名数量不同（1~3 个）。`alias_count` 告诉遍历函数"只查前 N 个槽位"，剩余的 `""` 是占位符。

为什么不把 `""` 也参与匹配？因为 `""` 是无效标准名，如果参与匹配，`--standard ""` 会被静默解析成 HBM4（数组第一个）——这很危险。`alias_count` 精确控制匹配范围，杜绝了这种误匹配。

### 3.9 为什么 LPDDR5 的 `""` 占位符比 HBM4 多？

`std::array` 是固定大小的，所有条目必须有相同的 3 个元素。LPDDR5 只有 1 个真实别名，剩下两个填 `""`。HBM4 有 2 个真实别名（`hbm4`、`hbm`），填一个 `""`。

---

## 四、normalized_name — 标准名归一化（第 81-86 行）

```cpp
std::string normalized_name(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return name;
}
```

**作用**：把传入的标准名全部转成小写。`"HBM4"`、`"hbm4"`、`"Hbm4"` 都变成 `"hbm4"`，然后与目录中的小写 alias 比较。

**三个细节**：
1. **lambda 参数是 `unsigned char` 而非 `char`**：C 标准规定 `std::tolower` 的实参必须可以表示为 `unsigned char`（或 `EOF`）。直接传 `char` 在有符号平台（`char` 为 `-128~127`）上是未定义行为——非 ASCII 字节（如 UTF-8 中文）会触发。这是 C++ 社区常见的未定义行为陷阱，这里显式规避。
2. **返回值是 `std::string` 而非引用**：值传递 + 值返回，调用方拥有独立字符串。代价是一次拷贝，但 `find_standard_traits` 只被调用一次（仿真启动时），性能无关紧要。
3. **`static_cast<char>` 的显式转换**：`std::tolower` 返回 `int`，转回 `char` 需要显式 cast 避免警告。

---

## 五、find_standard_traits — 目录查询（第 90-100 行）

```cpp
const StandardTraits& find_standard_traits(const std::string& name) {
  const std::string key = normalized_name(name);
  for (const auto& traits : kStandardTraits) {
    for (std::size_t i = 0; i < traits.alias_count; ++i) {
      if (traits.aliases[i] == key) {
        return traits;
      }
    }
  }
  throw std::invalid_argument("unsupported standard: " + name);
}
```

**算法**：双重循环。外层遍历 4 个标准，内层遍历每个标准的 `alias_count` 个别名，逐一比较。

**返回类型是 `const StandardTraits&`（引用）而非值拷贝**：traits 数组是编译期常量，返回引用零拷贝。调用方（`make_spec_draft`）不会修改它。

**找不到时抛异常而非返回空**：`throw std::invalid_argument("unsupported standard: hbm9")`。这个错误信息会直接出现在用户终端，帮助定位拼写错误。

**时间复杂度 O(4×3) = O(1)**：目录只有 4 个标准，常量时间。

---

## 六、apply_standard_traits — traits → DramSpec 骨架（第 102-151 行）

这是本文件最核心的函数：把一份 `StandardTraits` 展开成一个完整的 `DramSpec` 骨架。逐段解释：

### 6.1 重置 spec（第 103 行）

```cpp
spec = DramSpec{};
```

**为什么必须重置？** `DramSpec` 有几十个字段（org、timing、timing table、constraints、各种模式开关）。如果不重置，上次调用残留的字段会污染新 spec。例如先 `apply_standard_traits(spec, HBM3_traits)` 再 `apply_standard_traits(spec, HBM4_traits)`，如果不重置，HBM3 的 timing 残留还在。`DramSpec{}` 值初始化把所有字段设为默认值（数值为 0，字符串为空，枚举为默认）。

### 6.2 身份字段（第 104-112 行）

```cpp
spec.standard = traits.standard;
spec.family = traits.family;
spec.name = std::string(traits.display_name);
spec.timing_profile = std::string(traits.default_timing_profile);
spec.vendor_profile = "generic";
spec.mode_profile = std::string(traits.default_mode_profile);
spec.speed_bin_mbps = traits.default_speed_bin_mbps;
spec.density_gb = traits.default_density_gb;
spec.stack_height = traits.default_stack_height;
```

注意 `std::string(traits.display_name)` 的转换：traits 里是 `string_view`（指向字符串字面量），DramSpec 里是 `std::string`（可变的动态字符串）。这里必须拷贝——因为 spec 的字符串可以被后续配置覆盖（如 `--name custom`）。

**`vendor_profile = "generic"` 是硬编码默认**：所有标准的默认 vendor 标签都是 `generic`，表示"未指定具体厂商"。这是来源治理的一部分——报告里 `vendor_profile : generic` 提醒用户这不是任何真实厂商数据。

### 6.3 家族派生的协议特征（第 114-116 行）

```cpp
spec.lpddr_family = traits.family == DramFamily::Lpddr;
spec.dual_command_bus = traits.family == DramFamily::Hbm;
spec.split_activate = traits.family == DramFamily::Lpddr;
```

三个关键架构开关**从 family 自动派生**，而不是每个标准手写：

| 特征 | HBM 家族 | LPDDR 家族 |
|------|---------|-----------|
| `dual_command_bus`（行列双总线） | true | false |
| `split_activate`（ACT1→ACT2） | false | true |
| `lpddr_family` | false | true |

这是一个抽象层次的设计决策：**家族属性在 `DramFamily` 枚举层面定义一次，所有家族成员自动继承**。将来新增 HBM5 只需要 `family = Hbm`，双总线自动启用。

### 6.4 能力开关（第 117-121 行）

```cpp
spec.supports_refresh = true;                       // 所有标准都支持 refresh
spec.supports_rfm = traits.supports_rfm;            // 仅 HBM4/LPDDR6
spec.supports_ecc = traits.supports_ecc;            // 仅 HBM4
spec.lpddr_dual_bank_refresh = traits.lpddr_dual_bank_refresh;  // 仅 LPDDR6
spec.lpddr_wck_training_required = traits.lpddr_wck_training_required;
```

`supports_refresh = true` 是硬编码——四种标准都支持 refresh，不需要 traits 字段。其余三个从 traits 拷贝。

### 6.5 HBM 特征（第 123-131 行）

```cpp
spec.hbm_edge_pairing = traits.family == DramFamily::Hbm;  // HBM 自动启用
spec.hbm_strict_edge_pairing = true;                        // 硬编码严格模式
spec.hbm_edge_pairing_matrix = std::string(traits.hbm_edge_pairing_matrix);
spec.hbm_sid_mapping = std::string(traits.hbm_sid_mapping);
spec.hbm_ecc_scheme = std::string(traits.hbm_ecc_scheme);
spec.hbm_ras_policy = std::string(traits.hbm_ras_policy);
spec.hbm_link_crc_mode = "off";            // 默认关闭 link CRC
spec.hbm_link_retry_enabled = false;       // 默认关闭 link retry
spec.hbm_ecc_bits_per_request =
    traits.standard == DramStandard::Hbm4 ? 16 : 0;
```

**`hbm_strict_edge_pairing = true` 硬编码**：所有 HBM 默认严格 edge pairing 检查。这是项目对 HBM 协议理解的选择——研究模式下宁可严格。

**`hbm_link_crc_mode = "off"` 和 `hbm_link_retry_enabled = false` 硬编码**：link CRC/retry 是 HBM 的先进特性，默认关闭。这是"最小默认"原则——不开启默认值，除非显式配置。

**`hbm_ecc_bits_per_request = HBM4 ? 16 : 0`**：这是 traits 层中少见的数值判断。它与 `hbm_ecc_scheme = "metadata_16b_per_32B_transaction"` 一致——每 32B 事务 16 bit ECC metadata。为什么在 traits 层判断而不是 traits 字段？因为 16 bit 是 HBM4 的固有协议特征，直接内联比在 traits 中再加一个字段更简洁。这是设计权衡：**协议身份级数值（ECC bit 数）可以内联，器件级数值（timing）必须进 profile**。

### 6.6 LPDDR 特征（第 133-145 行）

```cpp
spec.lpddr_mode_register_profile = std::string(traits.lpddr_mode_register_profile);
if (traits.standard == DramStandard::Lpddr6) {
    spec.lpddr_dvfs_mode = LpddrDvfsMode::Nominal;             // DVFS 默认 nominal
    spec.lpddr_wck_mode = LpddrWckMode::CasSync;               // WCK 默认 CAS-sync
    spec.lpddr_wck_ratio = 4;                                  // WCK:CK = 4:1
    spec.lpddr_wck_training_mode = "startup_and_dvfs_retrain"; // 启动+DVFS 后重训
    spec.lpddr_dvfs_transition_policy = "idle_channel_nacu_guarded";
} else if (traits.standard == DramStandard::Lpddr5) {
    spec.lpddr_wck_training_mode = "cas_sync_only";            // 仅 CAS-sync 训练
    spec.lpddr_dvfs_transition_policy = "disabled";            // LPDDR5 无 DVFS
}
spec.lpddr_link_protection_mode = "off";
spec.lpddr_low_power_state_policy = "controller_idle";
```

**LPDDR6 与 LPDDR5 的分支差异**：
- LPDDR6 支持 DVFS（`Nominal` 模式 + `startup_and_dvfs_retrain` 训练 + `idle_channel_nacu_guarded` 过渡策略）
- LPDDR5 不支持 DVFS（`disabled`）+ 只做 CAS-sync 训练

为什么用 `if/else if (traits.standard == ...)` 而不是 traits 字段？因为 DVFS/WCK 的默认值是"每个标准一组组合值"（5 个字段一起），用 traits 字段逐个表达反而繁琐。这是设计权衡：**组合默认值用代码分支，独立默认值用 traits 字段**。

### 6.7 Timing 作用域（第 147-150 行）

```cpp
spec.activation_scope = traits.activation_scope;
spec.row_bus_scope = traits.row_bus_scope;
spec.column_bus_scope = traits.column_bus_scope;
spec.wck_scope = traits.wck_scope;
```

四个 TimingScope 直接拷贝。这四个字段决定了 timing 约束的**粒度**：

| Scope | 含义 | HBM4 默认 | LPDDR5 默认 |
|-------|------|----------|------------|
| `activation_scope` | ACT 的时序约束范围 | Rank | Rank / PseudoChannel(LPDDR6) |
| `row_bus_scope` | 行总线命令约束范围 | PseudoChannel | Channel |
| `column_bus_scope` | 列总线命令约束范围 | PseudoChannel | Channel |
| `wck_scope` | WCK/CAS 约束范围 | PseudoChannel | Channel |

例如 `column_bus_scope = PseudoChannel` 意味着：同一 pseudo-channel 上的列命令要满足 nCCD 约束，而不同 pseudo-channel 之间不受 nCCD 限制——这正确表达了 HBM 的物理特性（不同 PC 有独立数据路径）。

---

## 七、make_spec_draft — 工厂函数（第 153-157 行）

```cpp
DramSpec make_spec_draft(const std::string& name) {
  DramSpec spec;
  apply_standard_traits(spec, find_standard_traits(name));
  return spec;
}
```

**两行代码完成第一阶段**：查目录（`find_standard_traits`）→ 展开骨架（`apply_standard_traits`）。

**为什么叫 draft（草稿）？** 因为返回的 spec 还**不完整**——Timing 全是默认值（0），Organization 是空的，timing table 和 constraints 还没构建。CLI 拿到 draft 后：
1. 应用 profile（`apply_standard_timing_profile`）填充时序数值
2. 应用配置文件覆盖
3. 应用命令行覆盖
4. 调用 `finalize_spec()` 构建 timing table + constraints

**为什么返回对象而不是引用？** 调用方需要一份独立的、可修改的 spec。返回按值 + 移动语义（C++17 保证 RVO），零拷贝开销。

---

## 八、supported_specs — 标准清单（第 159-166 行）

```cpp
std::vector<std::string> supported_specs() {
  std::vector<std::string> names;
  names.reserve(kStandardTraits.size());   // 预分配 4 个槽位，避免多次 rehash
  for (const auto& traits : kStandardTraits) {
    names.emplace_back(traits.canonical_key);
  }
  return names;
}
```

**作用**：返回所有支持的规范名（`hbm4`, `hbm3`, `lpddr6`, `lpddr5`）。CLI 的 `--help` 输出用它列出可选标准，错误信息用它提示可用标准。

**为什么用 canonical_key 而不是 display_name？** `canonical_key` 是"程序接受的输入值"（小写，可配置），`display_name` 是"给人看的"（大写）。`supported_specs()` 返回的是可以继续传给 `make_spec_draft()` 的值，所以必须用 canonical_key。

`names.reserve(kStandardTraits.size())` 是微优化——预分配容量避免 `emplace_back` 时的多次堆分配。

---

## 九、StandardTraits 结构体定义（spec.hpp 第 241-270 行）

```cpp
struct StandardTraits {
  DramStandard standard = DramStandard::Unknown;   // 身份
  DramFamily family = DramFamily::Hbm;             // 家族
  std::string_view canonical_key;                  // 规范名
  std::string_view display_name;                   // 显示名
  std::array<std::string_view, 3> aliases{};       // 别名（最多 3 个）
  std::size_t alias_count = 0;                     // 实际别名数量

  std::string_view default_timing_profile;         // 默认 profile 名
  std::string_view default_mode_profile = "default"; // 默认 mode 名
  int default_speed_bin_mbps = 0;                  // 默认速率
  int default_density_gb = 0;                      // 默认密度
  int default_stack_height = 0;                    // 默认堆叠

  bool supports_rfm = false;                       // 协议能力
  bool supports_ecc = false;
  bool lpddr_dual_bank_refresh = false;
  bool lpddr_wck_training_required = true;

  std::string_view hbm_edge_pairing_matrix = "hbm4_core_pre_pairing";
  std::string_view hbm_sid_mapping = "stack_height_div4";
  std::string_view hbm_ecc_scheme = "none";
  std::string_view hbm_ras_policy = "metadata_only";
  std::string_view lpddr_mode_register_profile = "default";

  TimingScope activation_scope = TimingScope::Rank;      // 时序作用域
  TimingScope row_bus_scope = TimingScope::PseudoChannel;
  TimingScope column_bus_scope = TimingScope::PseudoChannel;
  TimingScope wck_scope = TimingScope::PseudoChannel;
};
```

### 9.1 字段分组

| 分组 | 字段 | 设计意图 |
|------|------|---------|
| 身份 | standard, family, canonical_key, display_name | 标准是谁 |
| 别名 | aliases, alias_count | 如何被查找 |
| Profile 选择 | default_timing_profile, default_mode_profile, speed/density/stack | 默认用哪个 profile |
| 协议能力 | supports_rfm, supports_ecc, dual_bank_refresh, wck_training_required | 支持什么特性 |
| HBM 特征 | edge_pairing_matrix, sid_mapping, ecc_scheme, ras_policy | HBM 协议细节 |
| LPDDR 特征 | lpddr_mode_register_profile | LPDDR mode register 预设 |
| Timing 作用域 | activation/row_bus/column_bus/wck_scope | 时序约束粒度 |

### 9.2 为什么 `lpddr_wck_training_required` 默认是 true？

结构体声明中 `bool lpddr_wck_training_required = true`，但 LPDDR5 条目显式写 `false`。设计意图是：**LPDDR 系列默认需要 WCK 训练**（这是协议要求），LPDDR5 作为例外显式关闭。默认值表达"多数情况"，例外显式覆盖。

### 9.3 为什么没有 `supports_refresh` 字段？

`supports_refresh` 在 `apply_standard_traits` 中被硬编码为 `true`——所有四种标准都支持 refresh，不需要在 traits 中可配置。这是"**常量行为不进入数据**"的设计原则——只有当某个标准与其他标准不同时才需要字段。

### 9.4 为什么没有 `dual_command_bus` / `split_activate` 字段？

同样从 `family` 派生（见 6.3），不进入 traits。

---

## 十、设计原则总结

### 原则 1：身份与数值分离

```
traits 管：标准是什么（身份）、支持什么（能力）、默认用哪个 profile（选择）
profiles 管：时序是多少（数值）
spec.cpp 管：如何构建表（结构）
```

这是整个 DRAM 规格系统最重要的分层。任何"哪个标准的 timing 是多少"的问题，答案都在 profiles.cpp / configs/profiles/，不在 standard_traits.cpp。

### 原则 2：家族派生 vs 标准分支

- **家族级特征**（双总线、split activate、edge pairing）→ `family` 枚举派生
- **标准级特征**（DVFS 支持、WCK ratio、ECC bit 数）→ `if (standard == ...)` 分支
- **条目级特征**（supports_rfm 等）→ traits 字段

三级抽象，每级只处理自己级别的问题。

### 原则 3：默认值即最小能力

- HBM3 不写 `supports_rfm` → 默认 false（不支持）
- 只有 HBM4/LPDDR6 写 `true`（增强）
- 例外情况显式覆盖（LPDDR5 的 `wck_training_required = false`）

### 原则 4：constexpr 零开销

整个目录是编译期常量，运行时零初始化、零分配、只读。查找 O(1)。

### 原则 5：aliases 兼容旧配置

`lpddr`、`ldppr`、`hbm` 等历史别名被保留，旧实验脚本无需修改。

---

## 十一、扩展指南：新增一个标准

以新增 LPDDR7 为例：

```cpp
// 1. spec.hpp 的 DramStandard 枚举加一项
enum class DramStandard { Unknown, Hbm3, Hbm4, Lpddr5, Lpddr6, Lpddr7 };

// 2. standard_traits.cpp 的 kStandardTraits 加一个条目
{
    .standard = DramStandard::Lpddr7,
    .family = DramFamily::Lpddr,
    .canonical_key = "lpddr7",
    .aliases = {"lpddr7", "", ""},
    .alias_count = 1,
    .default_timing_profile = "lpddr7_jedec_...",
    .default_speed_bin_mbps = 12800,
    ...
},

// 3. configs/profiles/lpddr7/ 加 profile 文件
// 4. 如果需要新协议行为，才改 apply_standard_traits 的分支
// 5. 通常无需修改 state.cpp / executor.cpp / timing.cpp
```

新增一个标准的成本 = 1 个枚举项 + 1 个数组条目 + 1 个 profile 文件。协议行为相同的标准**零代码改动**——这正是 StandardTraits 工厂模式的扩展性优势，与 Ramulator2.1 的 Python DSL 相比，代价更低（无需 codegen）。
