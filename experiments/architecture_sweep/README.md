# 定制化 HBM/LPDDR 架构实验

该实验只通过两份权威 master 配置加临时 `[override]` 改变模型，不维护第三份入口
配置。每个 case 以**该标准自己的完整标准组织**为基线，只在**一个维度**上扰动：

- bank 组：只改 `banks_per_group`（half / standard / double）；
- geometry 组：只改 `columns`（low / mid / high）；
- refresh 组：只改 `refresh_policy`（`per_bank` / `all_bank`）。

`channels/pseudo_channels/sids/ranks/bank_groups` 始终保持标准值，因此每个标准的
lane 数（channels×pseudo_channels×sids×ranks）与 bank 组织形状都来自标准本身。
基线不是硬编码的，而是每个标准跑一次 `--check-config --dump-resolved-config`（约 7 ms，
不跑仿真）解析出来的，保存在 `<out>/<standard>/baseline.cfg`。这一点是刻意的：
容量门禁要拿 Python 按公式算出的容量与引擎报告的 `aggregate_capacity_bytes` 比对，
如果两边都来自同一张硬编码表，检查就成了循环论证。

## 标准基线与各 case 容量

| | HBM3 | HBM4 | LPDDR5 | LPDDR6 |
|---|---|---|---|---|
| 标准组织（ch×pc×sids×ranks×bg×bpg×rows×cols） | 16×2×2×1×4×4×16384×32 | 32×2×2×1×2×8×16384×32 | 1×1×1×1×4×4×65536×64 | 1×2×1×1×4×4×65536×64 |
| 基线容量 | 16 GiB | 32 GiB | 2 GiB | 4 GiB |
| bank 组 | 8 / 16 / 32 | 16 / 32 / 64 | 1 / 2 / 4 | 2 / 4 / 8 |
| geometry 组 | 16 / 24 / 32 | 24 / 32 / 64 | 1 / 2 / 4 | 2 / 4 / 8 |
| refresh 组 | 16（=基线） | 32 | 2 | 4 |

容量 = `channels × pseudo_channels × sids × ranks × bank_groups × banks_per_group ×
rows × columns × dram_transaction_bytes`，**不含 `stack_height`**。上表单位均为 GiB。

geometry 组三点由目标容量反推，**没有统一的缩放规则**：HBM3 是 1.0/1.5/2.0 倍基线，
HBM4 是 0.75/1.0/2.0 倍，LPDDR5/6 是 0.5/1.0/2.0 倍。HBM 侧 `columns` 偏离标准
BL8 组织中 CA[4:0] 对应的 32，属于 research 型组织偏离，不是器件值。

## 为什么是 columns 而不是 rows

`columns` 是"一个行能连续接纳多少次访问"这个量本身，因而是唯一能驱动行命中/冲突的
几何维度；`rows` 只决定地址在何处回绕，在顺序 trace 下不改变局部性。实测把 geometry
组改成只扫 rows（12288/16384/32768）时，三个 case 的 cycles、行命中率、冲突率和带宽
**逐位相同**——门禁会空洞通过。因此该组扫 columns。

## 定向 trace

三组使用不同的定向 trace，而不是给所有配置复用一份随机流量：

- **bank**：请求同时到达并轮转全部 `(lane, bank)`；同一 bank 每轮访问一个从未使用过的
  row，避免 row wrap 产生的 FR-FCFS 命中把"可并行 bank 数"与"调度器重排行命中"
  混在一起。`lane = index % lanes` 与被扫的 `banks_per_group` 无关，保证三个 case
  呈现给机器的 lane 数与每 lane 请求数相同，唯一差别是 bank 复用。
- **geometry**：固定在单 lane 上顺序扫描，footprint 恰为 `--requests` 条事务，
  只让行内/跨行的拆分比例随 `columns` 变化。该组带宽只是单 lane 值，约为设备总带宽的
  `1/lanes`，**不可跨标准或对峰值比较**，门禁只看 hit/conflict 趋势。
- **refresh**：跨 lane 铺开、行数受限以保持页打开、持续到达；两个 case 使用**完全相同**
  的 trace，使 `refresh_policy` 成为唯一变量。

定向地址按 `AddressMapper::Default` 的完整 8 级顺序编码，低位到高位为
`column, bank, bank_group, pseudo_channel, sid, rank, channel, row`，byte address 首先
除以 `dram_transaction_bytes`（32 B）得到 line。**不要把 64 B host line 当成列步长**；
每个 host request 仍可在 frontend 中拆成两个 32 B transaction。

## channel_mapper 为什么改成 decoded

`configs/hbm.cfg` 默认 `channel_mapper = xor`，请求进哪个 channel 由
`(line ^ (line>>6) ^ (line>>12)) % channels` 决定，会**覆盖**地址解出的 `channel` 字段。
该哈希多对一、没有闭式反解，要定位通道只能在 `row` 上搜索候选值，而 `row` 正是几何
实验要测的变量，会把几何效应与定位所需的行偏移混在一起；在 Python 里复刻它还会与
C++ 内部实现静默耦合。因此本实验显式写 `channel_mapper = decoded`（LPDDR 本来就
是 `decoded`，不受影响）。代价是结果不再代表 HBM 主配置默认的 xor 路由行为，
这是为定向隔离而改的口径，已在 `resolved.cfg` 与 `result.json` 中逐 case 记录。

## 请求数

`--requests` 控制 geometry 与 refresh 组，默认 512。bank 组**单独**按标准 bank 总数
取请求数（可用 `--bank-requests` 覆盖，0 表示自动推导）：固定 512 请求时 HBM4 的
128 条 lane 每 lane 只有 4 个请求，bank 并行度超过 4 就完全不可分辨。

## 自动门禁

| 检查 | 判定 |
|---|---|
| `completion` | cycles>0、未撞 cycle limit、`data_mismatches=0` |
| `capacity` | Python 按容量公式算出的组织容量 **严格等于** 引擎报告的 `aggregate_capacity_bytes`（全为整数，无浮点容差） |
| `organization drift` | 每个 case 的 `resolved.cfg` 只在它声明的字段上偏离基线——把"单维度扰动"变成可执行断言 |
| `baseline stability` | `bpg_standard` 逐字段复现解析出的基线 |
| `builtin org fixture` | 模块内 `STANDARD_ORGS`（smoke fixture）与解析出的基线一致 |
| `density basis` | `density_gb` 与容量口径的关系固定下来，防止被当容量读 |
| `address encoding` | 用 `--cmd-trace` 导出的显式 DRAM 坐标校验 Python 侧地址编码与引擎解码一致 |
| `bank scaling` | 按每 lane 实际可用的 bank 并行度（engagement）归一，只比较覆盖到全部 bank 的配置；engagement 分不出档时显式标注"不可分辨"而不是给出趋势 |
| `geometry trend` | `columns` 增大时行命中率不降、冲突率不升 |
| `refresh scope` | per-bank case 必须有 PB 批且 AB 批为 0，all-bank case 反之 |

`density_gb` 是 Gibit 口径（HBM 按每 die、LPDDR 按每通道子通道），**不是容量**；
容量一律读 `aggregate_capacity_bytes`。bank scaling 允许 1% 输出精度余量，不要求三点
逐点严格单调——可用 bank 增加后命令/数据总线可能先饱和。任一检查失败脚本返回非零。

## 用法

```bash
# 默认扫描当前两种新标准 HBM4 和 LPDDR6
python3 experiments/architecture_sweep/run.py

# 统一复跑四标准
python3 experiments/architecture_sweep/run.py \
  --standards hbm3,hbm4,lpddr5,lpddr6

# 快速查看 HBM4 工具链
python3 experiments/architecture_sweep/run.py --standards hbm4 \
  --out outputs/experiments/architecture_sweep_quick
```

单 case 默认 120 秒超时，可用 `--case-timeout` 调整。输出包含 `results.csv`、
`checks.csv`、`summary.md`、离线 `trends.html`，以及每个 case 的 `workload.trace`、
`resolved.cfg`、`stats.txt` 与每个标准的 `baseline.cfg`、`probe/cmd_trace.csv`。

刷新组覆盖的短 `nREFI/nREFIpb/nRFC/nRFCpb` 标记为 `research`：其中
`nREFI/nREFIpb` 取 32 是为了让维护批次数在短窗口内可观测（覆盖到 64 请求的 smoke），
**不是物理间隔**，只用于压力验证，不替代 master 中的 JEDEC/reference timing。

注意：聚合文件（`results.csv` 等）按本次运行的标准重写，但各标准的 case 子目录**不会
被清理**。先跑四标准再跑单标准，旧标准的目录会留在磁盘上而聚合表里没有它们，
容易误读成"结果还在"。

结果属于模型参数敏感性研究，不代表某款器件。
