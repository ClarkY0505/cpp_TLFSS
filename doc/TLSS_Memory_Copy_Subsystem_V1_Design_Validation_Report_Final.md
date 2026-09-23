# TLSS Memory Copy Subsystem V1

## Design & Validation Report

### TLSS 内存复制子系统 V1 设计与验证报告

项目：cpp_TLFSS / TLSS Memory  
版本：V1.0 Baseline  
目标平台：Linux x86-64 / Intel Core i7-14700K  
验证日期：2026-09-24

本文档基于当前 TLSS memcpy V1 实现、基准测试与压力验证结果整理。

# 摘要

TLSS Memory Copy Subsystem V1是 cpp_TLFSS 中面向高性能存储与数据搬运场景设计的一套内部内存复制基础设施。它并不试图替代 libc memcpy，而是在已知工作负载、CPU 能力和运行时资源条件下，为 TLSS 提供更可控的 Copy Policy、Copy Executor和 MemoryRuntime。

V1 同时实现 AVX2 Cached Copy、REP MOVSB、Direct Non-Temporal Copy和 Parallel Non-Temporal Copy。在此基础上，系统进一步加入 CPU Capability Detection、CPU Topology Awareness、P/E-core Awareness、Caller Physical-Core Exclusion、Persistent Worker Pool、Selective Wake-up和 Precomputed Runtime Worker Plan。

在 Intel Core i7-14700K 平台的最终性能基线中，单 CPU 流式测试下 glibc 在大尺寸区间稳定约 13.7~14.1 GiB/s，Direct NT 稳定约 22.5~23.5 GiB/s；完整 CPU 亲和性下，Topology-aware Parallel Runtime 在 256 KiB 粒度达到 35.65 GiB/s，相对 glibc 的 13.98 GiB/s 为 2.55×。系统同时通过 ASan、UBSan 与 TSan 的完整回归。

> **V1 核心原则**  
> 机制尽可能通用，策略通过 benchmark 获得；CopyPolicy 决定“理想情况下怎么复制”，CpuCapabilities 决定“当前机器允许怎么复制”，MemoryRuntime 决定“当前运行时实际能提供多少资源”。

# 目录

1. 项目目标与范围

2. Public API 与语义

3. 总体架构与职责边界

4. 算法逻辑与设计原理（核心章节）

5. CPU 能力、拓扑与运行时自适应

6. 并发模型、失败处理与生命周期

7. Validation Methodology

8. Performance Baseline

9. 关键问题、故障定位与设计演化

10. V1 限制、V2 演进方向与结论

附录 A. 关键阈值与测试矩阵

# 1. 项目目标与范围

## 1.1 设计目标

TLSS memcpy V1 的直接目标不是“写一个比所有 libc memcpy 都快的函数”，而是建立一套可解释、可验证、可扩展的内部复制系统。对存储系统而言，数据复制往往出现在网络收包、RPC 序列化、缓存搬运、文件块拼接、日志缓冲以及后续可能的 FUSE / Raft 数据通路中，因此复制成本不仅取决于单条指令吞吐，还受到 cache hierarchy、memory bandwidth、working set、线程调度、CPU 拓扑和并发竞争影响。

- 正确性优先：语义保持 memcpy 约定，size=0 正确返回，非重叠内存复制结果与 std::memcpy 一致。

- 性能可解释：每一种 backend 都有清晰的适用区间和性能来源。

- 运行时自适应：不硬编码 CPU ID；根据可用亲和性、物理核心和 P/E-core 类型选择 worker。

- 生产可用：并发首次初始化、构造失败、受限 CPU 环境、线程池生命周期和静态析构顺序都有明确处理。

- 可演进：V1 可以作为稳定 baseline，未来扩展 AVX-512、NUMA Awareness或 Adaptive Policy。

## 1.2 非目标

V1 不试图完全复刻 glibc 中针对所有微架构和所有调用模式的高度复杂 dispatch 体系；也不支持 memmove 语义，因此源区间与目标区间发生 overlap 时行为不作为本模块保证范围。V1 的重点是 TLSS 内部典型工作负载下的性能、可控性和可维护性。

## 1.3 目标平台与验证环境

| **项目**                | **说明**                                                           |
|-------------------------|--------------------------------------------------------------------|
| CPU                     | Intel Core i7-14700K，8 个 P-core（16 logical CPUs）+ 12 个 E-core |
| OS                      | Linux x86-64                                                       |
| 编译/构建               | C++17 / CMake / NASM / GCC 或 Clang                                |
| 主要工具                | perf、taskset、GDB、ASan、UBSan、TSan                              |
| 主要性能单位            | GiB/s、ns/call                                                     |
| 最终单 CPU 流式工作集   | 192 MiB                                                            |
| 最终 Runtime 性能工作量 | Direct NT 与 Runtime 各复制 2 GiB                                  |

# 2. Public API 与语义

**V1 Public API**

```cpp
namespace TLSS::MEMORY {

enum class CopyBackend {
Auto,
Avx2,
RepMovsb,
NonTemporal,
};

enum class CopyHint {
Default,
Streaming,
};

void* memcpy(void* dst, const void* src, std::size_t size);

void* memcpy(
void* dst,
const void* src,
std::size_t size,
CopyBackend backend,
CopyHint hint);

} // namespace TLSS::MEMORY
```

三参数接口是低开销默认入口，用于普通复制场景；五参数接口允许上层显式表达 backend与 CopyHint。CopyHint::Default优化普通单次调用与缓存友好路径；CopyHint::Streaming明确告诉策略层数据更接近大工作集、低复用或顺序搬运，因此允许使用 Non-Temporal Store和 Parallel NT。

| **Backend** | **AVX2 可用** | **AVX2 不可用**       | **语义**              |
|-------------|---------------|-----------------------|-----------------------|
| Auto        | 按策略选择    | 安全退化到 libc       | 允许 fallback         |
| Avx2        | AVX2 Cached   | 抛出 unsupported 错误 | 显式后端不静默切换    |
| RepMovsb    | REP MOVSB     | REP MOVSB             | 与 AVX2 可用性无关    |
| NonTemporal | Direct NT     | 抛出 unsupported 错误 | 当前实现依赖 AVX2 YMM |

> **接口契约**  
> Auto允许为了正确性和兼容性退化；Explicit Backend代表调用者明确要求某种实现，因此“要么执行它，要么明确报不支持”，不会偷偷切换成另一种实现。

# 3. 总体架构与职责边界

**V1 逻辑架构**

```text
TLSS::MEMORY::memcpy()
│
├── CopyBackend / CopyHint
│
▼
CopyPolicy
│
▼
CpuCapabilities
│
▼
Capability Adaptation
│
▼
CopyPlan
│
▼
CopyExecutor
│
├── libc memcpy
├── REP MOVSB
├── AVX2 Cached
├── Direct NT
└── Parallel NT
│
▼
MemoryRuntime
│
▼
CpuTopology
│
▼
Precomputed Worker Plan
│
▼
ParallelCopyPool
```

该架构最重要的价值不是“多了一些类”，而是把三个经常混在一起的问题彻底拆开：CopyPolicy回答“理想情况下什么算法更合适”；CpuCapabilities回答“当前 CPU 和 OS 是否允许执行这些指令”；MemoryRuntime回答“当前进程亲和性、调用线程位置和物理核心资源允许使用多少并行度”。

| **层次**   | **核心问题**           | **典型输入**                     | **输出**          |
|------------|------------------------|----------------------------------|-------------------|
| CopyPolicy | 理想情况下怎么复制？   | size / hint                      | CopyPlan          |
| Capability | 当前硬件与 OS 允许吗？ | CPUID / XCR0                     | Safe CopyPlan     |
| Runtime    | 现在能拿到多少资源？   | affinity / topology / caller CPU | active worker set |
| Executor   | 如何执行既定方案？     | Safe CopyPlan                    | 实际复制          |

# 4. 算法逻辑与设计原理（核心章节）

本章是 V1 的核心。它不只描述“代码做了什么”，还解释为什么要这样设计、这些设计对应 CPU 和内存系统中的什么原理，以及各阶段实验如何推动架构演化。

## 4.1 从 memcpy 的本质开始：复制为什么会成为性能问题

memcpy 的本质是把 N 字节从 src搬到 dst。从算法复杂度看它只是 O(N)，但现代 CPU 上真正决定性能的不是“大 O”，而是每个 cache line经过哪些硬件路径：源数据是否命中 L1/L2/L3 cache、目标写入是否触发 write allocate、数据最终是否必须流经 DRAM、以及 CPU 是否有足够 outstanding memory operations隐藏访问延迟。

当数据很小时，复制成本通常由函数调用、分支、指令数量和 cache latency主导；当数据很大并且 working set远大于 LLC时，瓶颈逐渐变成 memory bandwidth。因此“一个算法覆盖所有 size”通常不是最优策略，这也是 V1 最终采用 size-aware policy的根本原因。

> **关键区分**  
> Hot-cache benchmark 主要测指令与 cache 路径；Streaming benchmark 主要测真实 DRAM 搬运能力。两者不能混在一起用同一组数字决定所有阈值。

## 4.2 Cached Copy 与 AVX2 的基本思想

AVX2提供 256-bit YMM register。一次 256-bit load/store 可处理 32 字节数据。理论上，与逐字节或较窄标量复制相比，向量化能够减少 loop overhead 和指令条数，并提高每个循环迭代搬运的数据量。

V1 的 AVX2 cached kernel采用分段处理：小尺寸通过不同分支避免进入通用大循环；中大尺寸使用 128-byte loop同时加载/存储多个 YMM；余量再由 32-byte loop 和 small tail处理。设计目的不是简单“越宽越快”，而是让不同尺寸避免为不需要的循环、计数器和分支付费。

**AVX2 Cached Kernel的大致尺寸分层**

```text
size < 16 -> small15
16 <= size < 32 -> size16_31
32 <= size < 64 -> size32_63
64 <= size <128 -> size64_127
size >= 128 -> loop128
+ loop32
+ small tail
```

128-byte loop 的典型思想是一次使用 ymm0~ymm3 处理四个 32-byte vector，从而每次循环复制 128 字节。这里主要提升的是 instruction-level parallelism和降低 branch / loop bookkeeping占比。对于 cached store，store 会进入正常 cache hierarchy，因此适合数据可能被很快再次访问的场景。

但 AVX2 并不意味着在所有小尺寸都必然赢。最终基准显示 2~3 KiB 区间 glibc 路径具有竞争力，而 3~8 KiB AVX2 再次较好。这说明真实表现不仅取决于向量宽度，还取决于 glibc 内部实现、分支布局、微架构优化和调用上下文。V1 因此没有把“AVX2 更现代”当作策略依据，而是坚持用 benchmark-derived thresholds。

## 4.3 REP MOVSB：为什么一条“老指令”仍然重要

REP MOVSB 是 x86 字符串复制指令序列。现代 Intel CPU 上，ERMS使硬件能够用内部优化路径执行连续复制。它的优势在于软件侧代码极短，dispatch overhead低，硬件可以根据内部实现高效处理一段连续内存。

需要特别区分“指令是否合法”和“是否有 ERMS 性能优化”。在 x86-64 上 REP MOVSB 指令本身可执行；ERMS 主要影响性能特性。因此 V1 的 capability model不会因为 erms=false 就认为 RepMovsb backend不可用。Default 策略在 \>2 KiB 后使用 REP MOVSB，是结合单次调用、缓存路径与整体复杂度后的选择。

> **设计原则**  
> Explicit RepMovsb与 AVX2 能力无关；ERMS 是性能特性，不是合法性开关。

## 4.4 Non-Temporal Store：为什么流式写入可以更快

普通 cached store通常会让写入进入 cache hierarchy。当目标 cache line 不在 cache 中时，处理器可能需要执行 Read For Ownership / RFO，也就是先把目标 cache line 拉进缓存，再修改并最终写回。对于“写完后短时间内不会再次读取”的流式数据，这会造成额外的 cache pollution和带宽消耗。

Non-Temporal Store提供另一种写入意图：告诉硬件这些数据不需要长期留在普通 cache 中。实现细节由微架构决定，但在大规模 streaming workload中，它通常可以减少 cache pollution，并避免普通 write-allocate路径带来的部分额外流量。

V1 的 Direct NT使用 AVX2 YMM 和 non-temporal store。原始 NT2 kernel具有严格前提：destination 32-byte aligned、主体 size 以 8192-byte block 组织，并使用 two-stream layout制造更好的 memory-level parallelism。内核末尾使用 sfence保证 non-temporal stores的可见性顺序，并使用 vzeroupper 降低 AVX/YMM 与较旧 SSE 路径混用时可能出现的状态转换代价。

为什么 Direct NT 不适合所有尺寸？因为 NT 路径本身也有固定成本：对齐处理、sfence、可能更高的 store setup开销。小数据时这些成本无法被足够的数据量摊薄；而大流式工作集时，避免 cache pollution 和减少不必要的 cache traffic才逐渐成为主要收益。最终 Streaming 策略把 Direct NT 起点放在约 8 KiB。

## 4.5 Arbitrary Alignment与 Prefix / Body / Tail

Raw NT kernel通常希望 dst 满足固定对齐，而公共 memcpy API 不能要求调用者总是提供 32-byte aligned address。因此 V1 没有把“调用者必须对齐”暴露为公共约束，而是在 wrapper 中把任意请求拆成 prefix、body和 tail。

**任意对齐请求的分解**

```text
dst/src/size 任意
│
├── prefix：复制到满足 NT 对齐要求的位置
│ └── cached AVX2
│
├── body：按 8192-byte block 组织
│ └── Direct / Parallel NT
│
└── tail：剩余不足 block 的部分
└── cached AVX2
```

这种分解的关键是“只有满足内核前提的主体才进入 NT kernel”。这把 unsafe precondition限制在 internal layer，公共接口仍然保持普通 memcpy 的易用性。对于 Parallel NT，make_copy_partition() 会根据 block count判断 body 是否足够大；如果不足以支撑有效并行，整个请求可以退回 cached 或 Direct NT。

## 4.6 Two-stream NT与 Memory-Level Parallelism

现代 CPU 的单次 DRAM 访问延迟很高，但处理器可以同时维护多个 outstanding cache miss / memory request。这类并发被称为 Memory-Level Parallelism / MLP。如果程序的 load/store 序列过于串行，即使带宽理论上很高，也可能因为等待单条访问完成而无法填满内存控制器。

V1 的 NT2 kernel通过在一个大 block 内交错处理两个相距约 4 KiB 的 stream，让硬件更容易同时看到多个相互独立的内存访问，从而增加在途请求数量。这里的“2-stream”不是多线程，而是单线程内部对两个独立地址流交错发出 load/store。它和后面的 Parallel NT 属于两个不同层级：前者提高一个核心内部的 MLP，后者增加多个物理核心共同产生的 memory requests。

## 4.7 Parallel NT：为什么多核能提高吞吐，也为什么不会无限提高

当 Direct NT 单核已经把软件侧指令效率优化得较高以后，下一层瓶颈通常变成单个核心能制造的 memory requests 数量和平台总内存带宽。Parallel NT把大请求拆给多个物理 P-core并行执行，让多个核心同时驱动内存子系统。

V1 的固定工作量实验显示吞吐随物理 P-core 数量先上升后趋于饱和：1T 约 24.64 GiB/s，2T 约 33.87 GiB/s，4T 约 37.38 GiB/s，8T 约 41 GiB/s。继续加入 E-core 或 SMT sibling几乎不再提高总带宽。这说明平台在约 8 个物理 P-core 的并发压力下已经接近 practical memory bandwidth saturation。

| **配置**           | **吞吐（GiB/s）** | **结论**       |
|--------------------|-------------------|----------------|
| 1 physical P-core  | 24.64             | 单核           |
| 2 physical P-cores | 33.87             | 明显扩展       |
| 4 physical P-cores | 37.38             | 继续提升       |
| 8 physical P-cores | 约 41.1~41.7      | 接近带宽饱和   |
| 8P + E-core        | 约 41.5~41.7      | 几乎无额外收益 |
| 8P + SMT           | 约 41.6           | SMT 无明显收益 |

这类结果说明并行 memcpy 不是“线程越多越快”。一旦 DRAM / memory controller达到带宽上限，更多线程只会增加调度、同步和尾部不均衡成本。V1 因此把物理 P-core 作为优先 worker 资源，并明确拒绝为了“凑线程数”而自动加入 E-core。

## 4.8 Persistent Worker Pool：为什么不能每次 memcpy 都创建线程

多线程并行的第一版直觉通常是“遇到大请求就 std::thread 创建 N 个线程，复制完 join”。但 thread creation本身是微秒级成本，而 64~256 KiB 的 memcpy 往往只有微秒甚至更低的执行时间。如果每次复制都创建线程，调度开销会吞掉并行收益。

| **线程数** | **一次创建开销（约）** |
|------------|------------------------|
| 1          | 14.95 µs               |
| 2          | 19.61 µs               |
| 4          | 31.86 µs               |
| 8          | 76.06 µs               |

因此 V1 使用 Persistent Worker Pool：线程在 Runtime 初始化时创建一次，并绑定到选定 CPU；后续每次复制只发布 task、唤醒需要的 worker、等待 completion，从而把微秒级的创建成本变成纳秒到亚微秒级的 synchronization cost。

空任务同步测试曾得到 1W 约 143 ns、2W 约 299 ns、4W 约 497 ns、8W 约 854 ns。这些数字解释了为什么并行阈值必须存在：当数据太小时，几百纳秒的 dispatch/wait开销本身就足以抵消并行收益。

## 4.9 Hybrid Spin/Sleep

纯 condition_variable sleep节省 CPU，但 wake-up latency较高；纯 busy-spin唤醒快，却会长期占用 CPU。V1 采用 hybrid spin/sleep：worker 在任务 generation未变化时先执行有限次数 \_mm_pause() 自旋，如果仍然没有任务再进入 condition_variable 等待。

\_mm_pause() 不是“停止线程”，它仍然在 CPU 上执行，只是向处理器提示当前处于 spin-wait，可减少部分 pipeline / SMT contention。V1 的目的不是让自旋无限持续，而是在“任务很快到来”的情况下避免完整的 sleep/wakeup，在空闲较久时又让 worker 进入睡眠。

## 4.10 Generation Publication与 Completion Handshake

线程池需要回答两个同步问题：第一，worker 怎么知道“这是一个新任务”；第二，caller 怎么知道“这一轮所有 active workers 都完成了”。V1 使用 generation作为任务版本号，每个 worker 保存 observed_generation。当自己的 generation 发生变化时，worker 才读取对应 task 并执行。

完成侧使用 completed_count_和 expected_completion_count\_。注意 expected completion 不能简单等于 pool size，因为 V1 支持 sparse active workers：一轮任务可能只启用 4 个、7 个或其它子集。只有被激活的 worker 才应该参与 partition和 completion计数。

<table>
<colgroup>
<col style="width: 100%" />
</colgroup>
<thead>
<tr class="header">
<th><strong>同步不变量<br />
</strong>一轮任务的完成条件不是“所有常驻 worker 都响应”，而是 completed_count == expected_completion_count，其中 expected_completion_count 等于该轮 active worker 数。</th>
</tr>
</thead>
<tbody>
</tbody>
</table>

## 4.11 Sparse Active Workers与 Active Rank

Master pool固定绑定在一组 P-core CPU 上，但 caller可能恰好运行在其中一个物理核心。如果仍然让该核心上的 worker 工作，caller 和 worker 会竞争同一 physical core的执行资源，导致性能断崖。因此 V1 允许每轮只启用 master pool 的某些 slot。

这里一个容易出错的地方是 partition不能使用“物理 slot index”直接计算数据偏移，而应使用 active_rank。例如 active slots 为 {0,1,2,3,5,6,7}，worker slot 5 实际上是第 5 个活动 worker（rank=4），而不是第 6 个分块。V1 因此按 active_worker_count 和 active_rank 分配 block，避免数据块空洞或重叠。

## 4.12 Selective Wake-up：为什么“没干活的线程”也会拖慢 caller

最初 sparse workers虽然只分配任务给 7 个 worker，但所有 worker 仍然共享一个 global generation和 notify_all。这意味着被排除、恰好绑定在 caller CPU8 上的 worker 仍会醒来、自旋并检查任务，即使最终发现 task.size==0。实验中这种“无效唤醒”仍然足以与 caller 争夺 CPU8，导致 128 KiB 等尺寸显著下降。

V1 最终为每个 worker 引入独立 WorkerState：每个 state 拥有自己的 generation、mutex和 condition_variable。Dispatch只更新 active workers 的 generation，并只对这些 worker 调用 notify_one。被排除的 worker generation 不变，因此可以继续睡眠，不再污染 caller 所在核心。

**Selective Wake-up核心结构示意**

```cpp
WorkerState {
atomic<uint64_t> generation;
mutex mutex;
condition_variable condition;
};

dispatch(active_slots):
for slot in active_slots:
tasks[slot] = ...
worker_state[slot].generation = new_generation
worker_state[slot].condition.notify_one()
```

这一步把 caller CPU8 上的 128 KiB 性能从早期约 5~6 GiB/s 的严重断崖恢复到 30+ GiB/s 级别，是整个 V1 中最能说明“系统性能问题不一定来自 memcpy 指令本身”的案例。

## 4.13 Caller Physical-Core Exclusion

在 SMT架构中，一个 physical core可能对应多个 logical CPUs。因此仅仅排除 caller 的 logical CPU ID并不充分：如果 caller 在 CPU8，而同一物理 P-core 的 sibling仍被 worker 使用，两个线程依然共享执行端口、cache、前端等核心资源。

V1 通过 Linux sysfs 获取 physical_package_id 和 core_id，把 logical CPUs 分组为 PhysicalCore，并在 worker candidate selection阶段排除 caller 所属的整个 (package_id, core_id)。这一规则同时适用于 caller 在任意 SMT sibling 上的情况。

## 4.14 P-core / E-core Awareness与 Tail Effect

i7-14700K 是 Hybrid CPU，包含 IntelCore 类型的 P-core 和 IntelAtom 类型的 E-core。V1 使用 CPUID leaf 0x1A 读取 core type，并把 P-core 作为并行 memcpy 的优先资源。

实验对比 4P、7P 和 7P+1E 时，7P 在 128 KiB~1 MiB 全部优于 7P+1E。原因不是 E-core “不能复制”，而是 heterogeneous worker speed会产生 tail effect：任务按块分给多个 worker 后，整体完成时间由最慢 worker 决定。加入一个更慢的 E-core 可能让快 P-core 都完成后仍等待 E-core 的最后分块，从而降低总体吞吐。

| **Size** | **4P** | **7P** | **7P+1E** |
|----------|--------|--------|-----------|
| 128 KiB  | 33.56  | 35.15  | 30.53     |
| 256 KiB  | 35.81  | 39.31  | 32.02     |
| 512 KiB  | 27.75  | 35.41  | 29.53     |
| 1 MiB    | 33.51  | 37.43  | 30.38     |

因此 V1 的原则是：宁愿使用较少的同类高性能 worker，也不为了达到 desired_worker_count强行用 E-core 补齐。

## 4.15 Precomputed Runtime Worker Plan

Topology-aware selection最初每次 memcpy 都执行 sched_getcpu()、遍历 topology、构造 WorkerCandidates、WorkerSelection 和 ActiveWorkerSelection。完整选择路径实测约 153 ns/call。对于 128 KiB 等仅数微秒级的复制，这已经是可见成本。

V1 最终把昂贵的决策搬到 MemoryRuntime initialization path：针对每一个可用 caller CPU，提前计算 desired=4 和 desired=8 时的 active worker slots。运行时只需要 sched_getcpu() + 数组 lookup。纯 plan lookup 从约 153 ns/call 降到约 1.82 ns/call；包含 sched_getcpu() 的 fast path约 6 ns/call。

| **阶段**                        | **开销**           |
|---------------------------------|--------------------|
| 动态拓扑选择                    | 约 153 ns/call     |
| 预计算 plan lookup              | 约 1.82 ns/call    |
| sched_getcpu + lookup fast path | 约 5.8~6.4 ns/call |

<table>
<colgroup>
<col style="width: 100%" />
</colgroup>
<thead>
<tr class="header">
<th><strong>系统优化原则<br />
把 expensive decision making从 hot path搬到 initialization path。这是 V1 从“能工作”走向“运行时可接受”的关键步骤。</strong></th>
</tr>
</thead>
<tbody>
</tbody>
</table>

## 4.16 Minimum Useful Parallelism

当 CPU affinity受限时，排除 caller physical core 后可能只剩 0、1、2、3 个 P-core worker。V1 没有假设“只要有 worker 就并行”，而是直接比较 Runtime 与 Direct NT。结果显示：1 个 worker 时 Runtime 反而更慢，因为 dispatch/wakeup/wait 的固定开销无法被并行收益抵消；2 个 worker 开始在 128 KiB~1 MiB 范围稳定超过 Direct NT。

| **实际 workers** | **256 KiB Runtime** | **256 KiB Direct NT** | **策略**    |
|------------------|---------------------|-----------------------|-------------|
| 0                | 约等于 Direct NT    | 约 22.3               | Direct NT   |
| 1                | 19.15               | 22.59                 | Direct NT   |
| 2                | 25.21               | 22.54                 | Parallel NT |
| 3                | 33.05               | 22.23                 | Parallel NT |

最终 V1 固化 k_min_parallel_worker_count = 2：actual workers \< 2 时直接退化到 Direct NT；actual workers \>= 2 才进入 Parallel NT。这个阈值是当前平台的 benchmark-derived policy，并非所有 CPU 的普适常数。

## 4.17 Default vs Streaming：为什么需要两个 Hint

一个重要设计是把 CopyHint::Default 与 CopyHint::Streaming 分开。Default 侧重普通单次调用和缓存友好语义，不希望为了理论带宽破坏 cache locality；Streaming 明确表示大工作集、低复用或顺序搬运，可以更积极地使用 Direct NT 和 Parallel NT。

| **Hint**  | **Size**   | **V1 策略**             |
|-----------|------------|-------------------------|
| Default   | 0~2 KiB    | AVX2 Cached             |
| Default   | \>2 KiB    | REP MOVSB               |
| Streaming | 0~2 KiB    | AVX2 Cached             |
| Streaming | 2~3 KiB    | libc memcpy             |
| Streaming | 3~8 KiB    | AVX2 Cached             |
| Streaming | 8~64 KiB   | Direct NT               |
| Streaming | 64~128 KiB | Parallel NT / desired 4 |
| Streaming | \>=128 KiB | Parallel NT / desired 8 |

最终性能表中 Auto Streaming 在 8 KiB 以上基本贴合 Direct NT，而在完整 CPU 亲和性下又能根据 Runtime 获得并行收益；这说明 Policy / Executor / Runtime 的分层没有引入明显额外成本。

# 5. CPU 能力、拓扑与运行时自适应

## 5.1 CpuCapabilities

V1 区分 hardware support和 usable state。AVX2 是否可执行不仅看 CPUID leaf 7 的 AVX2 bit，还要确认 OSXSAVE，并通过 XGETBV 检查 XCR0 中 XMM/YMM state是否被操作系统启用。只有 avx2_usable=true 才允许执行 AVX2 kernel。

| **字段**      | **含义**                    |
|---------------|-----------------------------|
| avx_hardware  | CPU 宣告支持 AVX            |
| avx2_hardware | CPU 宣告支持 AVX2           |
| osxsave       | OS 支持扩展状态保存         |
| avx_usable    | 硬件 + OS 状态均满足 AVX    |
| avx2_usable   | AVX 可用且硬件支持 AVX2     |
| erms          | Enhanced REP MOVSB 性能能力 |

## 5.2 CpuTopology

V1 首先使用 sched_getaffinity() 获取当前进程真正可使用的 CPU 集合，再只对这些 CPU 读取 Linux sysfs topology。这样 taskset、容器 cpuset 或调度限制都会自然反映到 Runtime，而不是假设系统所有 CPU 都可用。

logical CPU 通过 (physical_package_id, core_id) 分组为 PhysicalCore。每个 PhysicalCore 只选择一个 logical CPU 作为 worker candidate，避免 SMT sibling 同时作为独立 worker。对于 Hybrid CPU，再结合 CPUID 0x1A 把 core_type 标记为 IntelCore、IntelAtom 或 Unknown。

## 5.3 Master Worker Set

MemoryRuntime 初始化时，从 topology 中选择每个 IntelCore / P-core 的一个 logical CPU 组成 master_worker_cpu_ids。当前 14700K 全亲和性环境下得到 {0,2,4,6,8,10,12,14}。在 taskset 限制下，该集合会自动缩小；仅 E-core 环境下集合可以为空，此时 Parallel NT 不可用并安全退化到 Direct NT。

这种设计把“机器型号”和“可用资源”分开：即使同一台机器被 taskset 限制成 4 个 P-core、2 个 P-core 或 E-core only，Runtime 都不会依赖硬编码 CPU ID。

## 5.4 Capability Adaptation

Auto 路径先根据 size/hint 产生理想 CopyPlan，再通过 adapt_auto_plan_for_capabilities() 把不安全计划转换为安全计划。例如没有 usable AVX2 时，Avx2Cached、DirectNt 和 ParallelNt 都转为 LibcMemcpy。Explicit Avx2 / NonTemporal 则不会静默 fallback，而是明确报告不支持。

# 6. 并发模型、失败处理与生命周期

## 6.1 Single-flight与 Busy Fallback

V1 的 master ParallelCopyPool 是共享资源，一次只允许一个 Parallel NT request占用。MemoryRuntime 使用 atomic_flag作为 master_pool_busy\_。如果其它 caller 同时请求 Parallel NT，抢不到 pool 的调用不会等待队列，而是直接执行 Direct NT。

这种 single-flight 设计牺牲了“多个并行复制请求同时共享 pool”的潜在吞吐优化，但极大简化了 V1 的资源调度和 correctness model。对于 V1，明确 fallback 比引入复杂多租户 scheduler更合适。

## 6.2 Lazy Initialization

MemoryRuntime 不是程序启动即创建。只有 CopyPlan 真正选择 ParallelNt 时才通过 get_memory_runtime() 获取 Runtime，因此 LibcMemcpy、REP MOVSB、AVX2 Cached 和 Direct NT 不会为了“可能用到并行”而创建常驻线程。测试确认 RepMovsb 路径 runtime constructions before=0 / after=0，而第一次 ParallelNt 为 0→1，第二次仍为 1。

## 6.3 Concurrent First Use

get_memory_runtime() 使用 function-local static机制，因此 C++11 以后首次初始化具有线程安全保证。8 个 caller 同时第一次进入 Parallel NT 的压力测试中，Runtime 只成功构造一次，其它并发请求依据 busy fallback 规则执行 Direct NT。1000 个独立进程重复该场景全部通过。

## 6.4 Construction Failure Containment

Parallel runtime 是性能优化资源，不应成为 memcpy correctness dependency。V1 因此把 Runtime acquisition异常限制在很窄的 try_get_memory_runtime() 边界：Runtime 构造失败时返回 nullptr，当前调用退化到 Direct NT；Runtime 已经构造成功后 execute_parallel_copy() 内部真正的逻辑异常不会被无差别吞掉。

故障注入测试显示首次构造失败时 threw=0，复制仍正确；关闭故障后下一次调用重新尝试并成功。并发构造失败测试中 8 个 caller 导致 8 次失败尝试、0 次成功，所有 copy 均 fallback 正确；随后第 9 次尝试成功。1000 个独立进程压力测试全部通过。

## 6.5 Runtime Lifetime与 Static Destruction Order

局部 MemoryRuntime 对象本身支持正常 RAII：1000 次完整 construct → parallel copy → stop workers → join → destruct 生命周期压力测试通过。真正隐蔽的问题来自全局共享 Runtime 的 static destruction order。测试证明如果使用 static MemoryRuntime runtime;，进程退出时 Runtime 可能先于其它全局对象析构，而其它对象的 destructor 仍可能调用 TLSS memcpy。

V1 最终把全局共享 Runtime 设计成 Immortal Runtime：function-local static 保存一个 new MemoryRuntime() 指针，对象有意不参与 C++ static destruction。这样底层 memcpy 服务在其它 global/static destructor执行期间仍可使用。该分配是固定数量的 process-lifetime allocation，不是随运行时间增长的 unbounded leak。

<table>
<colgroup>
<col style="width: 100%" />
</colgroup>
<thead>
<tr class="header">
<th><strong>边界说明<br />
Immortal Runtime 适用于当前 TLSS 可执行程序内部基础设施。如果未来该模块进入支持 dlclose 的 shared library / plugin，则需要重新设计 explicit init/shutdown生命周期。</strong></th>
</tr>
</thead>
<tbody>
</tbody>
</table>

# 7. Validation Methodology

## 7.1 Correctness

正确性测试覆盖 size=0、策略边界、多个 backend、任意对齐、随机 offset、prefix/body/tail、不同 active worker sets以及受限 CPU affinity。核心 stress 测试包括 Parallel NT 10000 次和 Sparse Worker 10000 次。

| **测试**                         | **结果**     |
|----------------------------------|--------------|
| copy-policy-test                 | PASS         |
| copy-executor-test               | PASS         |
| copy-capability-policy-test      | PASS         |
| auto-capability-integration-test | PASS         |
| explicit-backend-capability-test | PASS         |
| backend-plan-contract-test       | PASS         |
| tlss-memcpy-auto-test            | PASS         |
| tlss-memcpy-backend-test         | PASS         |
| parallel-nt-stress               | PASS × 10000 |
| sparse-worker-stress             | PASS × 10000 |

## 7.2 Runtime / Concurrency

| **测试**                             | **验证内容**                       | **结果**             |
|--------------------------------------|------------------------------------|----------------------|
| memory-runtime-concurrency-test      | single-flight 与 busy fallback     | PASS                 |
| memory-runtime-dynamic-worker-test   | caller-aware active worker 选择    | PASS                 |
| runtime-degradation-test             | 1/2/3/4 P-core 与 E-core only 退化 | PASS                 |
| concurrent-first-use-test            | 并发首次初始化 exactly-once        | PASS / 1000 进程     |
| runtime-construction-failure-test    | 失败隔离与恢复                     | PASS                 |
| concurrent-construction-failure-test | 并发失败 fallback 与恢复           | PASS / 1000 进程     |
| runtime-lifetime-test                | 重复构造/使用/析构                 | PASS × 1000 生命周期 |

## 7.3 Sanitizer

V1 最终在独立构建中运行 ASan、UBSan和 TSan。ASan/UBSan 完整回归覆盖 policy、executor、public API、Runtime、failure injection、stress 和 lifetime；TSan 覆盖高风险共享状态与同步协议。最终未发现 sanitizer error或 data race report。

| **检查器** | **重点**                                                         | **结果** |
|------------|------------------------------------------------------------------|----------|
| ASan       | 越界、use-after-free、非法内存访问                               | PASS     |
| UBSan      | 未定义行为、对齐等                                               | PASS     |
| TSan       | generation、completion、busy flag、condition variable 等数据竞争 | PASS     |

## 7.4 glibc 对照测试方法

V1 最终新增独立基准程序 benchmark/memcpy_glibc_benchmark.cpp，并在 benchmark/CMakeLists.txt 中提供可选构建目标。基准测试将 glibc 与 TLSS 各 backend 放在同一测试框架中，统一缓冲区、对齐、工作集、复制尺寸、总复制量、预热方式与采样轮数。每个尺寸和 backend 测试 5 轮，最终报告中使用中位数，所有复制结果均通过 memcmp 校验。

为了避免编译器把 memcpy 当作 builtin 展开，glibc 路径通过独立 noinline wrapper 调用，并在 benchmark 构建中禁止 builtin memcpy 优化。反汇编确认 glibc 包装函数实际跳转到 memcpy@plt，因此对照数据对应当前系统 glibc 在本机 CPU 上实际选择的 memcpy 实现，而不是编译器生成的替代代码。

单 CPU 流式测试把整个进程限制在 CPU0，源和目标各使用 192 MiB 流式工作集，用于比较相同 CPU 资源条件下的 backend 吞吐。完整 Runtime 测试则先在完整 CPU 亲和性下初始化 Runtime，再把 caller 固定到 CPU8，使 TLSS 可以根据拓扑使用后台 P-core。该测试反映应用级系统吞吐，因此其 Auto/glibc 倍率不能解释为同等 CPU 资源条件下的单核算法加速比。

完整六 backend 数据、逐轮采样值与原始记录保存在 benchmark/results/glibc-compare-2026-09-24/summary.md。本报告正文仅保留最能说明 V1 行为的 glibc、Direct NT、Auto Default 与 Auto Streaming 数据，以及完整 Runtime 的 glibc 对照。

# 8. Performance Baseline

## 8.1 单 CPU 流式 glibc 对照

整个进程限制在 CPU0，源和目标缓冲区各 192 MiB。每个尺寸、每个 backend 测试 5 轮并取中位数；复制结果均使用 memcmp 校验。由于进程只能使用 CPU0，Auto Streaming 在策略要求并行时无法获得后台 P-core，因此会自然退化到非并行路径。本表主要用于相同 CPU 资源条件下比较 glibc、Direct NT 与两种 Auto 策略，单位为 GiB/s。

| **尺寸** | **glibc** | **Direct NT** | **Auto Default** | **Auto Streaming** |
|----------|-----------|---------------|------------------|--------------------|
| 1 KiB    | 14.06     | 13.25         | 12.32            | 12.36              |
| 2 KiB    | 12.69     | 12.05         | 12.62            | 12.60              |
| 3 KiB    | 12.76     | 13.30         | 12.69            | 13.36              |
| 4 KiB    | 13.75     | 12.95         | 13.29            | 13.19              |
| 8 KiB    | 13.72     | 21.23         | 13.53            | 21.24              |
| 16 KiB   | 13.68     | 21.87         | 13.59            | 21.93              |
| 32 KiB   | 13.61     | 22.24         | 13.67            | 22.12              |
| 64 KiB   | 13.73     | 22.45         | 13.70            | 22.36              |
| 128 KiB  | 13.69     | 22.64         | 13.68            | 22.47              |
| 256 KiB  | 13.72     | 22.67         | 13.73            | 22.55              |
| 512 KiB  | 13.71     | 22.77         | 13.71            | 22.65              |
| 1 MiB    | 13.73     | 22.69         | 13.74            | 22.71              |

从 8 KiB 开始，Auto Streaming 基本贴合 Direct NT：8 KiB 为 21.24 vs 21.23 GiB/s，1 MiB 为 22.71 vs 22.69 GiB/s。说明 Policy → Capability → Executor 的抽象层没有引入明显吞吐损失。Auto Default 则保持在 cached/REP 路径，符合“普通单次调用而非强制流式”的语义。

## 8.2 完整 Runtime 与 glibc 对照

完整 Runtime 对照测试先在完整 CPU 亲和性下初始化 MemoryRuntime，再将 caller 固定到 CPU8。每个尺寸分别测试 glibc、Direct NT 与 Auto Streaming，并以 5 个独立进程结果的中位数作为最终值。Auto Streaming 会根据 caller 所在物理核心排除冲突 P-core，并按 size 使用 4 或 7 个实际后台 P-core workers。

| **尺寸** | **glibc** | **Direct NT** | **Auto Streaming** | **Auto / glibc** |
|----------|-----------|---------------|--------------------|------------------|
| 64 KiB   | 13.82     | 23.06         | 26.96              | 1.95×            |
| 128 KiB  | 13.92     | 22.88         | 31.10              | 2.23×            |
| 256 KiB  | 13.98     | 23.40         | 35.65              | 2.55×            |
| 512 KiB  | 13.99     | 23.52         | 32.71              | 2.34×            |
| 1 MiB    | 14.01     | 23.47         | 34.89              | 2.49×            |

256 KiB 时 glibc 为 13.98 GiB/s，Direct NT 为 23.40 GiB/s，Auto Streaming 为 35.65 GiB/s；Auto Streaming 相对 glibc 为 2.55×，相对 Direct NT 也有显著提升。64 KiB、128 KiB、512 KiB 与 1 MiB 的 Auto/glibc 分别为 1.95×、2.23×、2.34× 与 2.49×。这表明 Persistent Worker Pool、Selective Wake-up、Topology Awareness 与 Caller-core Exclusion 的组合能够把单核内存复制优化进一步扩展为系统级吞吐提升。

## 8.3 性能数据应如何解释

本报告的性能数字只作为当前 V1 在当前 CPU、内存、OS、编译器和工作集上的 baseline，不应被解释为所有平台上的固定规律。特别是 8 KiB、64 KiB、128 KiB、minimum workers=2 等阈值，都来自当前平台的实测。单 CPU 流式结果用于比较相同 CPU 资源下的 backend 行为；完整 Runtime 结果使用多个后台 P-core，体现的是应用级系统吞吐，不能被解释为同等 CPU 资源条件下的单核加速比。

# 9. 关键问题、故障定位与设计演化

## 9.1 CPU8 Performance Cliff

早期 8-worker pool 固定绑定 {0,2,4,6,8,10,12,14}，而 benchmark caller 也固定 CPU8。结果 128 KiB 一度只有约 5~6 GiB/s。最初如果只从 memcpy kernel 指令角度分析，很容易误判为“128 KiB 阈值错误”或“NT kernel 在这个尺寸失效”。真正原因是 caller 和 worker 同时竞争 CPU8 所在物理 P-core。

后续通过 caller 移到 CPU16、排除 caller physical core、再进行 4P/7P/7P+1E 对比，最终证明问题来自 CPU resource collision而不是复制算法。最终 topology-aware plan 在 caller CPU8 下 128 KiB 达到 30.48 GiB/s。

## 9.2 Inactive Worker Wake-up

仅仅把 worker4 标记为 inactive 并不够。如果仍然使用 global generation + notify_all，该 worker 会被唤醒并短暂自旋。由于它恰好绑定 CPU8，仍然会干扰 caller。将 inactive dummy 改绑 CPU16 后性能恢复，从实验上证明“无任务线程的唤醒”就是干扰来源，最终促成 per-worker generation + selective notify_one 设计。

## 9.3 Runtime Selection Overhead

拓扑感知初版虽然正确，但每次都构造 vector、扫描 topology，实测约 153 ns/call，导致 128 KiB Runtime 与手工 sparse 7P 有明显差距。Precomputed plan 把选择开销降低到 1.82 ns lookup、约 6 ns 包含 sched_getcpu 的 fast path；128 KiB 性能也从约 31.33 恢复到 33 GiB/s 级别。

## 9.4 Legacy Pools对基准的污染

迁移到 single master pool期间，旧 pool_4\_、pool_8\_ 与 master_pool\_ 曾同时存在。即使旧 pool 不执行实际复制，额外的常驻线程、sleep/wakeup 和调度状态也会污染 benchmark，造成偶发低值。删除 legacy pools 后 1 MiB A/B 数据明显更稳定。这说明性能实验中“未执行任务的基础设施”也可能成为噪声来源。

## 9.5 Static Destruction Order

正常 scope lifetime测试无法发现进程退出阶段的问题。专门的 shutdown probe 证明 function-local static MemoryRuntime 会先于某些 global object destructor 析构。如果后者调用 TLSS memcpy，就可能访问已销毁 Runtime。最终采用 Immortal Runtime 消除该依赖顺序。这个问题说明 production hardening不仅是“多跑几次性能”，还必须验证语言层和对象生命周期层面的边界。

# 10. V1 限制、V2 演进方向与结论

## 10.1 V1 已知限制

- 当前策略阈值主要来自 i7-14700K 与当前内存配置的实测，不保证跨平台最优。

- Parallel Runtime 采用 single-flight，并发请求抢不到 pool 时直接 Direct NT fallback，不支持多个请求动态共享 worker。

- 当前 NonTemporal backend 使用 AVX2 YMM 实现，未包含 AVX-512 backend。

- 拓扑模型覆盖 package/core/P-E core，但尚未加入 NUMA node和 memory locality。

- memcpy 语义不支持 overlap；重叠复制应使用 memmove 语义的独立实现。

## 10.2 V2 可演进方向

V2 最有价值的方向不是继续手工调整某一个 128 KiB 阈值，而是让 Runtime 更自适应：启动阶段执行 micro calibration，测量 Direct NT、REP、Parallel NT 的交叉点；根据 CPU model、memory bandwidth、可用 P-core 数量自动生成 RuntimePolicy。这样“机制通用、策略平台化”会比维护大量硬编码 profile 更稳健。

- Adaptive RuntimePolicy：启动时微基准生成 threshold。

- NUMA Awareness：按数据所在 NUMA node 选择本地 worker。

- AVX-512 / 新 kernel：根据 CpuCapabilities 增加更宽向量后端。

- Multi-request Scheduler：允许两个并发 memcpy 各自获得部分 worker，而不是 busy fallback。

- 跨 CPU 平台 profile：Intel 非混合、AMD Zen、不同 DDR 平台重新验证。

## 10.3 结论

TLSS memcpy V1 从最初的 AVX2 汇编练习逐步演化为一个完整的 memory copy subsystem。最终成果的价值不仅在于某个 AVX2 loop，而在于建立了“Kernel → Policy → Capability → Executor → Runtime → Topology”的系统边界，并通过 benchmark、压力测试、故障注入和 sanitizer 验证逐步把每一层的不确定性收敛。

V1 的性能结论是：单 CPU 流式测试中 glibc 在大尺寸区间约 13.7~14.1 GiB/s，Direct NT 约 22.5~23.5 GiB/s；完整拓扑感知运行时在 256 KiB 粒度达到 35.65 GiB/s，相对 glibc 为 2.55×。同时，在 caller CPU 与 worker 冲突、受限 CPU 环境、E-core only、并发首次初始化、Runtime 构造失败和进程退出生命周期等场景下都有明确退化或保护策略。

因此，V1 已经达到“可作为 TLSS 内部稳定性能基础设施”的目标。后续升级应以 V1 为 baseline，通过可重复数据证明收益，再进入 V1.x 或 V2，而不继续无限修改当前稳定版本。

# 附录 A. 关键阈值与测试矩阵

## A.1 V1 Policy Thresholds

| **常量/区间**                   | **当前 V1 值** | **来源**                    |
|---------------------------------|----------------|-----------------------------|
| Default AVX2 threshold          | 2 KiB          | 小尺寸性能实测              |
| Streaming libc window           | 2~3 KiB        | glibc 与 AVX2 交叉实测      |
| Direct NT threshold             | 8 KiB          | 流式工作集实测              |
| Parallel desired 4 threshold    | 64 KiB         | 并行同步成本与吞吐实测      |
| Parallel desired 8 threshold    | 128 KiB        | 并行吞吐实测                |
| NT block size                   | 8192 B         | NT2 kernel / partition 设计 |
| NT destination alignment        | 32 B           | AVX2 YMM / kernel 前提      |
| Minimum useful parallel workers | 2              | 受限 affinity A/B 实测      |

## A.2 关键 Regression状态

| **类别**                                        | **最终状态** |
|-------------------------------------------------|--------------|
| Correctness / Backend                           | PASS         |
| Random / Alignment Stress                       | PASS         |
| Parallel NT Stress ×10000                       | PASS         |
| Sparse Worker Stress ×10000                     | PASS         |
| Concurrent First Use ×1000 processes            | PASS         |
| Concurrent Construction Failure ×1000 processes | PASS         |
| Runtime Lifetime ×1000 lifetimes                | PASS         |
| Restricted P-core / E-core-only                 | PASS         |
| ASan + UBSan full regression                    | PASS         |
| TSan regression                                 | PASS         |

## A.3 V1 Freeze Checklist

- Production build 中移除或宏隔离 failure injection / debug counters / shutdown probe。

- 保留核心 regression tests，确保未来改动可以对照 V1。

- 保存本报告中的两张最终性能基线表。

- 为 V1 建立 Git tag，例如 tlss-memcpy-v1.0。

- 未来任何 V1.x / V2 改动先回答：解决什么问题、性能收益多少、复杂度增加多少、是否破坏 V1 regression。

<table>
<colgroup>
<col style="width: 100%" />
</colgroup>
<thead>
<tr class="header">
<th><strong>文档定位<br />
</strong>本报告描述的是 TLSS memcpy V1 的当前实现、设计推导与实测结论。性能阈值属于当前平台 baseline，不应脱离测试条件解释为通用 CPU 常数。</th>
</tr>
</thead>
<tbody>
</tbody>
</table>
