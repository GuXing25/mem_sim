# HBM3/HBM4/LPDDR5/LPDDR6 多 Stack 工程 Demo

这里的四个脚本复用两份多实例用例配置：
[`configs/usecases/hbm_nstacks.cfg`](../../configs/usecases/hbm_nstacks.cfg) 和
[`configs/usecases/lpddr_nstacks.cfg`](../../configs/usecases/lpddr_nstacks.cfg)。两份文件通过
`[meta] extends` 继承对应的标准主配置，只保存多实例场景的差异参数，避免复制整套
标准参数后产生漂移。

```bash
bash examples/multistack_demos/hbm3_nstack.sh
bash examples/multistack_demos/hbm4_nstack.sh
bash examples/multistack_demos/lpddr5_nstack.sh
bash examples/multistack_demos/lpddr6_nstack.sh
```

默认运行 2 个独立 Stack。以下命令把同一 HBM4 demo 扩为 6 Stack；共享 trace 只访问
Stack 0/1，其余 Stack 空闲是预期行为。

```bash
STACK_COUNT=6 bash examples/multistack_demos/hbm4_nstack.sh
```

每次运行在 `outputs/demos/<standard>_<N>stack/` 生成：完整生效配置、stdout 统计、
command/response/DFI/DFI-signal CSV、带来源的 timing 表、每个 Stack 的热网格和一个
可离线打开的 `dashboard.html`。`resolved.cfg` 是本次实验全部生效参数的审计快照；
参数含义、联动和默认值详见根目录《堆叠存储模型交付手册》的配置章节。

四个 demo 的协议/组织默认值如下；`banks` 是每颗存储实例全部 channel/PC/SID 的
聚合 bank 数，`data_bus_bits` 是一颗实例的接口总位宽：

| demo | rate Mb/s/pin | tCK ps / tick× | bus bits | ch / PC / SID | banks | host line / transaction | 物理含义 |
|---|---:|---:|---:|---:|---:|---:|---|
| HBM3 | 6400 | 625 / 2 | 1024 | 16 / 2 / 2 | 1024 | 64 / 64 B | 8Hi HBM Stack 研究基线 |
| HBM4 | 8000 | 500 / 2 | 2048 | 32 / 2 / 2 | 2048 | 64 / 32 B | 8Hi full-stack，host line 拆成两个 transaction |
| LPDDR5 | 6400 | 1250 / 1 | 16 | 1 / 1 / 1 | 16 | 64 / 64 B | 两个独立 LPDDR5 device 实例 |
| LPDDR6 | 10667 | 375 / 1 | 24 | 1 / 2 / 1 | 32 | 64 / 32 B | 两个独立 LPDDR6 device 实例，含 WCK/REFdb 能力 |

两份 `*_nstacks.cfg` 的共有值是 `stack_count=2`、256 B interleave、每 Stack 256 项
ingress、每拍最多 dispatch 4 个 transaction、strict-priority QoS、Behavioral PHY、
sparse backend，并开启 command/DFI 验证。四个包装脚本可以覆盖 `STACK_COUNT`，但不会
修改 standard 的组织/timing。若要研究 scheduler、row policy、ECC、backend、PHY、功耗
或热参数，可复制对应的用例配置后修改 `[override]`，也可通过 CLI 临时覆盖，并以运行目录
中的 `resolved.cfg` 确认最终生效值。

可覆盖变量：`HBM_SIM_BIN` 指定二进制，`STACK_COUNT` 指定 Stack 数，
`OUTPUT_ROOT` 指定产物根目录。未知标准、Stack 数小于 2 或不存在的可执行算法都会
立即报错，不会静默回退。
