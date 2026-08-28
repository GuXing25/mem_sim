# 配置模型

项目对普通使用者只提供两个主入口：

- `configs/hbm.cfg`：HBM 家族，选择 HBM3 或 HBM4。
- `configs/lpddr.cfg`：LPDDR 家族，选择 LPDDR5 或 LPDDR6。

它们同时包含系统拓扑、workload、控制器、调度、行策略、地址映射、PHY、
organization、timing、刷新/RFM、ECC、存储后端、功耗、热模型和输出参数。
仓库内可执行 `.cfg` 只有这两个；校准、差分和演示口径均为其中的命名 preset。
`profile_index.csv` 记录 preset 的来源与声明边界，`validation/*.csv` 仅是审计元数据。

## 1. 最常用的命令

```bash
# 检查配置，不运行流量
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm4 --check-config

# 运行 HBM3、HBM4、LPDDR5 或 LPDDR6
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm3 --requests 10000
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm4 --requests 10000
./build-clang-debug/hbm_sim --config configs/lpddr.cfg \
  --standard lpddr5 --requests 10000
./build-clang-debug/hbm_sim --config configs/lpddr.cfg \
  --standard lpddr6 --requests 10000

# 多 stack 和自由探索
./build-clang-debug/hbm_sim --config configs/hbm.cfg \
  --standard hbm4 --stack-count 6 --scheduler fcfs
```

配置文件里的 `[model] base_standard` 是默认选择；`--standard` 始终覆盖它。
命令行参数无论写在 `--config` 前还是后，都具有最终优先级。

## 2. 配置分层

schema-v2 的固定合并顺序为：

```text
程序内置默认
  < 公共 section（如 [system]、[controller]）
  < [family.*]
  < [standard.<标准>.*]
  < [preset.<标准>.<名称>.*]
  < [override]
  < 命令行
```

常见 section：

| section | 内容 |
| --- | --- |
| `[model]` | 模型名、基础标准、preset |
| `[validation]` | `exploratory/standard/device` 校验模式 |
| `[system]` | stack 数量、跨 stack 映射、入口队列和 QoS |
| `[workload]` | 请求数、读写比例、trace、注入间隔 |
| `[controller]` | buffer、水位、调度器和行策略 |
| `[controller.scheduler]` | `type = fcfs/frfcfs` |
| `[controller.row_policy]` | `open_page/closed_page/closed_cap` |
| `[mapping]` | DRAM 地址映射和 channel mapper |
| `[phy]` | Direct/Behavioral PHY、DFI、FIFO 和训练流水线 |
| `[architecture]` | 速率、容量、stack height、channel/bank/row/column |
| `[timing.*]` | nCK 或 ns/us timing 及数值来源 |
| `[maintenance]` | refresh、RFM、温度和低功耗策略 |
| `[storage]` | payload backend 与物理拓扑 |
| `[reliability*]` | 接口开销和 payload SECDED |
| `[power]`、`[thermal]` | 功耗与热模型输入参数 |
| `[outputs]` | trace、验证和 dump 路径 |
| `[override]` | 当前研究实验相对 preset 的显式修改 |

例如只想研究一个非标准 HBM4 组织和时序：

```ini
[override]
stack_count = 6
channels = 24
bank_groups = 4
nCL = 28
scheduler = fcfs
```

这会生成一个“以 HBM4 命令/状态语义为基础的自定义模型”，不应称作某个真实
HBM4 器件。建议同步修改 `[model] name`，并保持 `validation_mode = exploratory`。

## 2.1 命名 preset 与四个工程 Demo

```bash
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 --list-presets
./build-clang-debug/hbm_sim --config configs/lpddr.cfg --standard lpddr6 --list-presets
```

- `baseline`：当前标准派生基线。
- `multistack_demo`：四种标准共有的多 Stack 工程入口。
- `ramulator2_reference_1ch`：四种标准的受控单通道外部差分口径。
- HBM4 另含 `validation_native_1ch`、`storage_demo` 和 synthetic vendor 回归分支。
- HBM3 另含 `dramsim3_hbm2_common` 辅助验证分支。
- LPDDR6 另含 `link_protection`、`low_dvfs_4267` 和 synthetic vendor 回归分支。

直接运行四个多 Stack demo：

```bash
bash examples/multistack_demos/hbm3_nstack.sh
bash examples/multistack_demos/hbm4_nstack.sh
bash examples/multistack_demos/lpddr5_nstack.sh
bash examples/multistack_demos/lpddr6_nstack.sh
```

## 3. 基础共有与协议特有参数

共有部分包括系统拓扑、流量、controller buffer、调度、行策略、地址映射、
Behavioral PHY、通用 organization/timing、存储后端、payload ECC、功耗和热模型。

HBM 特有部分包括 SID/PC interleave、双命令总线相关 pairing、link CRC/RAS
metadata，以及 stack/layer/TSV 热拓扑。LPDDR 特有部分包括 split ACT、WCK、
DVFS、DBI、link protection/ECC、CA parity、REFdb 和低功耗行为。

另一协议族的字段可以安全地放在 inactive 的 `[standard.*]` 或 `[preset.*]`
section 中；如果把 `lpddr_*` 键放进正在运行的 HBM `[override]`，程序会直接报
“没有可执行语义”，避免静默忽略。

## 4. 可配置不等于已实现

数值、容量和布尔参数只要在配置白名单中，就可以自由改变；算法名称则必须有
C++ 实现。配置文件不是脚本，不会根据一个陌生字符串自动生成调度器、行策略、
PHY 或存储后端。

当前注册项：

| 类型 | 已实现值 |
| --- | --- |
| scheduler | `fcfs`、`frfcfs` |
| row policy | `open_page`、`closed_page`、`closed_cap` |
| memory backend | `sparse`、`mmap_sparse`、`chunk_file` |
| PHY mode | `direct`、`behavioral` |

查询运行程序自身的注册表：

```bash
./build-clang-debug/hbm_sim --list-schedulers
./build-clang-debug/hbm_sim --list-row-policies
./build-clang-debug/hbm_sim --list-backends
./build-clang-debug/hbm_sim --list-phy-modes
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 --list-presets
```

例如 `type = blis` 而代码没有 BLISS 实现时，配置解析立即失败，错误会带配置
文件和行号，并列出 `fcfs, frfcfs`。这比回退到默认策略更安全，因为结果不会在
用户不知情时使用错误算法。要增加新策略，需要实现枚举/解析、候选排序逻辑、
帮助/注册列表和测试，然后该名称才能出现在配置中。

## 5. 三种可信度模式

| 模式 | 用途 | 行为 |
| --- | --- | --- |
| `exploratory` | 自由架构和参数探索 | 允许偏离 preset；仍拒绝未知算法、无语义字段和非法范围 |
| `standard` | 标准级基线 | 拒绝 `[override]`/CLI 对标准模型参数的偏离；拒绝 research/external timing |
| `device` | 目标器件数值比较 | 包含 standard 检查，并要求具名 `vendor_profile` 和 vendor 来源 timing |

当前 HBM4 的若干 RL/WL 和行时序仍是 research-default，因此它能在
`exploratory` 模式运行，但不能通过 `standard`。HBM3/LPDDR5 的部分基线来自
Ramulator2.1，标为 `external_reference`，同样不能冒充 JEDEC 或厂商数据。
LPDDR6 主配置的当前标准项可以用于 JEDEC 级检查，但 `device` 仍需厂商参数。

## 6. timing 单位和来源

- `nXXX` 是 nCK，例如 `nCL = 30`。
- `tXXX_ns`、`tXXX_us` 由最终 `tCK_ps` 向上取整换算成 nCK。
- `source`/`timing_override_source` 可取 `jedec`、`vendor`、`derived`、
  `external_reference`、`research_default`。
- `external_reference` 表示来自 Ramulator/DRAMsim 等参考实现，不等于标准或器件值。
- 固定 JEDEC/vendor 表必须记录 speed bin、density、stack height/mode 和来源说明。

建议把不同来源放在独立 subsection：

```ini
[standard.hbm4.timing_jedec]
source = jedec
tCCDL_ns = 2.5

[standard.hbm4.timing_research]
source = research_default
nCL = 30
```

## 7. 审计最终生效模型

```bash
# 导出包含内置值、选中标准/preset、公共段、override 和 CLI 的最终快照
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --stack-count 6 --dump-resolved-config outputs/hbm4_resolved.cfg --check-config

# 查看相对 preset 的修改
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --speed-bin-mbps 9000 --compare-preset --check-config

# 解释某个配置文件字段的覆盖链
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --explain-config nCL --check-config
```

`--dump-resolved-config` 的结果可再次作为 `--config` 加载；timing 按来源分组，
因此重新加载后不会丢失 JEDEC/vendor/research provenance。`--check-config` 只完成
解析、分层、模型构造和一致性校验，不产生流量。

## 8. 后端、ECC、功耗和热模型

- `sparse` 只为访问过的 transaction 分配内存，适合通常的仿真。
- `mmap_sparse` 提供 mmap 文件后端。
- `chunk_file` 用有界 chunk cache 访问文件后端。
- `[reliability.payload]` 是实际 payload 上的 SECDED shadow；接口 ECC/metadata
  统计位于 `[reliability]`，二者不是同一层。
- `configured_pj` 使用逐命令能量；`dramsim3_idd` 使用 VDD/IDD 风格公式。
- HBM 配置采用 stack/layer/TSV 参数解释热拓扑；LPDDR 主配置采用较薄的
  device/package 拓扑参数。两者目前仍是行为级研究模型，需要厂商 IDD/封装参数校准。

完整可用键直接查看两个主配置中的注释；CLI 等价项查看 `hbm_sim --help`。

## 9. 自定义实验的定位

解析器仍能读取旧式平面 `key=value`，但仓库不再维护这种副本。新实验不要复制
第三份完整配置；应从 `hbm.cfg` 或 `lpddr.cfg` 选择标准/preset，再通过 master 的
`[override]`、CLI 或由实验工具生成的临时小型 `[override]` 叠加差异。每次运行都用
`--dump-resolved-config` 把最终模型随结果归档。
