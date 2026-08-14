# 多 Stack 系统及存储后端

本文档是当前真实存储区和满负载路径的主文档。根目录 README 只保留概要；backend 的选择、文件布局、流式 trace、覆盖率统计和模型边界以本文为准。

## 1. 要解决的问题

请求压力、工作集大小和地址空间覆盖率是三个不同维度：

```text
高请求压力：每周期持续注入 RD/WR，控制器、bank、总线接近满载。
大工作集：请求分布在较大的 hot region。
满容量覆盖：逻辑存储空间中的大部分 line 都真正写入了 payload。
```

高请求压力不必然导致满容量覆盖。大量带宽实验会在有限工作集内反复访问，
这时稀疏内存仍是合适选择；若真的要写满几十 GB，则应使用文件后端。

## 2. 当前分层

```text
TrafficStream / RequestSource
  按需产生请求，不保存完整 workload

Controller / MemorySystem
  请求队列、调度、ACT/RD/WR/PRE、时序和完成统计

MemoryImage
  row buffer、物理坐标、ECC shadow、功耗、热模型、checkpoint

MemoryBackend
  payload、initialized mask、持久元数据的具体保存方式
```

`MemoryImage` 仍负责协议和堆叠存储语义。切换后端不会改变
ACT/RD/WR/PRE、bank/row/column、row buffer、ECC、功耗或热模型的行为。

## 3. 三种后端

### sparse

- 使用地址哈希表保存已经建立的 line。
- 使用反向哈希表完成 `StorageKey -> address` 查询。
- 适合低/中覆盖率、小工作集反复访问、debug 和普通回归。
- 覆盖率高时，每个哈希节点的额外开销会使宿主内存明显大于原始 payload。

### mmap_sparse

- data、init、meta、presence 都是 sparse file。
- 使用 `mmap` 映射完整逻辑文件，未访问的文件页通常不占实际磁盘块。
- 适合较大工作集以及宿主具有足够 64 位虚拟地址空间的 Linux/WSL 环境。
- 逻辑映射包括 payload、每 byte 初始化位和每 line 元数据；超大容量时虚拟映射
  也会很大，因此 WSL 或受限环境更推荐 `chunk_file`。

### chunk_file

- 使用相同的 sparse file 布局，但只缓存有限个 chunk。
- 默认 chunk 为 2 MiB，默认缓存 16 个 chunk，LRU 淘汰脏 chunk 时写回。
- 宿主内存主要与 `chunk_size * cache_entries` 成正比，不与逻辑容量成正比。
- 适合满容量写入、长时间运行和 WSL 环境。

## 4. 文件布局

指定：

```ini
memory_data_file = outputs/hbm_data.bin
```

默认产生：

```text
outputs/hbm_data.bin          payload，1 byte/逻辑 byte
outputs/hbm_data.bin.init     initialized bitmap，1 bit/逻辑 byte
outputs/hbm_data.bin.meta     固定头 + 每 line 持久元数据
outputs/hbm_data.bin.present  allocated bitmap + written bitmap，各 1 bit/line
```

这些文件是项目内部研究格式，不是 JEDEC 或具体厂商规定的介质格式。首次创建时
会建立逻辑大小；在支持 sparse file 的文件系统中，未写区域不分配实际磁盘块。
重新打开时会校验格式版本、line size 和容量，配置不一致会直接报错。
已分配行和唯一写入行计数保存在 metadata header 中，正常 `flush`/析构后可 O(1)
恢复，不需要在重开时扫描完整 bitmap。该布局不提供断电事务一致性；进程异常终止
后的文件应视为待检查 checkpoint。

## 5. 配置和 CLI

配置文件：

```ini
memory_backend = chunk_file
memory_capacity_bytes = 34359738368
memory_data_file = outputs/hbm_data.bin
memory_chunk_size = 2097152
memory_chunk_cache_entries = 16
sparse_density_warning_pct = 30
topology_stats_scan_limit = 100000
```

等价 CLI：

```bash
./build-cmake/hbm_sim --standard hbm4 \
  --memory-backend chunk_file \
  --memory-capacity-bytes 34359738368 \
  --memory-data-file outputs/hbm_data.bin \
  --memory-chunk-size 2097152 \
  --memory-chunk-cache-entries 16
```

`memory_capacity_bytes = 0` 时，容量由 DRAM 几何中的 transaction 数乘
`dram_transaction_bytes` 推导。HBM4 32Gb/die、8Hi 的当前配置得到 32 GiB。
LPDDR6 两个 16Gb x12 subchannel 的当前配置得到 4 GiB。
显式容量必须是后端块大小的整数倍，访问越界会失败。

HBM4 和 LPDDR6 当前的 frontend `line_size=64`，单命令
`dram_transaction_bytes=32`。`MemoryImage` 和三种 `MemoryBackend` 都按 32B
物理事务块保存数据，一个默认 64B host 请求先拆成两个块。旧版按 64B 后端块
创建的持久文件与新格式不兼容，应更换 `memory_data_file` 路径，或删除 data、
`.init`、`.meta`、`.present` 后重新创建。

`memory_init_file`、`memory_meta_file`、`memory_presence_file` 可以覆盖默认 sidecar
路径。已有文件会作为持久后端继续使用；要开始全新实验，应使用新的 data 路径或
删除 data 及三个 sidecar。

## 6. 流式满负载路径

CLI 使用 `TrafficStream` 逐条生成 synthetic 请求或逐行读取 trace。`BW/BR`
burst 也逐 cache line 展开，因此请求数很大时不会先构造完整 `vector<Request>`。
`--requests N` 在 trace 模式下限制的是展开后的请求数；例如 64B line size 下，
`BW len=512` 会消耗 8 个请求额度。

`BW` 支持 `pattern=request_id|zero|ff`，默认 `request_id` 使用按地址确定的
payload。`BR check=last_write` 会读取 trace frontend 维护的地址级 golden map，
逐地址比较最近一次 `W/BW` 的真实 payload；`check=auto` 有历史写入时等价于
`last_write`，否则回退到默认 deterministic payload。

输入必须按 `inject_cycle` 非递减排列。控制器只保留当前待注入请求，buffer 满时
通过 backpressure 推迟实际 `arrival`，所以高压请求不会因流式读取而跳过队列压力。

默认 CLI 不保留完整 command/DFI 轨迹。只有指定 `--cmd-trace`、
`--dfi-trace`、`--dfi-signal-trace` 或 `--validate-cmd-trace` 时才保留相关事件；
显式导出超长轨迹仍会消耗与命令数相关的内存，这是当前可审计模式的边界。

## 7. 覆盖率与统计

关键输出：

```text
total_addressable_lines
storage_lines_allocated
unique_written_lines
storage_density_pct
storage_backend_recommendation
storage_topology_lines_scanned
storage_topology_scan_skipped
```

`storage_density_pct` 的口径是：

```text
unique_written_lines / total_addressable_lines * 100%
```

它衡量真实写覆盖率，不等于请求利用率、带宽利用率或 row-buffer 命中率。

详细 stack/channel/bank/row/cell 去重统计需要枚举后端地址并为多个层级建立集合。
默认只在后端地址枚举成本不超过 100000 line 时执行：`sparse` 的枚举成本是已分配
行数，两个 bitmap 文件后端的枚举成本是逻辑 line 数。超过后
`storage_topology_scan_skipped = 1`，但容量、已分配行、唯一写入行和覆盖率仍然
精确。设置 `topology_stats_scan_limit = 0` 可以强制完整扫描，但满容量实验不建议。

## 8. 选择建议

```text
低/中覆盖率，重点研究控制器与数据正确性：
  sparse

工作集较大，需要文件持久化，虚拟地址空间充足：
  mmap_sparse

满容量、长期运行、WSL 或希望限制宿主内存：
  chunk_file
```

覆盖率阈值只是提示，不会在运行中自动迁移后端。自动迁移会引入不可控的 I/O
突发和实验口径变化，因此后端由配置或 CLI 明确选择。

## 9. 模型边界

- 后端解决的是 payload、初始化位和 line 元数据的可扩展保存，不是 DRAM
  transistor/cell 电气仿真。
- 稀疏文件的实际磁盘占用依赖宿主文件系统；可用 `du -h` 查看已分配块，
  `ls -lh` 显示的是逻辑大小。
- RL/WL、行时序、RFM/RAS/ECC、训练和功耗数值仍受 JEDEC、研究默认值和 vendor
  数据边界约束，详见 [审计](审计.md)。
- `.bin` memory image 是可移植的稀疏 checkpoint；file-backed 四文件布局是运行时
  后端，不承诺跨版本兼容。

## 10. 主动多 Stack 系统

`MemorySystem` 按 `[stack][channel]` 创建控制器。以 HBM4 六 stack、每 stack 32
channel 为例，共有 192 个独立 Controller；每颗 stack 另有独立 `MemoryImage`、
bank/refresh/RFM、功耗和热状态。stack 之间不直接共享 DRAM 状态。

```text
全局请求
  -> StackAddressMapper（interleaved 或 blocked）
  -> 目标 stack ingress（容量、反压、FCFS/strict-priority QoS）
  -> stack 内 channel mapper
  -> channel-local Controller
  -> 该 stack 独立 MemoryImage/backend
```

这里的 system 层就是用户所说的“控制器上层调动部分”：它负责全局地址拆分、
入口队列、QoS、反压、channel 分发、并行 tick 和统计聚合；单个 Controller 只负责
一颗 stack 的一个 channel，不应包含跨 stack 仲裁。当前未实现 UCIe flit/credit、
链路重传和跨 chiplet 一致性协议，因此不要把 stack ingress 当成完整 UCIe 控制器。

`stack_mapping=interleaved` 按 `stack_interleave_bytes` 条带轮转，适合聚合带宽；
`blocked` 把每颗 stack 暴露成连续容量区间，适合 NUMA 风格实验。显式 trace
`stack=N` 时地址按 stack-local 解读；否则地址是全局系统地址。

## 11. 多 Stack 文件与输出

文件后端必须一 stack 一组文件。路径含 `{stack}` 时直接替换；否则自动把
`.stackN` 插入扩展名前，例如 `outputs/hbm.bin` 变为 `outputs/hbm.stack0.bin`。
command/DFI CSV 同时保存 `stack_id`、本地 `address` 和 `system_address`。

`stack_N_reads/writes/bw_GBps/avg_read_latency/power_pJ/peak_temp_C` 是各 stack
结果；`controller_count`、`active_stacks`、`peak_bandwidth_GBps` 和容量是系统聚合。
同一 stack 内 strict-priority 队首目标 channel 反压时，本 stack 暂停分发；其他
stack 仍可继续。这是明确的 head-of-line 语义，不是 stack 之间存在状态耦合。

统一示例：

```bash
./build-clang-debug/hbm_sim --config configs/run/hbm4_6stack.cfg \
  --trace examples/multistack_qos.trace --requests 0

./build-clang-debug/hbm_sim --config configs/run/hbm4_storage.cfg \
  --memory-backend mmap_sparse --memory-capacity-bytes 1073741824 \
  --memory-data-file outputs/hbm4.bin
```
