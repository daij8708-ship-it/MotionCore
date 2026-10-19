# 测试报告 —— 增量 1 … 21

> 报告日期：2026-10-19
> 被测对象：`libmc_core`（协议、状态机、工艺/联锁/审计等核心库，零 Qt 依赖）+ `mc_loopback_demo` + **`mc_sim`（设备侧模拟器，真串口/伪终端）** + **Qt6 上位机 `mc_host`** + **负载实测工具 `mc_host_bench`** + **`firmware/`（STM32F103C8T6 最小固件与宿主端 `mc_fw_sim`）**
> 结论：**409 / 409 通过**（根工程 389 = 协议与核心库 32 个测试套 286 条 + 上位机 10 个测试套 101 条 + 无头自检 2 条；固件支线 20 条 = 帧/CRC 6 + 设备命令与安全 12 + 外部反馈 1 + 真 PTY 端到端 1）

| 增量 | 内容 | 累计结果 |
|---|---|---|
| 增量 1 | 帧编解码 / CRC16 / 流式拆包 / 有界环形缓冲 | 39 / 39 |
| 增量 2 | 载荷编解码 / 应答跟踪与超时处置 | 68 / 68 |
| 增量 3 | 传输抽象 / 进程内回环（含故障注入）/ 最小虚拟设备 / 上位机会话 | 90 / 90 |
| 增量 4 | 设备侧序列号去重（**重放应答**）/ 心跳调度与链路失效判定 / **按实测 RTT 配超时窗口** | 110 / 110 |
| 增量 5 | **设备 7 状态状态机**（命令门禁 + 故障锁存）/ 设备侧通信丢失保护（**F7**） | 129 / 129 |
| 增量 6 | 上位机侧**会话生命周期**（自动握手 / 退避重开 / 链路失效重连）+ 修掉"判了可重传却没人真发"的缺口 | 134 / 134 |
| 增量 7 | **Qt6 上位机五层骨架** + 进程内模拟设备直连 + **设备总览 / 运动控制 / 数据曲线**三页 + 无头界面用例 + 带判据的自检 | 166 / 166 |
| 增量 8 | 设备侧遥测**按自身周期补发**（造出与上位机 tick 率无关的压力源）+ 上位机**计时诊断**与 `mc_host_bench` 单线程负载基线 | 172 / 172 |
| 增量 9 | **PID 参数 / 通信调试 / 故障报警**三页 + 协议追加 **CLEAR_FAULT / CLEAR_FAULT_ACK**（人工确认是"锁存只能人工解除"在线路上唯一能表达的动作） | 178 / 178 |
| 增量 10 | **`SerialDevice`（QSerialPort）+ `SerialTransport` + 系统设置页**；串口路径用**内核伪终端**验证 | 183 / 183 |
| 增量 11 | **SQLite 历史落库**（WAL + 批量事务 + 按 (会话, seq) 覆盖写）+ **历史数据页**（会话/样本/事件/命令查询 + CSV 导出） | 188 / 188 |
| 增量 12 | **设备侧运动模型补全**（一阶惯性 / 编码器量化 / 固定窗口速度估计 / PI(D) 抗积分饱和）+ **`mc_sim` 可执行程序**（真串口或伪终端）+ **伪终端自检** | 205 / 205 |
| 增量 13–18（P0/P1 功能批） | **工艺配方**（模型/校验/持久化 + 配方页）· **自动任务调度**（Sequencer 引擎 + 任务页）· **报警分级与代码表 + 报警历史落库** · **软件联锁规则表** · **操作员权限与操作审计** · **回零流程与软限位** | 352 / 352 |
| 增量 19（P1 收尾批） | **多轴抽象与双轴协调**（速度缩放式同步规划 + 轴组编排与全组安全闸 + 双轴协调页）· **日志分级与滚动**（级别过滤 + 有界环形 + 滚动文件 + 日志页） | 389 / 389 |
| 增量 20（固件支线） | **最小固件**（协议/状态机/命令/运动模型/遥测/故障）+ **STM32F103C8T6 裸机端口**（USART1 中断 + SysTick + TIM3 PWM）+ **宿主端 `mc_fw_sim` 真 PTY 端到端** | **408 / 408**（根工程 389 + 固件 19） |
| 增量 21（固件上板） | **编码器外部反馈**（TIM4 正交解码 PB6/PB7 + `externalFeedback` 跳过虚拟被控对象）+ **双向驱动**（TB6612 AIN1=PA7 / AIN2=PB0）+ 真串口上板联调 | **409 / 409**（根工程 389 + 固件 20） |

> 增量 13–18 是连续推进的一批功能，**中间状态没有各自定稿过累计数**，
> 因此这里只记最终值（与增量 8–11 同一处理：不补一个编出来的中间数）。
> 增量 19 是一批独立收尾的功能，有各自定稿的累计数。
> 增量 21 只新增 1 条固件用例（外部反馈取代虚拟被控对象），
> 其余是端口实现与上板联调：**上板联调的结论是实测记录，不是自动化用例**，
> 见 §4.6 与 §7 的硬件栏。

---

## 1. 结果绑定（本轮验证对应哪份内容）

本报告**不用 commit hash 绑定**（提交与报告是并行推进的，报告先于提交定稿），
改用**源码内容指纹**绑定。**任何源码改动都会使本报告失效，必须重跑。**

### 增量 21（当前，覆盖增量 1–20）

- **被测产物共 156 个文件、35688 行**（构建脚本 + `libmc_core` + 上位机 + 固件支线 + 测试 + 演示与实测程序）
- **指纹口径（可逐字复现，须在项目根执行）**：

  ```sh
  find CMakeLists.txt common tools host firmware -type f | LC_ALL=C sort | xargs sha256sum --text | sha256sum --text
  ```

  结果：`ee21b408e1e24501e3bce9f54780c3e28ebaeb36d536268e2de7ed62c77802f9`

  > 口径从增量 7 起**纳入 `host/`**，从增量 20 起**纳入 `firmware/`**。
  > 固件构建目录刻意放在仓库根的 `build-fw*`，不放进 `firmware/`，否则构建产物会混进指纹。
  > 同时仍在刻意**排除 `docs/`**：口径包含 `docs/TEST_REPORT.md` 会形成自指
  > （这个文件里就写着指纹本身）。`docs/PERF.md` 同理不参与 —— 它是文档，不是产物。

  已在 **WSL** 与 **Windows Git Bash** 两侧各算一遍，得到**同一个值**（理由与踩过的坑见 §6 的 C23）。

> **口径里那两个 `--text` 是必要的，不是装饰。** `sha256sum` 默认按平台决定标记：
> Linux 输出两个空格，Git Bash（MSYS）输出 `*`（二进制模式）。于是**同样的文件、同一句命令**，
> 在 Git Bash 里算出来的聚合值会**不一样**（实测 `52a1831a…` vs `bd26904f…`）——
> 而两者逐文件哈希其实完全一致。加上 `--text` 之后两个平台给出同一个值，
> 指纹才真正"换台机器也能复现"。**这一条是核指纹时当场发现的**（见 §6 的 C23）。

> **指纹口径改过两次，这里说清为什么。**
> ① 增量 3 用的是"按我手写的 31 个文件清单拼起来取 sha256" —— 手写清单**会漏掉新加的文件**，
> 而漏掉的文件不会让指纹报错，只会让它悄悄失去约束力。所以撤掉手写清单，改成机器枚举。
> ② 先改成"全树枚举"，立刻撞上**自指**：口径包含 `docs/TEST_REPORT.md`，而这个文件里就写着指纹
> 本身 —— 改一次文档指纹就变，**永远对不上**。
> 于是收紧为**只覆盖参与构建的产物**（`CMakeLists.txt` + `common/` + `tools/`），文档不参与。
> 这样指纹约束的正好是"会被测的那一部分"，改文档也不会让它漂移。

### 历史增量

| 增量 | 文件数 / 行数 | 指纹（旧口径：手写清单拼接） |
|---|---|---|
| 1 | 14 / 1475 | `f5bfc404fec1343350a8941fa67e5da9428f04ad8f5ab2fe80fde46f5e077aba` |
| 2 | 20 / 2522 | `3f4f2c98c709040f402d1fefb841d22309f379bb851dc64df03f562a708b8c00` |
| 3 | 31 / 4120 | `680715f0dd6f32e3b9ac6ec8dcd87d692528d28db3f09c2e0afc70a6960994e7` |
| 4 | 34 / 5129 | `cab3cf94334ab1583b68d78d78812099afe07581994873edcd1d1db6229637cb` |
| 5 | 37 / 6140 | `2a75fd9a234e6379256a10f5a779261b9914c5d9b3ae41ccd097995448506671`（机器枚举口径） |
| 6 | 37 / 6633 | `bd26904f0ccdb6454315097826fbb7f4ec68e9c32af03eec72a2fd912e8e5245`（口径：`CMakeLists.txt common tools`，**不含 `host/`**） |
| 7 | 63 / 10154 | `6561a296a69dc84a22eb27e856675b0b6d4411f0f13ed487f414e3b1f7e214ec`（口径：`CMakeLists.txt common tools host`） |
| 8–11 | 78 左右 / 14000 左右 | ⚠️ **未逐轮记录**：增量 8–11 的中间状态没有各自定稿过指纹（那几轮是连续推进的），这里不补一个编出来的值 |
| 12 | 89 / 16977 | `4c66fa5c51996b03c9da6099f00519fa91e4ca5426085dc8ac6a8f55a1f7572f` |
| 13–17 | ⚠️ 未逐轮记录 / 未逐轮记录 | ⚠️ 同 8–11：P0/P1 功能批是连续推进的，中间状态未各自定稿 |
| 18 | 124 / 29913 | `8d2ea4657d2b6747c8b65fdfadae62e1d5bece07170c45115c37472ca28fab7e`（口径：`CMakeLists.txt common tools host`，**不含 `firmware/`**） |
| 19 | 139 / 32825 | `cb1a7b1ac42bc53fc11aac80b4a1dea560ff9905e1568e0b3a2162f40c415834`（同上，不含 `firmware/`） |
| 20 | 156 / 35553 | `d421357fe4813b2a8e38a1fa2c4155c99e21df9e638e32585005f71e5b5d62fe`（口径：`CMakeLists.txt common tools host firmware`） |
| 21 | 156 / 35688 | `ee21b408e1e24501e3bce9f54780c3e28ebaeb36d536268e2de7ed62c77802f9`（当前口径：同上） |

## 2. 测试环境

| 项 | 值 |
|---|---|
| 宿主 | Windows 10（构建在 WSL 内完成） |
| 发行版 | WSL `Ubuntu-22.04`，用户 `dev`，`nproc` = 20 |
| 编译器 | `g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0` |
| CMake | `3.22.1` |
| GoogleTest | `1.11.0`（系统包，通过 `GTestConfig.cmake` 导入目标） |
| 固件交叉编译器 | `arm-none-eabi-g++ 10.3.1`（Ubuntu jammy 包，解包到用户目录使用；Cortex-M3、`-mthumb`、`-Os`、`-fno-exceptions -fno-rtti`） |
| 固件产物 | `text 8623 B / data 0 B / bss 272 B`；入口 `0x08000425`（Thumb 位）；初始 SP `0x20005000`；向量表 59 项 |
| 固件测试构建 | 宿主 `g++ 11.4.0`、`Debug`、`build-fw`；20 条含 1 条真 PTY 端到端 |
| 语言标准 | C++17（`CMAKE_CXX_EXTENSIONS OFF`） |
| 构建类型 | `Debug` |
| 告警策略 | `mc_core` / 演示程序 / 上位机库与程序：`-Wall -Wextra -Werror`；测试：`-Wall -Wextra` |
| 源码位置 | **本报告的数字来自仓库内 `<repo>/build`（即 `/mnt/c/...` 上的构建树）**。⚠️ `/mnt/c` 上 `make` 会因时钟偏移**静默跳过重建**（改了源码却不重编），所以采集脚本一律**先 `touch` 全部源文件再构建**（见 §6 的 C3/C6/C11/C38）。早期增量曾在 WSL ext4（`~/mc9`）内构建以避开 `/mnt/c` 的 I/O 与权限问题；两种位置产出的**结论一致**（§1 的指纹在 WSL 与 Git Bash 两侧一致即为佐证） |
| Qt（本轮新增） | `6.2.4`（`qt6-base-dev`）；GUI 另装 **`libgl-dev`**（缺它 `Qt6Widgets` 配不上）；中文界面另装 **`fonts-noto-cjk`**（缺它中文全是方框） |
| 界面渲染方式 | ① **无头**：`QT_QPA_PLATFORM=offscreen` —— 本轮全部界面用例、自检与三张截图都是这么出的；② **真显示**：`QT_QPA_PLATFORM=xcb`（WSLg，`DISPLAY=:0`）—— 自检 **13 条判据全 ok、退出码 0**。⚠️ 该环境**没有 `wayland` 平台插件**（只有 `xcb/offscreen/minimal/vnc/linuxfb/eglfs/minimalegl/vkkhrdisplay`），不指定平台时 Qt 先试 Wayland 失败再退回 xcb，会多打一行 `qt.qpa.plugin: Could not find the Qt platform plugin "wayland"` |

> **本报告里的测试数字一律来自上表的 `Debug` 构建。**
> 增量 8 的**性能**数字是另一套环境（`Release` + `-O3`，同一台机器、同样 `offscreen`），
> 那份的条件、原始输出与未覆盖面记在 `docs/PERF.md` —— **两张表的环境不可混用**：
> 拿 `Debug` 的耗时当性能指标，或拿 `Release` 的耗时当"测试通过"的证据，都是错的。

## 3. 复现命令

```bash
cd /path/to/MotionCore   # 仓库根（WSL 下可换成 /mnt/c/... 的实际挂载路径）

# /mnt/c 上必须先“碰”一遍源文件：否则 make 会因时钟偏移跳过重建（改了代码却不重编，见 §6 C3/C38）
find CMakeLists.txt common tools host firmware -type f \( -name '*.cpp' -o -name '*.hpp' \) -exec touch {} +

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

# 固件支线：独立构建，20 条（含真 PTY 端到端）
cmake -S firmware -B build-fw -DCMAKE_BUILD_TYPE=Debug
cmake --build build-fw -j"$(nproc)"
ctest --test-dir build-fw --output-on-failure

# 固件核心扮演设备：打印 /dev/pts/N，填进上位机「系统设置」页
./build-fw/mc_fw_sim --pty

# 交叉编译 STM32F103（需要 arm-none-eabi-g++ 在 PATH 上）
cmake -S firmware -B build-fw-stm32 \
  -DCMAKE_TOOLCHAIN_FILE=firmware/cmake/arm-none-eabi.cmake \
  -DMC_FW_PLATFORM=stm32f103 -DMC_FW_BUILD_TESTS=OFF -DMC_FW_BUILD_SIM=OFF
cmake --build build-fw-stm32 -j"$(nproc)"

# 端到端演示（可运行产物，不依赖 Qt 与硬件）
./build/tools/mc_loopback_demo

# 设备侧模拟器：在伪终端上跑"上位机 ↔ 设备"整条会话，退出码即结论（已进 ctest）
./build/tools/mc_sim --self-test

# 设备侧模拟器：起一对伪终端，把打印出来的 /dev/pts/N 填进上位机「系统设置」页
./build/tools/mc_sim --pty

# 无头自检（带 13 条判据，失败即非 0 退出）+ 十二页截图
QT_QPA_PLATFORM=offscreen ./build/host/mc_host --selftest=3 --shot-prefix=/tmp/shot

# 单线程负载实测（Release；条件与解释见 docs/PERF.md）
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j"$(nproc)"
QT_QPA_PLATFORM=offscreen ./build-rel/host/mc_host_bench --seconds=5

# 浸泡（≥1 h；内存/库文件/计数器随时间的变化见 docs/PERF.md）
QT_QPA_PLATFORM=offscreen ./build-rel/host/mc_host_bench --soak --seconds=3600

# 想在有显示器的环境里看窗口（WSLg）：
./build/host/mc_host
```

> 注意：采信任何结果之前**先确认 CONFIGURE / BUILD 为 OK**。
> 若构建失败而继续跑 `ctest`，跑的是上一次的二进制（见 §6 的 C3、C6、C11）。

## 4. 实测结果

| 检查 | 观测值 |
|---|---|
| CONFIGURE / BUILD / CTEST | `OK` / `OK` / `OK` |
| ctest（根工程） | `100% tests passed, 0 tests failed out of 389` |
| ctest 用例数（根工程） | `Total Tests: 389` |
| ctest（固件支线） | `100% tests passed, 0 tests failed out of 20` |
| ctest 用例数（固件支线） | `Total Tests: 20` |
| gtest 独立口径（根工程） | `286 + 81 + 20 = 387 tests from 42 test suites ran` / `[  PASSED  ] 387 tests`（+ 2 条 `host_selftest` / `mc_sim.SelfTest` 由 ctest 直接执行，不是 gtest 用例） |
| gtest 独立口径（固件支线） | `20 tests from 3 test suites ran` / `[  PASSED  ] 20 tests`（`FirmwareFrameIo` 6 + `FirmwareDevice` 13 + `FirmwarePty` 1） |
| 上位机自检 | 13 条判据**全部 ok**，退出码 `0` |
| 设备侧自检（`mc_sim --self-test`） | **51 项检查全通过**，退出码 `0`（伪终端上的握手/运动/停车、上行损坏、F7 通信丢失与确认清除、**回零与软限位**） |
| 编译告警 | 无（`-Werror` 下通过） |
| 演示程序退出码 | `0` |
| 负载实测工具 | `mc_host_bench` 两档各跑通、浸泡模式跑通，退出码 `0`（数字见 `docs/PERF.md`） |
| Qt 依赖（核心侧） | `mc_core_tests` / `mc_loopback_demo` / `mc_sim` **三个**二进制 `ldd \| grep -ci qt6` 均为 **0** |

**根工程两个独立计数器一致**（ctest 389 ↔ gtest 387 + 自检 2）；**固件支线两个独立计数器一致**（ctest 19 ↔ gtest 19），不存在"注册了但没跑"的用例。

> `--selftest` 那一份为什么不算 gtest 用例：它跑的是**整套真实链路**（建窗口 → 起定时器 →
> 握手 → 下发命令 → 收遥测 → 出图），并用 `ctest` 直接吃它的**退出码**。
> 把它塞进 gtest 只会多一层壳，判据本身没有变化。

### 4.1 用例分布（32 个测试套，286 条 + 自检 2 条）

这 32 个套**全部属于 `libmc_core`**（零 Qt 依赖）。上位机侧的 9 个套见 §4.2。

| 测试套 | 条数 | 覆盖内容 |
|---|---:|---|
| `Crc16` | 6 | 标准向量、逐位/查表双实现交叉校验、流式累计一致性 |
| `FrameCodec` | 8 | 线序钉死、超限/版本/坏 SOF/截断/坏 CRC 拒绝、随机往返 |
| `StreamDecoderTest` | 9 | 整块↔逐字节等价、任意分块等价、半帧暂存、坏帧后仍可用、超长拒绝、垃圾前缀精确计数、未知类型透传、版本跳过 |
| `ProtocolPolicy` | 9 | STOP 单向、运动指令需 ACK 但不可重传、幂等命令可重传、心跳、遥测免应答、ACK 配对、**CLEAR_FAULT 需应答且可重传**、**HOME 需应答且绝不自动重传** |
| `RingBuffer` | 9 | 容量常量、FIFO、覆盖计数精确、持续写入有界、clear 语义 |
| `Payload` | 20 | IEEE-754 小端位模式、遥测布局钉死、各类载荷线长、全长拒绝、失败不留半成品、NaN 结构合法语义非法、量程/枚举拒绝、确认码载荷往返与"码 0 非法"、**遥测状态位组（`statusFlags` @20）与"未定义位往返保留"**、**`HomeParam` / `SoftLimits` / `ParamState` 三类新载荷的线长与语义校验** |
| `AckTracker` | 14 | 无 ACK 类型拒绝、重复 seq 拒绝、应答配对、陈旧应答、超时边界、重试窗口重算、**运动指令永不重传**、重试用尽、**心跳漏拍判 `Dropped` 而非 Fault**、**窗口调整只作用于新请求** |
| `LoopbackTransportTest` | 10 | 默认无故障、方向独立、断开可见于两端、延迟、丢字节精确计数、损坏精确计数、有界队列、分向统计、通道所有权 |
| `LinkMonitor` | 14 | 定频发心跳且迟到不追赶、RTT 采样、在途容量上限丢弃最旧、连续丢 N 拍判失效、应答后恢复、时钟回退不误判、无样本时退回显式初值、窗口随 RTT 变化并被上下限夹取、推荐值只在真变化时上报 |
| `DeviceState` | 20 | 线上取值表钉死、**192 个 (8 状态 × 锁存位 × 12 事件) 组合逐格对照规格表**、被拒事件不改状态不改锁存、STOP 处处合法且幂等、**从 `Home` 被打断只进 `Stopping`、不算建立过零点**、故障只能经 Recovery 离开、**故障跨断线重连锁存**、握手超时进 Fault、重握手不打断运动也不复位故障、断线时握手被拒、**命令门禁表 8 状态 × 10 命令逐格断言（含 `CLEAR_FAULT` / `READ_PARAMS` / `HOME` / `SET_LIMITS`）**、F7 窗口必须长于心跳周期 |
| `SimulatorModel` | 17 | 上报速度落在量化噪声带内且落在"整数个计数"的格点上、位置读数只能是整计数、惯性时间常数大→同样指令下响应更慢、增益全 0 的速度环＝纯前馈且无稳态误差、位置环零增益不动（负对照）、给上增益后收敛到目标、停机后再使能不因陈旧积分窜动、**回零到位即把该处定义成零点并回 Ready**、**回零在时限内走不完就以具名故障收场**、**回零半路被 STOP 停下但零点不建立**、**「本轮回零已建立零点」位进入时清零、完成才置位**、**该位随每一帧遥测上路（解回来的字节里确实有它）**、**越软限位即撤输出并进 `OverLimit`**、**关掉限位就一律不判（数值还在也不判，负对照）**、**把当前位置关在门外的限位被明确拒掉**、**合法限位真的存进设备、并能从回读里拿回同一组**、**回零期间不判限位** |
| `SimLoop` | 4 | 请求-应答在一轮里走通（与传输无关）、**设备不依赖上位机说话也持续上报**、**损坏序号是"整条上行流"上的、不是每拍重数**（且坏字节真的过不了 CRC）、不配置注入时一个字节都不改（默认值门禁） |
| `FdTransport` | 2 | 真 fd 上双向搬运并记账、关闭幂等且关后不再读写 |
| `FdTransportAndPty` | 3 | 设备挂在 master 端时**从端写进去的帧会得到 HELLO_ACK**、**从端尚未打开时的 EIO 不算链路断（但必须记账）**、**二进制载荷原样穿过行规（0x0A/0x0D 不被改写）** |
| `EndToEnd` | 31 | 命令全应答、遥测收敛、**SET_TARGET 在 Ready 放行但不启动电机**、**SET_MODE 在 Running 被拒**、非法参数拒绝、单向停止、**丢包→Fault 且不重发**、**损坏链路零遥测**、**伪造 NaN 被拒且不覆盖好样本**、字节守恒、断开上报、NAK 帧头/载荷 seq 一致、**重复 seq 重放应答且不重复执行**、**HELLO 清空去重窗口**、**实测 RTT 撑开窗口后高延迟不误判**、**心跳漏拍不算故障 + 连续丢拍判链路失效**、**F7 保护下车 + 其负对照（关掉窗口就带着陈旧目标一直转）**、**会话自己握手到就绪**、**没有序列号来源就不启用握手层**、**失败轮次退避重开且链路恢复即自动重连**、**判了可重传就会真的重发（线上帧数）**、**应答丢失时绝不重发运动指令且以超时结清**、**F7 静默 + 链路失效 → 会话作废 → 自动重连但设备仍锁存故障且未自动恢复运动**、**设备按自身周期产帧而不是按上位机 tick 率**、**单拍产帧上限与"欠发"计数（且上限能被单次交换搬走）**、**周期为 0 时不补发也不死循环**、**高密度遥测真的顺着链路到上位机且一帧不丢不拒**、**毫秒量化时钟下帧数在 0/1/2 之间抖但总数守恒**、**CLEAR_FAULT 只在 Fault 放行、只认当前锁存的那个码、确认后回 Ready 且故障位归零**（含"确认错码被拒"） |
| **`AlarmCatalog`** | **5** | 每个已知报警码都有稳定 token 与一条规格、除 `None` 外没有码落在表外、级别与"是否需人工确认"遵守处置规则、**设备故障码与线路字段是同一组数字**、每一级都有名字 |
| **`AlarmLog`** | **13** | 同一原因重复发生**合并成一条而不是刷屏**、不同码各占一条、**复燃需要重新确认**、条件恢复**不自动移除条目**、警告与提示**永不阻止启动**、只有规格要求确认的才算"阻塞项"、**未知码按最坏情况对待**、`acknowledgeAll` 只数仍待确认的那些、`clear` 不动除列表外的任何东西、**满表永不驱逐未确认的阻塞报警**、已确认的先被驱逐、确认与清除**如实报告有没有找到东西**、容量有界且默认值不为零 |
| **`AuditLog`** | **4** | 每次尝试都拿到序号且**两种结局都被计数**、窗口有界而计数器不封顶、**容量为 0 时仍保留最新一条**、空日志没有"最新一条" |
| **`AuditTable`** | **7** | **用例里的动作清单覆盖了每一枚举值**、每个动作都有 token 与所需权限、每个权限都被某个角色授予、**观察者做不了任何有后果的事**、操作员能运行机器但改变不了"怎么运行"、工程师在表内畅通无阻、**未知角色或未知权限对所有人都不放行** |
| **`InterlockTable`** | **6** | 每条规则都有稳定 token 与存在的理由、**一切就绪时什么都不拦**、每条阻塞规则**只读它自己那个字段**、**警告永不阻塞但仍要被报出来**、**所有不满足的规则一次全报**、断链与端口未开是同一条规则 |
| **`ParamState`** | **3** | 逐字段往返（含软限位）、长度必须精确匹配、**荒谬值被拒**（免得被误当成一份合法的回读） |
| **`Recipe`** | **19** | 编码是规范且逐字节确定的、逐字段往返、最长名字也往返、校验和与独立的按位 CRC 一致、**校验和覆盖名字与版本在内的每个字段**、每一种截断长度都被拒、尾部多余字节被拒、**荒谬的名字长度被如实报成"长度不对"而不是截断**、解码失败不动调用方的原配方、合法配方零错误、空名字被拒、超长名字被拒、**版本 0 被拒（它只表示"没设过"）**、模式超出行模式范围被拒、**目标值上界随模式变（单位不同）**、停机模式带非零目标被当成自相矛盾拒掉、非有限目标在"是否为零"之前先报、三个增益共用同一条规则、每个错误码都有 token |
| **`SequenceEncoding`** | **4** | 往返且确定、校验和覆盖每个字段、坏字节被拒而不是猜、**解出来的程序仍要过同一套校验** |
| **`SequenceHome`** | **4** | HOME 步的速度与时限都必须给且要在合理区间、**HOME 步后面必须紧跟 WaitReady**、该步被派发成命令后能收敛到结局、**存下来的类型字节是 7 并且能往返** |
| **`SequenceValidation`** | **6** | 合法程序通过、程序级字段被检查、步骤参数被检查、**目标上界跟着程序自己设的模式走**、**非法程序被整份拒绝且旧程序原封不动**、WaitReady 把它自己的时长当作有界超时 |
| **`SequencerRun`** | **10** | 步骤按序执行且每条都必须被确认、**被 NAK 的步骤就地终止整份程序**、**超时的步骤绝不自动重试**、被本地拒掉的步骤以原因失败、**没有程序或已在运行时 START 被拒**、**第一个确定的理由胜出、后面的被忽略**、中止会停下程序并保留理由、**从未发出的命令结局被忽略**、WaitReady 步在设备就绪时立刻放行、**设备始终不收敛时 WaitReady 终止程序** |
| **`AxisGroup`** | **13** | **每一轴的 行程/速度 都算回同一个公共时长（这才是"同时到达"的定义，只钉速度值等于给恰好算对的实现让路）**、速度带符号、**一轴不动时它的速度是 0 而公共时长仍由动的轴定**、三轴也照样缩放、每轴速度都不超上限、空轴组/超上限/速度非正/非有限/行程超界/全零行程各自被拒且**失败的计划里不带可用的速度**、错误码与措辞各自非空且互不相同 |
| **`LogLevel`** | **3** | 级别 token 往返且大小写不敏感、**认不出的名字退回 `Info` 而不是"全都不记"**、每一级都有自己的 token 与中文措辞（两级长得一样人就分不出） |
| **`LogLine`** | **1** | 一行日志的文本里带级别 token、带 `[分类]`、带正文 |
| **`LogRing`** | **3** | **满了丢最旧并计数丢弃**、容量 0 夹到 1 而不是"一条存不下还每写一条崩一次"、**清屏只清"还剩几条"、不清"一共丢过多少"** |
| **`RotatingFile`** | **4** | 超限轮转、**保留 N 份时最老的被丢掉而最新的还在**（不断言"当前文件非空"——最后一次写入刚好触发轮转时它本来就是空的）、保留 1 份时原地重开而不轮转、**重开时追加而不是截断**（崩溃前那几行必须还在）、**打不开时明确失败且此后仍安全可调** |
| **`Logger`** | **5** | **过滤在入口：被挡掉的既不进环形也不写文件**，两个计数器各记一边、`wouldLog` 在拼字符串之前就能问、**文件那一路只拿到过了级别的那些**、**写文件失败被计数而不是静默吞掉**、没挂文件时环形照常工作 |

### 4.2 上位机侧用例（10 个套，101 条 = 81 + 20）

| 测试套 | 条数 | 覆盖内容 |
|---|---:|---|
| `CurveModel` | 14 | 通道取值/名称/单位、**取窗含两端**、窗口比数据宽时只给交叠段、**反向窗口返回空**、空缓冲返回空、按通道取值；抽稀**不超预算**、**等价于原样返回**（没超限就不动它）、预算为 0、**全局极值必留**（尖峰刻意放在桶内部，见 C27）、**输出按时间排序**、**只选点不造点**、单点窗口 |
| `DeviceManagerTest` | 35 | 未连接时命令一律不发（且**不留下 NotSent 噪声**）、连上即自动握手到就绪、SET_MODE+START 到 Running 且两条都回来"已确认"、运行中改模式**被拒且无副作用**、STOP 记成"单向"且**设备真的离开 Running**、下行全丢→运动指令**超时且只发过一帧**、上行全丢→会话作废→**恢复后自动重连（轮次增加）**、断开时在途命令**以超时结清**、遥测入库且"清屏"不动累计计数、缓冲里存的是**真实字段不是一片 0**、计时诊断默认关闭且只在打开时记样本、**报警连同代码/级别/确认状态一起落库**、**任务按序跑完且每一步都要先被确认**、**设备拒绝的那一步就地停并说明原因**、**存在未确认的阻塞报警时程序被拒**、**联锁表列出每条规则并给出当前判定**、**跑任务途中链路断掉即停**、**配方下装按序发三条命令并核对回读**、**运行中下装配方被带原因拒绝且一个字节都不发**、**非法配方在出门之前就被拒**、**链路丢失把在途下装结算为失败**、**观察者能看但什么也动不了**、**操作员能运行机器但改不了"怎么运行"**、**做过什么与没做成什么**都进审计流与数据库、**拒绝文案说清是三类里的哪一类**、**回零只有在设备回到 Ready 时才判成功**、**运行中回零被拒且设备不受影响**、**回不到零的回零被报成失败并报警**、**被操作员停下的回零被报成"中止"而不是成功**、**回读之前对设备存的参数一无所知**、**软限位被接受、存下来、并能从设备回读**、**把当前位置关在门外的限位被 NAK**、**结构性不可能的限位根本不上线**、**程序可以先回零再等零点建立**、**带 HOME 步的程序必须带一个等停步** |
| `HistoryStore` | 7 | 重开库后会话/样本/事件/命令**都还在**、**批量落盘有据可查**、CSV 带 UTF-8 BOM 与可读表头、**报警连同代码/级别/确认状态落库**、**清除后同一码再报是新增一行而不是覆盖旧行**、打不开的库明确失败且仍可安全查询、**审计行不挂会话追加、且不去重** |
| **`RecipeStoreTest`** | **8** | 保存与列举**逐字段往返**、**同名同版本不能存两次**、**被外部改过的行报成"损坏"而不是静默加载**、**只动名字也会被检出**、非法配方在存储边界被拒、更新**不能窃取别的行的版本**、删除只删选中那一行、打不开的路径带原因失败 |
| **`SequenceStore`** | **7** | 逐字段往返且**重开后仍在**、同名同版本不能存两次、非法程序在存储边界被拒、**被外部改过的程序报成"损坏"**、**解不开的字节也报"损坏"**、更新与删除只动选中那一行、打不开的路径带原因失败 |
| `SerialTransport` | 2 | 在**真 tty（伪终端）**上双向搬运、**读多了一帧的余量必须留着下一次**（丢了会被上层当成 CRC 错） |
| `SerialDevice` | 2 | 在伪终端上**握手 → 下发 → 回读遥测**整条路（观测值全部来自遥测帧）；**打不开的端口明确失败并给出原因**，且此后所有观测访问器都安全可调 |
| **`AxisGroupHost`** | **4** | 规划在没连设备时也**同步且不超限**（公共时长按最远轴定、其余按比例慢下来）、**没连上时拒发且说得出为什么**（链路上一条 `SET_TARGET` 都没有）、**协调移动把命令发到两轴并跑到"走完"**（且**启动绝不与"切模式"同拍**）、**任一轴故障时健康那一轴也立刻收到 `STOP`** |
| **`DeviceLoggingHost`** | **2** | 连接与命令结局真的被 `DeviceManager` 写进日志器（`link` / `cmd` 两类）、**级别过滤在入口生效**（调到只记错误时连"已连接"都不进环形、计数落在"被挡掉"上） |
| `UiSmokeTest` | 20 | 模拟数据在横幅与状态栏都可见、**十二页标题**与越界切页返回 false、**点连接按钮真能到"就绪"**、**点下发模式+启动后表格出现"已确认"**、运行中改模式**表格出现"被拒绝"且原因列正确**、**运动页说明是哪条联锁在拦启动**、曲线页**真的重绘过且画出点数 > 0**、注入上行断开→**界面出现报警**并撤除后自动恢复、**故障锁存跨重连仍在且只能靠"确认清除"解除**、**设置页切换数据源并如实显示当前生效来源**、**设置页切换操作员角色、拒绝落进审计流**、**历史页能读回连接期间记录下来的数据**、**任务页保存程序并把每一步跑到被确认的结局**、**配方页保存、下装并如实报告每一步**、**运动页回零并依据设备那一位报出"零点已建立"**、**运动页把被打断的回零报成"零点没建立"**、**参数页下发软限位并从回读里拿回它们**、**任务页提供 HOME 步并要求紧跟一个等停步**、**双轴页连上两轴并把一次协调移动跑到"走完"（两行结局都是"已确认"，且**「全组停止」按下去状态栏真的改口**）**、**日志页按级别过滤并报告日志文件与保留份数（且**「清空视图」清掉行而不抹掉"环形丢弃"计数**）** |

> 界面用例的判据一律**读控件**（`findChild` + `text()`），不读内部成员 ——
> 读成员等于绕开了这一层，界面写错了照样绿。所以这些用例比"能编译"强、又比"像素级视觉验收"弱：
> 它证明的是**控件存在、被点动、并显示了正确的结果**，不证明好看。

### 4.3 无头自检的实测输出（`--selftest=3`）

```
设备类型        : 进程内模拟器
会话            : 就绪（1 轮握手）
tick / 时长     : 264 拍 / 2.6 s
发送帧          : 9（编码失败 0 / 发送失败 0）
应答 / NAK      : 对上 9 / NAK 0
遥测            : 接收 262 / 拒绝 0 | 缓冲 262 条（丢弃 0）
解码拒绝        : CRC 0 / 长度 0 / 版本 0 / 重同步字节 0
超时            : Fault 0 / 重发 0 / 心跳漏拍 0
设备            : Running | 使能 是 | 速度 300.0 deg/s | 位置 462.6 deg | 目标 300.0
命令行          : 共 5 条（新→旧）
    SET_TARGET   #7     已确认
    SET_PID      #5     已确认
    SET_TARGET   #4     已确认
    START        #3     已确认
    SET_MODE     #2     已确认
曲线控件        : 重绘 2 次（自检里显式渲染过）
界面刷新信号    : 52 次 / tick 264 拍（分频 5）
（判据失败 0 条）
```

**9 帧 = 5 条命令 + 4 次握手/心跳**：5 条命令各一帧，握手 `HELLO` 1 帧，
2.6 s 内发了 3 拍心跳（健康链路上反复被确认，所以"重发"为 0）。

**"界面刷新信号 52 次 / tick 264 拍"就是 ADR-006 的证据**：`264 / 5 ≈ 52`。
（tick 数与时长每次略有不同 —— 脚本给的是 3 s 墙上时间，启动那 200 ms 与调度抖动都会影响
实际拍数；所以 13 条判据里凡是涉及量的，都用**比值与关系**，不用绝对数。）

本轮**改正了这条判据的口径** —— 原先比的是"控件重绘次数 vs tick 数"，但在无头环境里
没人投递绘制事件，重绘次数只反映自检主动渲染了几次，拿它比 tick 数**等于自己造结论**（见 C26）。

### 4.4 演示程序与界面截图

截图由 `--shot-prefix` 生成（`offscreen` 渲染）。增量 7 的**三页时代**逐张看过：
总览页五组数值齐全、运动控制页的控件记录都在表格里且结果列为"已确认"、
曲线页三条曲线各用自己的纵轴刻度（速度 / 位置 / 目标），图例给出当前值与范围。

> 此后界面从三页长到**十二页**（新增 PID 参数 / 工艺配方 / 任务调度 / 故障报警 / 历史数据 /
> 系统设置 / 通信调试 / 双轴协调 / 运行日志）。**十二页的逐页观感验收是人工在真显示（WSLg，`xcb`）下操作的**；
> 自动判据只到"控件存在、被点动、显示了正确结果"这一层（见 §4.2 的说明）。
> 本报告**不逐页复述视觉细节** —— 那需要重出截图并逐张看，属于人工步骤，且不构成对渲染质量的断言。

> 截图与观感只证明"**这个程序确实渲染出了这些页、且数字来自真实链路**"，
> **不构成任何硬件指标**（数据源本身是进程内模拟器，页面上方也标着"★ 模拟数据"）。

### 4.5 端到端演示程序实测输出（摘要，`mc_loopback_demo`）

```
① 健康链路      发送 4 帧 | ACK 对上 4 | 遥测 120 发 → 119 收 / 0 拒 | 拒绝：全 0 | 超时 0 | 末端状态 Running
② 上报链路损坏  发送 8 帧 | ACK 对上 0 | 遥测 120 发 → 0 收 / 0 拒 | 重同步丢字节 3768 | 超时 8（Fault 4 / 可重传 4）
③ 下行全丢      发送 8 帧 | ACK 对上 0 | 遥测 40 发 → 39 收 / 0 拒 | 超时 6（Fault 2 / 可重传 4）| 末端状态 Ready
④ 上行延迟 10 拍  发送 4 帧 | ACK 对上 4 | 遥测 120 发 → 110 收 / 0 拒 | 超时 0
⑤ 延迟 80 拍·未开监控  发送 8 帧 | ACK 对上 2 | 超时 6（判 Fault 2 / 可重传 4）
⑥ 延迟 80 拍·已开监控  发送 8 帧（含心跳）| ACK 对上 6 | 超时 1（判 Fault 0 / 心跳漏拍 1）
                      实测 RTT(P95) 640 ms | 生效超时窗口 1920 ms

每个场景都多印了「末端状态 / 故障码」两栏 —— 阶段 4 起这两项由状态机裁决，
不再是按 `enabled_` 现算的投影。演示程序也不再手工发 HELLO：**握手由会话自己完成**。
```

**② ③ ⑤ 的"发送 8 帧"是增量 6 带来的可见变化，而它恰好是把 R5 与 ADR-003 的边界画在了线上**：
`SET_MODE` / `SET_PID` 是幂等命令，重发到用尽（首发 + 2 次），各贡献 3 帧；
`START` / `SET_TARGET` 是运动指令，**各只发过 1 帧**。所以 8 = 3 + 1 + 3 + 1。
② 里那几次同 `seq` 的重传被设备侧去重窗口变成了**重放应答**，没有重复执行 —— 这正是 §5.6 要的效果。

**⑤ 与 ⑥ 是同一组链路条件，唯一差别是有没有开链路监控** —— 这就是"按实测 RTT 配超时"的证据：
不开监控时 2 条运动指令被**误判**成超时（`Fault`），开了之后同样延迟下判 Fault 为 **0**。
⑥ 里实测 RTT 640 ms × 300% = 1920 ms，落在 [50, 2000] ms 之内，窗口被撑到 1920 ms。
⑥ 的"心跳漏拍 1"是**自举过程**的真实体现：窗口还没撑开之前，头一拍心跳先被判超时出队，
但它的应答仍然喂进了监控器（这正是窗口能收敛过来的原因）。

**场景②的实际拒绝路径是"重同步丢字节"，不是 CRC** ——
每第 2 个字节取反会打坏 SOF，解码器找不到帧头，于是逐字节重同步并丢弃（3768 字节）。
CRC 计数器因此为 0。这说明**"坏数据没进来"可以由多道关卡中任意一道保证**，
所以对应的用例刻意不断言"具体是哪一道关卡拒绝的"，只断言"没有任何坏数据被当成好数据"。

**③ 的末端状态是 `Ready` 而不是 `Fault`，这是对的行为**：
下行链路全丢，命令一帧都没到设备，所以设备停在握手后的 `Ready`，
从来没有进入过 `Running`。`Fault` 是**上位机侧** `AckTracker` 的判定（ADR-003），
两者是不同的东西，界面上必须分开显示 —— 把"上位机等不到应答"显示成"设备故障"会误导排查方向。

### 4.6 固件支线（3 个套，20 条）

| 测试套 | 条数 | 覆盖内容 |
|---|---|---|
| **`FirmwareFrameIo`** | **6** | CRC 与宿主实现逐位一致、固件编码的帧能被宿主 `mc::decodeFrame` 接受、拆包/粘包、CRC 错后重同步、超长载荷拒绝、版本不匹配整帧跳过 |
| **`FirmwareDevice`** | **13** | 握手/心跳、速度环运动与停车、参数回读、软限位、非法状态、通信丢失与故障确认、回零与"零点建立"位、回零超时、重复 SEQ 重放、未知类型 NACK、遥测周期、故障事件周期、**外部反馈取代虚拟被控对象** |
| **`FirmwarePty`** | **1** | **真 PTY 端到端**：`mc_fw_sim`（使用与 STM32 相同的固件核心）← `HELLO / SET_MODE / SET_TARGET / START` → 遥测进入 `Running` → `STOP` → `Ready` |

交叉编译（这是构建验证，不是测试）：

- `arm-none-eabi-g++ 10.3.1`、Cortex-M3、`-Os`
- `text 8623 B / data 0 B / bss 272 B`
- 入口 `0x08000425`（Reset_Handler，Thumb 位）、初始 SP `0x20005000`、`.isr_vector` 59 项

**上板实测（这是一次人工联调记录，不是自动化用例）**：

| 项 | 观测 |
|---|---|
| 烧录 | OpenOCD + ST-Link V2，`program … verify reset exit` 输出 **Verify OK** |
| 串口 | CH340 → WSL `/dev/ttyUSB1`，115200 8N1；上位机会话就绪，遥测持续计数 |
| 开环 | PA6 修正为复用推挽（0xB）后 TIM3 PWM 到达引脚，TB6612 驱动 N20 转动 |
| 编码器 | TIM4 正交解码 PB6/PB7，N20 的 7 PPR × 4 × 1:100 = 2800 计数/输出圈 |
| 速度闭环 | 外部反馈接入后，目标 200 °/s，2 s 时遥测 220.4 °/s，5 s 时 202.0 °/s；`STOP` 后回 `Ready` |
| 反向 / 位置环 | ⚠️ **待实测**：AIN2 从 GND 改接 PB0（双向驱动固件）之后才允许负目标；在此之前只验证过正向 |

- ⚠️ 上表只覆盖**空载**、**短时**联调；位置环、真实回零、软限位执法与 F7 拔线保护仍未在实物上验证。
- 编译链接通过、甚至烧录通过，都不等于电机行为已验证 —— 两者是不同层级的证据。

## 5. 判据有效性 —— 变异测试

"绿"本身不能证明判据有效。逐组把断言打红，改完立即还原。

### 5.1 增量 12（本轮）

本轮的新代码集中在**设备侧运动模型**、**设备侧一拍（SimLoop）**、**真 fd 传输与伪终端**、
**历史落库的批量**。逐条打红它们（每条的注入都用唯一标记 `MUT43-n` 自检，且**先确认构建成功**再跑测试）：

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M43-1** | 速度估计改用**未量化的位置**（等于让设备偷看"上帝视角"真值） | **1 条** | `SimulatorModel.ReportedVelocitySitsInsideTheQuantizationFloorOfTheTrueVelocity` |
| **M43-2** | 去掉"不驱动分支"里对控制器状态的清理（陈旧积分会让停机后再使能窜一下） | **1 条** | `SimulatorModel.ReEnablingAfterAStopDoesNotLurchFromStaleControllerState` |
| **M43-3** | 上行损坏**只计数、不改字节**（计数器照涨，"坏帧"一个都没有） | **2 条** | `SimLoop.CorruptsEveryNthByteOfTheWholeUplinkStream`、`mc_sim.SelfTest` |
| **M43-4** | 传输的读错误不记账（"对面还没接上"与"链路坏了"从此分不开） | **1 条** | `FdTransportAndPty.ReadingTheMasterBeforeTheSlaveIsOpenIsNotAnError` |
| **M43-5** | 落库退化成「每个样本一个事务」（每 10 ms 让 SQLite 刷一次盘） | **2 条** | `HistoryStore.WritesSamplesInBatchesNotOneTransactionPerSample`、`UiSmokeTest.TheHistoryPageReadsBackWhatWasRecordedWhileConnected` |
| **M43-6** | `CLEAR_FAULT` 门禁放宽成**处处放行** | **3 条** | `DeviceState.CommandGateTableMatchesSpec`、`DeviceState.ControlCommandsAreGatedOnReadyOrRunningOnly`、`EndToEnd.ClearFaultOnlyWorksInFaultAndOnlyForTheCodeTheDeviceIsReporting` |
| **M43-7** | 伪终端**不设 raw**（行规改写二进制载荷里的 0x0A/0x0D） | **3 条** | `FdTransportAndPty.BinaryPayloadSurvivesTheLineDiscipline`、`FdTransportAndPty.ADeviceBehindTheMasterAnswersAFrameFromTheSlave`、`mc_sim.SelfTest` |

七条全部**构建成功**；基线 `205 / 205`；全部还原后复跑回到 `205 / 205`，
并且**与现场快照逐字节比对一致**（排除 `build/`），残留标记检查为空。变异在 WSL ext4 的副本上进行，
工作区与那台正在跑浸泡的构建目录都没有被碰过。

**两条值得单独看的结果**：

- **M43-1 是本轮最有价值的一条：它第一次跑的时候"没有任何用例变红"**。
  原来的断言是 `EXPECT_NE(reported, trueVelocity)`（"上报值不该等于真值"）——
  稳态下这两个数值几乎相等，一个**直接拿真值上报**的实现照样混得过去。
  改成结构性判据后才抓住：上报速度必须落在「整数个计数 × 每计数速度」的格点上。
  这条记在 §6 的 C41，因为它暴露的是**断言写得太弱**，而不是代码有错。
- **M43-7 是"注释里写了理由"与"用例真的钉住"之间的差别**。
  `openPtyPair()` 里那句 `cfmakeraw` 原本只有注释解释为什么必要；
  加了一条"二进制载荷必须原样穿过行规"的用例之后，去掉它立刻三处变红。
  PTY 上跑的是二进制协议，而 0x0A/0x0D 在 CRC 里几乎必然出现 ——
  这类"看起来只是移植细节"的地方最需要用例兜住。

**另外一条不在上表里的反向验证**：逐页看截图时发现的 C42（历史页统计停在"刚连上"那一刻）
修好之后，是把**修复本身**再拆掉一次来确认断言有效的 —— 去掉 `showEvent` 里的刷新，
`UiSmokeTest.TheHistoryPageReadsBackWhatWasRecordedWhileConnected` 立刻变红并打出
`共 0 条样本 / 已落盘 0 条 / 0 个事务`。**"改好了"与"用例真的会因为这个改动变红/变绿"是两件事**，
前者肉眼可看，后者必须有反向证据。

### 5.2 增量 8

本轮新增的 5 条用例集中在**设备侧产帧速率**与**计时诊断开关**。逐条打红它们：

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M31** | 补发改回「每拍最多一帧」（= **本轮的修改之前**的行为） | **4 条** | `EndToEnd.DeviceEmitsTelemetryOnItsOwnPeriodNotTheHostTickRate`、`EndToEnd.DeviceCapsPerTickTelemetryAndCountsWhatItCouldNotHandOver`、`EndToEnd.DenseTelemetryReallyArrivesAtTheHostOverTheLink`、`EndToEnd.MillisecondQuantizedClockDithersTheFrameCountWithoutLosingAny` |
| **M32** | 去掉单拍产帧上限（该发多少全发） | **1 条** | `EndToEnd.DeviceCapsPerTickTelemetryAndCountsWhatItCouldNotHandOver` |
| **M33** | 去掉 `period == 0U` 的显式特例 | **用例当场崩溃：浮点异常（整数除零），`exit=136`** | `EndToEnd.ZeroTelemetryPeriodMeansOneFramePerTickAndDoesNotSpin` |
| **M34** | 产帧基准不再「按整数个周期」前进（多走一个周期） | **4 条** | 同 M31 那四条 |

四组变异**都构建成功**；基线 172/172；全部还原后复跑回到 **172/172**。
还原校验用的是**逐字节 sha256**（`common/src/device_simulator.cpp` = `0460a3c3…`，还原后一致），
不是"看起来改回去了"；工作树里除本轮应有的改动外**无残留**。

**三条值得单独看的结果**：

- **M31 最有说服力**：它把实现退回到本轮的修改**之前**（"每拍最多一帧"），
  一次打红四条 —— 说明这 5 条新用例测的正是"补发"这件事本身，
  而不是那种"改不改都绿"的装饰性断言。
- **M33 说明那个 `period == 0` 的分支不是防御性冗余，是防除零**。
  去掉它，`(nowMs - lastTelemetryMs_) / period` 立刻整数除零，用例**崩溃**而不是失败
  （`exit=136` = 128 + SIGFPE）。这类"看起来多余、其实是硬约束"的守卫靠读代码很难判断，
  变异一眼可见。
- **M34 钉住的是"基准按整数个周期前进"这条性质**：改成多走一个周期之后，
  每拍应产 10 帧变成 9 帧，长期帧率就偏了（用例期望的 90 变成 82）。
  这条性质不写用例就完全靠不住 —— 它不影响任何单拍的正确性，只影响长期速率的准确性。

### 5.3 增量 7

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M26** | 界面不再节流（每拍都发 `statsRefreshed`） | **1 条** | `host_selftest`（判据"刷新次数不超过 tick 数的五分之一"） |
| **M27** | 断开链路时不去结清在途命令（`close()` 里把 `TimedOut` 换成别的终态） | **1 条** | `DeviceManagerTest.DisconnectSettlesAnInFlightCommandAsTimedOut` |
| **M28** | 抽稀不再保留桶内极大（`indexMax` 恒等于桶首） | **1 条** | `CurveModel.DecimationKeepsTheGlobalExtremes` |
| **M29** | `extractSeries` 去掉"反向窗口"的早退 | **0 条（非杀手，已查明原因）** | 见下 |
| **M30** | 曲线页把单帧点数预算设成 0（等于不画） | **1 条** | `UiSmokeTest.TheCurvePageDrawsAndReportsItsCounters` |

五组变异**都构建成功**；基线 166/166，全部还原后复跑回到 166/166，
且与暂存副本 `diff -r`（排除 `build/`）**无差异**、无残留标记。

**M29 为什么没红 —— 这是一次有价值的"变异没打中"。**
去掉 `if (ring.empty() || window.fromMs > window.toMs)` 里的后半段之后，反向窗口**仍然**返回空。
原因是它可被证明**等价**：二分查找的锚点是"第一个 `tMs >= fromMs` 的点"，而 `fromMs > toMs`，
所以第一个候选点必然 `> toMs`，循环立刻 `break`。也就是说这个早退是**冗余守卫**
（把前置条件写在函数开头，表达意图用），并不承载行为。
本轮**保留**它，但把结论记下来：**"看起来在把关、其实不改变结果"的代码靠变异是杀不出来的** ——
所以 §5.2 之外，本轮真正有价值的是 C24 / C25 那两条**用例写错方向**的发现。

**另外两次"没打中"另有原因，也一并记下（C27、C28）**：
① M28 第一版把尖峰放在索引 500 上，而 500 恰好是分桶边界，"`indexMax` 恒取桶首"的退化实现
也照样留住了它；**把尖峰移到桶内部（505 / 506）之后**变异才红 ——
用例的数据点必须避开"退化实现也能侥幸通过"的位置。
② M29 / M30 的第一轮**构建失败**（sed 替换把源码改成了语法错），随后跑的其实是**旧二进制**；
并且还原命令里的 `$R` 变量被外层 shell 吃掉，**还原从未生效**，工作树被污染。
这一轮证据作废，改用字面路径 + "变异是否真的注入"的自检重做（与 C6、C11 同类）。

### 5.4 增量 6

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M20** | 握手退避失效（一到「未就绪」就立刻重开一轮） | **2 条** | `EndToEnd.FailedHandshakeRoundsBackOffAndReopenUntilTheLinkComesBack`、`EndToEnd.ARetryableRequestIsActuallyRetransmittedOnTheWire` |
| **M21** | 一轮 `HELLO` 重试用尽后**不再重开**（下次可尝试时刻推到无穷远） | **2 条** | `EndToEnd.FailedHandshakeRoundsBackOffAndReopen…`、`EndToEnd.LinkFailureTearsDownTheSessionAndReconnectNeverResumesMotion` |
| **M22** | 链路失效仍报告「会话没丢」（假装还就绪） | **1 条** | `EndToEnd.LinkFailureTearsDownTheSessionAndReconnectNeverResumesMotion` |
| **M23** | 把运动指令也宣告成「可重传」（直接违反 ADR-003） | **7 条** | `AckTracker.MotionCommandsFaultOnTimeoutAndAreNeverRetried`、`AckTracker.MultipleRequestsAreTrackedIndependently`、`AckTracker.HeartbeatTimeoutIsDroppedNotFaulted`、`ProtocolPolicy.MotionCommandsNeedAckButAreNeverAutoRetried`、`EndToEnd.DroppedLinkMakesMotionCommandFaultInsteadOfRetrying`、`EndToEnd.AckLossNeverResendsAMotionCommandAndSettlesItAsATimeout`、`EndToEnd.LinkFailureTearsDown…` |
| **M24** | 会话作废时把在途请求**静默丢弃**（`tracker_.reset()`） | **2 条** | `EndToEnd.AHeartbeatTimeoutIsNotReportedAsAFault`、`EndToEnd.LinkFailureTearsDownTheSessionAndReconnectNeverResumesMotion` |
| **M25** | 判了 `Retry` 却**不真发**（退回本次修掉的那个缺陷） | **1 条** | `EndToEnd.ARetryableRequestIsActuallyRetransmittedOnTheWire` |

六组变异**都构建成功**；基线 134/134，全部还原后复跑回到 134/134，
且与现场快照**逐字节一致**（排除 `build/`，那是构建产物不是源码）、**无残留标记**。

**三条值得单独看的结果**：

- **M25 是"本次修掉的那个缺陷"的反向证据。** 它把 `pump()` 里那句真正重发的 `sendFrame` 去掉，
  只打红一条 —— 而那条用例正是为它写的（数**线上的帧数**：首发 1 + 重发 2 = 3）。
  没有这条用例，"只判不发"这个缺陷会一直绿着。
- **M23 一次打红 7 条，横跨单元、协议策略、端到端三个层次**：这说明"运动指令绝不重传"
  不是靠某一条断言撑着的，而是**多道独立的闸门**叠起来的 ——
  `isRetryable` 不认它、`AckTracker` 不为它产出 `Retry`、重发路径的"发件留底"里根本没有它。
- **M24 的第一轮设计不成立（判据的判据也得验）。** 最初把在途请求放在断链**之后**才发出去，
  结果它在链路判失效**之前**就已经超时结清了，`reset()` 抹不到它 ——
  M24 于是只打红了一条**口径不对**的用例（心跳计数），而目标用例照旧全绿。
  改成"等超时窗口按实测 RTT 收敛到 900 ms 之后再发命令，让断链判定（≈150 ms）明显快过它"，
  M24 才打在目标用例上。教训见 §6 的 C21。

### 5.5 增量 5

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M15** | `enterFault()` 里不再置位故障锁存 | **5 条** | `DeviceState.AllOneHundredFortyCombinationsMatchTheSpecTable`、`FaultCanOnlyBeLeftByRecovery`、`FaultSurvivesLinkLossAndReconnectUntilConfirmed`、`HandshakeTimeoutEntersFaultAndLatches`、`EndToEnd.CommLossWhileRunningStopsTheMotorAndEntersFault` |
| **M16** | 设备对 `HELLO` 不再绕过去重窗口 | **1 条** | `EndToEnd.HelloClearsTheDedupWindowSoARestartedHostIsNotSilentlyIgnored` |
| **M17** | `SET_MODE`/`START` 的门禁从「仅 `Ready`」放宽到「非 `Disconnected`」 | **3 条** | `DeviceState.CommandGateTableMatchesSpec`、`ControlCommandsAreGatedOnReadyOrRunningOnly`、`EndToEnd.SetModeWhileRunningIsRejectedWithIllegalState` |
| **M18** | `STOP` 不再经过 `Stopping` | **3 条** | `DeviceState.AllOneHundredFortyCombinationsMatchTheSpecTable`、`StopPassesThroughStoppingBeforeReady`、`StopIsAcceptedFromEveryStateAndIdempotentOutsideRunning` |
| **M19** | F7 保护窗口乘 1000（等价于关掉） | **1 条** | `EndToEnd.CommLossWhileRunningStopsTheMotorAndEntersFault` |

五组变异**都构建成功**；基线 129/129，全部还原后复跑回到 129/129，
且与现场快照**逐字节一致**（排除 `build/`，那是构建产物不是源码）、**无残留标记**。

**两条值得单独看的结果**：

- **M19 只打红正面用例，负对照 `WithoutTheCommLossWindowTheDeviceKeepsRunningOnAStaleTarget`
  仍然是绿的。** 这正是要的形状：负对照证明"把窗口关掉 = 没有通信丢失保护时的行为"，
  正面用例证明"窗口开着就真的会停车"。两者互相独立，缺任何一个都说明不了问题。
- **M15 打红 5 条，横跨单元（3 条）、穷举表（1 条）、端到端（1 条）三个层次。**
  故障锁存这条规则不是靠某一条断言撑着的。

### 5.6 增量 4

| 组 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M11** | 设备不再记住处理过的 `seq`（去重窗口永远为空） | 1 条变红 | `EndToEnd.DuplicateSequenceReplaysTheReplyWithoutReExecuting` |
| **M12** | `HELLO` 不再清空去重窗口 | 1 条变红 | `EndToEnd.HelloClearsTheDedupWindowSoARestartedHostIsNotSilentlyIgnored` |
| **M13** | 心跳超时不再单独处置（退回判 `Fault`） | 3 条变红 | `AckTracker.HeartbeatTimeoutIsDroppedNotFaulted`、`EndToEnd.AHeartbeatTimeoutIsNotReportedAsAFault`、`EndToEnd.MeasuredRttWidensTheTimeoutSoHighLatencyDoesNotFaultAMotionCommand` |
| **M14** | 推荐窗口恒为初值（按 RTT 自适应失效） | 5 条变红 | `EndToEnd.MeasuredRttWidens…` + 4 条 `LinkMonitor` 单测 |

四组变异**都构建成功**；基线 110/110，全部还原后复跑回到 110/110，且与暂存副本**逐字节一致**。

> **M13 第一轮构建失败，那一轮证据作废。** 第一版写成让 `isPeriodicProbe` 直接 `return false`，
> 于是形参 `type` 变成"设而未用"，`-Wextra -Werror` 让构建挂掉 —— 随后的 `ctest` 跑的是
> **上一次的二进制**。这正是 C3 那条纪律要防的事。重做时改成在**消费侧**断开
> （`if (isPeriodicProbe(...))` → `if (false)`），并给 harness 加了
> **"确认变异真的注入了"的自检**（`grep -q` 验证源码确实被改），
> 免得"`sed` 没匹配上"被误读成"测试抓不到"。

### 5.7 增量 3

| # | 变异 | 变红数 | 命中的用例 |
|---|---|---:|---|
| M6 | `cancelPending()` 变空操作（收到 NAK 后仍继续等应答） | 1 | `EndToEnd.TargetBeforeStartIsRejectedWithIllegalState` |
| M7 | 上位机不再做遥测语义校验（放行 NaN） | 1 | `EndToEnd.ForgedNanTelemetryIsRejectedAndDoesNotReplaceTheLastGoodSample` |
| M8 | 链路不再执行丢字节注入 | 2 | `EndToEnd.DroppedLinkMakesMotionCommandFaultInsteadOfRetrying`、`LoopbackTransportTest.DropInjectionLosesExactBytesAndCountsThem` |
| M9 | 设备不再校验"必须先使能" | 2 | `EndToEnd.TargetBeforeStartIsRejectedWithIllegalState`、`EndToEnd.DeviceNackEchoesRejectedSeqInBothHeaderAndPayload` |
| M10 | 链路不再执行延迟注入 | 1 | `LoopbackTransportTest.LatencyDelaysVisibilityByGivenPolls` |
| — | 全部还原后复跑 | 0 | 回到 `100% tests passed, 0 tests failed out of 90` |

**M8 是本轮最重要的一条**：它证明 `DroppedLinkMakesMotionCommandFaultInsteadOfRetrying`
不是"因为链路本来就是坏的所以报错"，而是**真的依赖丢包发生**——
把丢包注入关掉，这条用例立刻变红。这是 ADR-003 在**真实链路上**的可执行证据。

### 5.8 增量 2

| # | 变异 | 变红数 | 命中 |
|---|---|---:|---|
| M1 | `isRetryable()` 对一切返回 true | 6 | `AckTracker.MotionCommandsFaultOnTimeoutAndAreNeverRetried` 等 |
| M2b | 应答类型配对检查取反 | 5 | `AckTracker.MismatchedAckTypeDoesNotCompleteTheRequest` 等 |
| M3 | `isTelemetryValid()` 恒为 true | 3 | `Payload.NonFiniteTelemetryDecodesButFailsValidation` 等 |
| M4 | 超时判定 `<` 改 `<=` | 6 | `AckTracker.TimeoutFiresExactlyAtDeadline` 等 |
| M5 | 载荷长度校验 `!=` 改 `<` | 1 | `Payload.DecodeRejectsEveryWrongLength` |

### 5.9 增量 1

| # | 变异 | 变红数 | 命中 |
|---|---|---:|---|
| A | CRC 多项式 `0x1021` → `0x8005` | 1 | `Crc16.StandardCheckVector` |
| B2 | 关闭 CRC 校验 | 2 | `FrameCodec.DecodeRejectsCorruptedCrc` 等 |
| B3 | 关闭载荷长度上限检查 | 1 | `StreamDecoderTest.OversizeLengthIsRejectedWithoutBuffering` |
| C | 环形缓冲覆盖时不累加丢弃计数 | 3 | `RingBuffer.OverflowOverwritesOldestAndCountsExactDiscards` 等 |
| C2 | 关闭 SOF 识别 | 3 | 重同步类 3 条 |

**变异 A 的结论至今有效**：把 CRC 多项式改错后，39 条里**只有 1 条**变红 ——
编解码两端共用同一多项式时往返自洽，错误的 CRC 在自己的实现内部完全隐形。
唯一锚点是那条硬编码的公开标准向量 `0x29B1`。

### 5.10 增量 13–18（本轮：回零与软限位）

本轮新增/变更的用例集中在**回零流程**与**软限位**，并且在核心层、宿主层、界面层三处各有一条判据。
逐条把实现弄坏，确认**每一处都有断言真的会因此变红**（11 组，全部构建成功，基线 `352 / 352`，
每组改完立即还原，全部还原后复跑回到 `352 / 352`）：

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M44-1** | 门禁里 `HOME` 也在 `Running` 放行 | **2 条** | `DeviceState.CommandGateTableMatchesSpec`、`DeviceState.ControlCommandsAreGatedOnReadyOrRunningOnly` |
| **M44-2** | 去掉「`HOME` 步后面必须紧跟 `WaitReady`」这条载入期规则 | **2 条** | `SequenceHome.AHomeStepMustBeFollowedByWaitReady`、`DeviceManagerTest.AProgramWithAHomeStepMustCarryASettleStep` |
| **M44-3** | 遥测编码不再写 `statusFlags` 那个字节（恒 0） | **4 条** | `Payload.UnknownStatusFlagBitsSurviveTheRoundTrip`、`Payload.TelemetryByteLayoutIsPinned`、`Payload.TelemetryRoundTrip`、`SimulatorModel.TheHomingAttainedBitTravelsInEveryTelemetryFrame` |
| **M44-4** | 回零开始时不清「本轮回零已建立零点」这一位 | **1 条** | `SimulatorModel.HomingAttainedBitIsClearedOnStartAndSetOnlyOnCompletion` |
| **M44-5** | 设备收到 `SET_LIMITS` 后**不存**（`softLimits_ = payload` 去掉） | **3 条** | `SimulatorModel.SetLimitsCommandIsStoredAndComesBackInParamState`、`DeviceManagerTest.SoftLimitsAreAcceptedStoredAndReadBackFromTheDevice`、`UiSmokeTest.TheParamPageDownloadsSoftLimitsAndReadsThemBack` |
| **M44-6** | 软限位判定恒通过（`softLimitAllows` 直接 `return true`） | **3 条** | `SimulatorModel.LimitsThatWouldLockOutTheCurrentPositionAreRefused`、`SimulatorModel.SoftLimitsAreNotEnforcedWhileHoming`、`SimulatorModel.TheAxisIsStoppedAndFaultedWhenItRunsPastTheSoftLimit` |
| **M44-7** | 设备不再拒「把当前位置关在门外」的限位 | **1 条** | `SimulatorModel.LimitsThatWouldLockOutTheCurrentPositionAreRefused` |
| **M44-8** | 宿主改用「停在 `Ready`」判回零成功（不看设备报的那一位） | **1 条** | `DeviceManagerTest.AHomeStoppedByTheOperatorIsReportedAsAbortedNotSucceeded` |
| **M44-9** | 同 M44-8，看**界面**上的表现 | **1 条** | `UiSmokeTest.TheMotionPageReportsAStoppedHomeAsNotEstablished` |
| **M44-10** | 去掉 `HOME` 步的 `WaitReady` 规则（宿主层复算一遍，证明宿主链接的是同一份核心） | **1 条** | `DeviceManagerTest.AProgramWithAHomeStepMustCarryASettleStep` |
| **M44-11** | 设备不存软限位（界面回读会显示默认值） | **1 条** | `UiSmokeTest.TheParamPageDownloadsSoftLimitsAndReadsThemBack` |

**M44-8 / M44-9 是本轮最要紧的一条**：它就是当初那个**真实缺陷**（被 STOP 打断的回零被报成"成功"）
的反向证据。回零是低速运动，"被打断后回到 `Ready`"与"走到位后回到 `Ready`"在**状态上分不出来**，
所以判据必须落在**设备持续上报的那一位**上；把这条换成看状态，宿主层与界面层同时变红。

**本轮有两组变异第一次「打不红」，追下去都是断言太弱（不是代码有错）** —— 记在 §6 的 C43：
M44-3 与 M44-5 起初只用 `SimulatorModel.*` 过滤器跑，结果全绿，
因为那两条断言**钉的是设备内部结构体与构造配置**，而不是**线上的字节**与**报文这条路径**。
把 `TheHomingAttainedBitTravelsInEveryTelemetryFrame` 改成"把设备真正发出的帧解回来逐字节确认该位"、
并新增 `SetLimitsCommandIsStoredAndComesBackInParamState`（发 `SET_LIMITS` 后走一遍 `READ_PARAMS` 回读）之后，
同样的变异立刻变红（上表 M44-3 的 4 条、M44-5 的 3 条即此）。

### 5.11 增量 19（本轮：轴组同步与运行日志）

本轮新增的用例集中在**轴组同步移动**与**运行日志**，在核心层、宿主层、界面层三处各有一条判据。
逐条把实现弄坏，确认**每一处都有断言真的会因此变红**（11 组，全部构建成功，基线 `389 / 389`，
每组改完立即还原，全部还原后复跑回到 `389 / 389`）：

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **M45-1** | 轴组不按行程缩放（每轴都跑满上限，等于"先到先停"） | **3 条** | `AxisGroup.EveryAxisTakesTheSameTimeSoTheyArriveTogether`、`AxisGroup.AStationaryAxisGetsZeroSpeedAndDoesNotShortenTheDuration`、`AxisGroup.ScalesAcrossMoreThanTwoAxes` |
| **M45-2** | 有界环形满了**丢最新的**（而不是最旧的） | **2 条** | `LogRing.DropsTheOldestAndCountsWhatItDropped`、`LogRing.ClearingKeepsTheDroppedAccount` |
| **M45-3** | 超过上限**不轮转** | **4 条** | `RotatingFile.WritesLinesThenRotatesAndKeepsTheConfiguredCount`、`RotatingFile.KeepingASingleFileTruncatesInsteadOfRotating` 等 |
| **M45-4** | 级别过滤失效（挡在入口这一步去掉，全都记） | **2 条** | `Logger.FiltersBelowTheMinimumLevelAndCountsBothSides`、`Logger.TheFileOnlySeesWhatSurvivedTheLevelFilter` |
| **M45-5** | 取消「任轴故障就 STOP 全组」这道安全闸 | **1 条** | `AxisGroupHost.AnyAxisFaultStopsTheWholeGroup` |
| **M45-6** | 把「切模式」与「启动」塞进同一拍（= 本轮修掉的那个缺陷） | **1 条** | `AxisGroupHost.ASynchronizedMoveDispatchesToBothAxesAndCompletes` |
| **M45-7** | 宿主的日志埋点 (`logEvent`) 变成空操作 | **1 条** | `DeviceLoggingHost.ConnectsAndCommandOutcomesLandInTheLog` |
| **M45-8** | 双轴页的节拍刷新改回"只刷表不刷按钮/标签"（= 本轮修掉的界面缺陷） | **1 条** | `UiSmokeTest.TheAxisPageConnectsBothAxesAndCompletesASynchronizedMove` |
| **M45-9** | 日志页改级别时**不下发给日志器** | **1 条** | `UiSmokeTest.TheLogPageFiltersByLevelAndReportsItsFile` |
| **M45-10** | 拆掉「全组停止」按钮的连线（= 假按钮） | **1 条** | `UiSmokeTest.TheAxisPageConnectsBothAxesAndCompletesASynchronizedMove`（2 处断言同时红） |
| **M45-11** | 拆掉「清空视图」按钮的连线（= 假按钮） | **1 条** | `UiSmokeTest.TheLogPageFiltersByLevelAndReportsItsFile` |

**M45-5 与 M45-6 是本轮最要紧的两条。** 前者证明"全组安全闸"不是写在注释里的愿望 ——
把那一支去掉，健康那一轴就再也不会收到 `STOP`；后者证明"启动是跨拍的"这件事**真的被用例钉住**，
而不是靠"设备恰好一拍就报上新模式"侥幸通过。

**M45-10 / M45-11 是收尾自查时补上的两条 —— 它们盯着的是"假按钮"。**
全量扫了一遍新页面上的控件，发现两个按钮**没有任何用例点到**：「全组停止」与「清空视图」。
两者的**底层逻辑**都有覆盖（`stopAll` 由宿主安全闸用例覆盖、`ring().clear()` 由核心用例覆盖），
但"按钮接没接上"这一层没有判据 —— 连线漏了照样全绿，而界面上那个按钮就是死的。
补上断言后再把连线拆掉，两条立刻变红。**这正是"宁可少一个页面，不可有一个假按钮"的可执行版本。**

**一次"打不红"值得单独记（与 C41 / C43 同类，这次是设计上的等价）。**
M45-6 的第一版想把 `allAxesInMode` 那道等待守卫改成 `if (false)`，结果**全绿**。
追下去不是断言弱，而是**仿真设备让这件事等价**：`SimulatedDevice::runMode()` 直接读模拟器自己的状态，
而模拟器在**同一拍**里就处理了 `SET_MODE` —— 于是"隔一拍再发启动"与"等模式报上来再发启动"
在仿真下没有可观察的差别。**真串口上不等价**：`SerialDevice::runMode()` 只报**被确认过**的设定模式
（`hasAckedMode_`，见 `host/src/device.cpp:808`），那需要一次往返，所以那道等待在真硬件上是必需的。
因此把变异改成"同一拍里连着发 `SET_MODE` 和 `START`"（就是本轮的原始缺陷），立刻变红；
并把这条等价性**如实写进 §7 的边界表**，不假装它被仿真覆盖了。

**另一次"打不红"是纯断言写错，记在 §6 的 C49。**
M45-10 第一次跑**全绿**，因为「全组停止」那条断言的谓词选了 `"已发出全组停止"` ——
而**"按计划走完"那一刻的文案里也有这五个字**（`按计划走完（…），已发出全组停止`），
于是按下按钮之前断言就已经成立。改成只有人工停止那条路才会说的话（`行程收不回来`）
并把阶段文案也钉住之后，拆掉连线立刻变红。**这与 C24（`contains("就绪")` 被"未就绪"命中）
是同一类错，同一个地方的第二次出现。**

### 5.12 增量 20（固件支线）

固件侧的 5 组变异，**每组都构建成功**（避免"构建失败后跑旧二进制"的假证据），
对应断言全部变红；每组立即还原，全部还原后复跑 **19 / 19**：

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **MF1** | CRC 校验失效（坏帧也当合法帧收下） | **1 条** | `FirmwareFrameIo.DropsACorruptedFrameAndResynchronizes` |
| **MF2** | 去重窗口失效（重传被重新执行） | **1 条** | `FirmwareDevice.DuplicateSequenceReplaysTheAckWithoutReapplying` |
| **MF3** | 通信丢失保护失效（静默超窗不报故障） | **1 条** | `FirmwareDevice.CommLossFaultsAndClearFaultNeedsTheMatchingCode` |
| **MF4** | 回零"本轮回零已建立零点"位不置位 | **1 条** | `FirmwareDevice.HomeReturnsToTheOriginAndSetsTheAttainedBit` |
| **MF5** | 故障事件不再周期重发 | **1 条** | `FirmwareDevice.FaultEventIsPeriodicWhileFaulted` |

**这 5 条钉住的是固件里最容易"看起来对"的五处**：CRC 丢没丢、重传会不会重复执行、
失联停不停、回零成没成、故障通知还在不在。它们全部是**宿主端**判据；
实物上的时钟、引脚、电平与电机行为仍未验证。

### 5.13 增量 21（外部反馈）

新增的 `ExternalFeedbackReplacesTheVirtualPlant` 只钉一件事：
**开启 `externalFeedback` 且喂进外部位置时，设备必须用它，而不是继续跑虚拟被控对象。**
变异在 `/tmp` 的整树副本上进行，工作区未被改动：

| 编号 | 变异（把什么弄坏） | 结果 | 变红的用例 |
|---|---|---|---|
| **MF6** | 外部反馈分支恒为假（`const bool external = false;`），设备继续积分虚拟模型 | **1 条** | `FirmwareDevice.ExternalFeedbackReplacesTheVirtualPlant` |

变红位置：`firmware/tests/test_device.cpp:399-400` ——
位置读回 `0`（期望 `50`）、速度读回 `0`（期望 `1000`）。
这说明该用例测的确实是"外部反馈取代虚拟被控对象"这条路径；
若只断言"位置不为零"，一个恰好被虚拟模型推动的实现也会混过去。

**未覆盖**：这条用例不验证编码器引脚、计数方向或 2800 计数/圈的换算 ——
那些属于硬件端口，宿主测试里没有对应外设。

## 6. 本轮发现并纠正的问题（含对我自己前几版报告的更正）

| # | 问题 | 处理 |
|---|---|---|
| **C1** | **增量 1 的"退出码 0"不可靠**。`cmd > log 2>&1; rc=$?` 采集时 `$rc` 被外层 Git Bash 吃掉（实测打印空串），当时显示 `0` 是**外层 shell 的 `$?`** 恰好为 0，并非测到内层命令的退出码 | 改用不依赖 `$?` 的 `if cmd; then … else … fi`，并**自检 harness 能识别失败**。增量 1 的通过结论仍成立（有两项独立证据），但"退出码"那一栏作废 |
| **C2** | `cmake --build … \| tail` 的 `$?` 是 `tail` 的 —— 构建失败也报 `build_exit=0` | 先重定向到文件，单独判构建成功与否，再显示输出 |
| **C3** | 变异 M2 首轮**构建失败**（`-Werror` 把"变量设而未用"当错误），随后的 `ctest` 跑的是**上一次的二进制** | 该轮证据作废，改用不改变量使用状态的 M2b 重做 |
| **C4** | `AckTracker(Config config = {})` 的默认实参 GCC 不接受 | 拆成 `AckTracker()` 与 `explicit AckTracker(Config)` 两个构造函数 |
| **C5** | `CorruptedLink…` 用例初版断言"必须观察到 CRC 错误"—— 实测 CRC 计数为 **0**。真相是：每第 2 字节的损坏打坏了 **SOF**，走的是重同步丢弃，根本到不了 CRC | 改为不断言"哪一道关卡"，只断言"坏数据未进入数据通路"+ 三条正对照（设备确实在发、链路确实在坏、拒绝活动确实 > 0） |
| **C6** | 变异 harness 的 `restore()` 里用了 `$SRC` 变量，**被外层 shell 吃掉**导致还原从未生效，M6–M10 的变红数实际是**累积值**而非各自归属 | 该轮归因作废；改用字面路径重跑，并保留"全部还原后复跑必须回到 90/90"这条自检 —— **正是它抓到了这个坏 harness** |
| **C7** | `HealthyLinkDeliversEveryByte…` 初版在推进 tick 之后断言字节守恒，漏算了"每拍末尾设备必发一帧"造成的在途数据（差 30 字节 = 恰好一帧） | 改成"只读不产地排空"（`drainHost()`）再断言；并用 sha256 逐文件 + 递归 diff 确认 WSL 副本与暂存源码**逐字节一致** |
| **C8** | 发现并修掉一处**协议自身的不一致**：`FaultEvent` 被标为"需要 ACK"，但 `tryAckOf` 里没有配对的 ACK 类型（与增量 2 的心跳问题同类） | 把 `FaultEvent` 改为**不需要应答的周期性通知** —— 故障期间设备反复上报，丢一帧下一拍就有，比引入 `FaultEventAck` 再维护一条等待队列更简单，也不要求"已经出事的设备"还必须响应 |

| **C9** | 集成时发现一个**会让命令静默失效的真 bug**：`LinkMonitor` 拿自己的 seq 计数器从 0 开始发心跳，与命令的 seq 计数器**撞车**。`AckTracker` 只按 `seq` 判重、不看类型，于是 `canRegister` 前置检查会把命令拦下来 —— 表现为 `sendXxx()` 返回 false、**命令根本没发出去**，而日志上只看到"没收到应答" | 定为规则："心跳的 `seq` 必须与命令共用同一个计数器"。`HostSession::enableLinkMonitor()` 改为要求显式传入序列号来源（`SeqSource`），传空则不启用；`PROTOCOL.md §5.2` 写明 |
| **C10** | 推演用例时发现一处**自举死锁**：心跳应答原本只在 `AckTracker` 匹配成功时才通知监控器。若初始窗口小于真实 RTT，心跳先被判超时出队 → 监控器永远拿不到 RTT 样本 → 推荐窗口永不收敛 → 窗口一直太小 | 心跳应答**无条件**交给监控器，由监控器自己的在途表校验 `seq`。迟到的心跳应答同样携带真实 RTT，而它正是把窗口撑开所必需的 |
| **C11** | 变异 M13 第一轮**构建失败**（`-Werror` 把"形参设而未用"当错误），随后的 `ctest` 跑的是旧二进制 | 该轮证据作废；改在消费侧断开并重做，同时给 harness 增加"**变异是否真的注入**"的自检 |
| **C12** | 演示里延迟按"接收端 poll 次数"注入，场景名却写成毫秒。**第一次跑 ⑤ 时根本没复现出误判**（判 Fault 0）—— 因为一次 `pump()` 会 poll 1–2 次，30 拍实际只有约 150 ms，仍在 200 ms 窗口之内 | 场景名改为按拍数命名并注明"拍数不等于毫秒"，真正的时间以 ⑥ 里**实测的 RTT** 为准；注入值改为 80 拍。⑤ 这才复现出判 Fault 2 |
| **C13** | `ClockRollbackDoesNotDeclareTheLinkDown` 初版构造写错：先 `tick(0)` 再 `tick(5)`，那是**时间前进**不是回退，于是"回退后应立刻补发一拍"这条断言失败 | 修测试构造（先 `tick(5000)` 再 `tick(5)`）。**实现没有问题，是测试写错了** —— 遇到失败必须先分清是哪一边错，不能默认改实现 |

| **C14** | **F7 的超时窗口定反了方向**。我把它写成"必须**远小于**心跳周期"（取 500 ms），理由是"不能等太久"。跑端到端立刻暴露：上位机心跳周期是 1000 ms，于是**心跳之间的空档被判成丢线** —— 健康链路上的设备自己进了 `Fault`（`TelemetryFlows…` 报 `velocity=0 / state=5`）。同一原因让 `MeasuredRttWidens…` 也红 | 改成 **3000 ms = 上位机允许连丢 3 拍**，与 `LinkMonitor.missedBeatsToDeclareLinkDown` 同口径；并加一条**跨模块**用例 `CommLossWindowMustOutlastTheHostHeartbeatPeriod`，直接拿 `LinkMonitor::Config` 的默认值当判据 —— 这个耦合关系必须由测试守住，不能只写在注释里 |
| **C15** | **`HELLO` 被自己的去重窗口挡住**（真 bug，不是测试问题）。上位机重启后仍从 `SEQ=0` 发 `HELLO`，而设备窗口里恰好还留着上一会话的 `(HELLO, 0)` → 这条 `HELLO` 被判成重传直接回放，`recent_.clear()` **永远不会执行** → 新会话所有命令与旧会话 seq 撞车被静默吞掉。这正是"握手看起来成功了但一条命令都不生效" | `HELLO` 改为**绕过去重窗口**且不记入窗口（无业务副作用、幂等，不需要去重）。`PROTOCOL.md §5.1` 写明这是"自指僵局" |
| **C16** | 测试台在构造时补了一次 `HELLO` 往返，于是**绝对计数器全部被污染**：`framesSent` 多 1、`decoder().framesDecoded` 多 2、`telemetryAccepted` 多 1；而且握手最后一拍还留下**一帧在途遥测**，会在场景第一拍被读走（`CorruptedLink…` 因此多出 1 条"接收"，`ForgedNan…` 因此多 1 条"接受"） | ① 测试台构造末尾**排空链路**；② 增加 `framesSent()` / `framesDecoded()` / `telemetryAccepted()` 三个**增量**函数，需要绝对值的用例一律用增量，不再拿总量跟场景数字硬比 |
| **C17** | `AllOneHundredFortyCombinationsMatchTheSpecTable` 初版失败，根因是**我写的规格表副本错了**（不是实现错）：`LinkOpened` 在非法时也返回了目标状态 `Initializing`，而"非法"的正确含义是**状态原地不动** | 修表：非法分支一律 `return {false, from, latch}`。**这类"规格副本 vs 实现"的对撞，两边都要怀疑** —— 这次是表错，所以改表而不是改实现 |
| **C18** | **修掉一个"判了却不做"的缺口**：`AckTracker` 会判出 `TxOutcome::Retry`，但**从来没有人执行"重发"**。后果很隐蔽：请求会一直挂在等待表里，而调用方因为 `canRegister` 已为 false，连自己调 `sendXxx()` 重发都会被前置检查拦下（直接返回 false）。这个缺口在增量 3–5 一直存在，**没暴露的原因是当时的用例只断言"运动指令不重传"（那一侧本来就是对的），没人断言"幂等命令真的重传出去了"** | `pump()` 在判出 `Retry` 的那一拍按**同 `seq`、同载荷**重发；只给 `isRetryable` 的类型留"发件留底"（运动指令天然不在其中）。新增用例 `ARetryableRequestIsActuallyRetransmittedOnTheWire` 用**线上帧数**钉住它，M25 反向验证这条判据有牙 |
| **C19** | **本报告自己的数字漂移了**：§4 的 ctest/gtest 数字与 §4.1 的套数/条数仍停在**增量 3 的 90 条**（正文却写着 129），增量 5 更新时漏改 —— 一份"用来证明可信"的报告里出现自相矛盾的数字，本身就是可信度问题 | 本轮按实测重填（134 / 11 套），并加纪律：更新时必须让 §1、§4、§4.1 三处的数字**互相自洽** |
| **C20** | 握手参数一开始写成 `HostSession` 的**嵌套类型 + 默认实参** `HandshakeConfig config = HandshakeConfig{}`，GCC 直接拒绝：*default member initializer … required before the end of its enclosing class* —— 嵌套类型的默认成员初始化器在**外层类定义完之前不可用** | 把 `HandshakeConfig` 移到 `HostSession` **之外**（命名空间作用域），并在头文件里写明"为什么不能像 `LinkMonitor::Config` 那样嵌套"，免得后来者又挪回去 |
| **C21** | **变异 M24 第一轮打在了错的用例上**：我在断链**之后**才把在途命令发出去，结果它在链路判失效**之前**就已经超时结清，`reset()` 根本抹不到它 —— M24 只打红了一条**口径不对**的用例（心跳计数），而目标用例照旧全绿。**"变异变红了"不等于"变异打在了该打的判据上"** | 改用例构造：先等超时窗口按实测 RTT 收敛到 900 ms（`minAckTimeoutMs`），再发命令，让断链判定（≈150 ms）明显快过它；M24 这才红在 `LinkFailureTearsDown…` 上 |
| **C22** | 演示程序里"③ 下行全丢：运动指令超时判 Fault，**且发送帧数不变**"这句话在增量 6 之后**变成错的了** —— 幂等命令现在真的会重发（8 帧 = 3+1+3+1），"发送帧数不变"只对运动指令成立 | 叙述跟着实测数字改写成"运动指令各只发 1 帧，多出来的 4 帧全是幂等命令的重发"，把 R5 与 ADR-003 的分界直接画在线上 |
| **C23** | **指纹口径原来不可跨平台复现。** 我在 WSL 里算出的聚合值，拿到 Windows 的 Git Bash 上用同一句命令重算**对不上**（`bd26904f…` vs `52a1831a…`）—— 一度像是"源码不一致"。查下来是 `sha256sum` 的**标记字节随平台变**：Linux 两个空格，MSYS/Git Bash 是 `*`；而**逐文件哈希两边完全一致** | 口径加上 `--text`（两个平台都强制文本模式），重算两边都得到 `bd26904f0ccdb6454315097826fbb7f4ec68e9c32af03eec72a2fd912e8e5245`，并且**逐文件哈希与暂存副本逐字节相同**。这条同时证明了"WSL 里测的确实是 Windows 侧这份源码" |
| **C24** | **界面用例的等待谓词写反了方向。** 我写 `waitUntil(会话标签.contains("就绪"))` —— 而"**未**就绪（正在握手 / 等待链路恢复）"这句话里**也含"就绪"两个字**，于是刚点下连接按钮谓词就为真，用例根本没等握手就往下跑。表现是"连接用例在 `sessionReady()` 上失败"，看起来像**界面或会话的 bug** | 判据改成 `startsWith("就绪")`。**教训**：等一个异步状态时，谓词必须**精确匹配目标态**，不能选会被"否定形式"命中的子串（与 C5、C13 同类：先分清是哪一边错） |
| **C25** | **启动用例漏了"选控制模式"这一前置动作**，直接点「使能并启动」收到 `IllegalState` NAK，表格里那一行的结果列是"被拒绝"。第一反应像是"设备把合法的启动拒了" | 查 `device_simulator.cpp` 后确认**设备拒得对**：`case Start:` 里写着"门禁放行了（确实是 `Ready`），但**没选控制模式就使能**在语义上不成立"→ 回 `IllegalState`。**是测试写错了**，用例改为"先下发 `SET_MODE(1)` 并等它被确认，再启动"；这条门禁顺带被用例钉住（另加一条"运行中改模式 → 表格出现被拒绝 + 原因"） |
| **C26** | **自检里有一条平台相关的伪判据。** 原先比的是"曲线控件**重绘次数** < tick 数"，理由是"证明定频刷新在节流"。但**无头环境里没有窗口系统来投递绘制事件**，控件不会自己画 —— 不给 `--shot-prefix` 时重绘次数干脆是 **0**，于是那条判据退化成 `0 < 284` 的恒真式；给了前缀也只是"我主动渲染了几次" | 改成数**界面刷新信号**（`statsRefreshed`）的次数：`284 拍 / 分频 5 ≈ 56 次`，这才是 ADR-006 的真实机制，且与平台无关。另保留一条独立判据"曲线控件确实被绘制过"（自检里显式渲染一次，并注明这是**取样手段**不是产品路径）。**教训**：判据要落在**机制**上，不能落在"这个环境下恰好也成立"的现象上 |
| **C27** | **变异 M28 第一版打空**：尖峰放在索引 500，而 500 恰好是 min/max 分桶的边界，"`indexMax` 恒取桶首"的退化实现也照样留住了它 —— 于是"弄坏代码"却全绿 | 把尖峰移到桶内部（505 / 506）后变异才红。**用例的数据点要避开"退化实现也能侥幸通过"的位置** —— 这与 C21（变异要红在目标用例上）是同一类问题的两个方向 |
| **C28** | **变异 harness 又坏了一次（与 C6 同类，换了个原因）。** ① `sed` 用了 `\|` 当分隔符，而**模式里本身含 `\|\|`**，替换后源码变成语法错 → 构建失败；随后跑的 `ctest` 其实用的是**旧二进制**，结论无意义。② 还原命令里的 `$R` 变量**被外层 shell 吃掉**，`cp $R/...` 变成 `cp /...` 静默失败，**还原从未生效**，工作树被污染，后续两轮全部建不起来 | 该轮证据作废。改法：sed 分隔符换成模式里不出现的字符；还原路径一律**写字面量**；并在每次变异前先 `grep` 确认"变异真的注入"。纪律里已有的"还原后必须回到全绿"这条自检再次起到作用 |
| **C29** | **中文界面在容器里全是方框。** 三页截图出来后发现**所有中文都渲染成 `□`**（数字与英文正常）——`fc-list :lang=zh` 计数为 **0**，容器里只有 DejaVu / Ubuntu 字体。这**不是观感问题，是"界面能不能读"**：他在 WSLg 桌面上看到的会是同一批方框 | 装 `fonts-noto-cjk`（下载 61.2 MB / 占 93.2 MB，落在他已迁到 F 盘的 vhdx 内），`fc-cache -f` 后 `:lang=zh` 命中 30 条，重出截图中文正常。**这条依赖已写进 README 的前置依赖**（不是可选项） |
| **C30** | **指纹口径漏了整个上位机。** 上一轮的口径是 `find CMakeLists.txt common tools` —— 增量 7 新增的 `host/` **不在其中**。这不是"少算几个文件"：它会让"绑定"在**改坏界面时依然对得上**，等于这份报告的约束力出现一个洞 | 口径扩为 `CMakeLists.txt common tools host`，两边平台重算得到同一个值 `198c2e11…`。**教训**：指纹的覆盖面要跟着"参与构建的产物"一起长，新增顶层目录时必须回头核对口径 |
| **C31** | **对上一版文档的更正：Qt 口径是错的。** README 与 §7 此前写着"Qt6 6.2.4 **已装并实测编过跑过**"，而我这次真去配 `Qt6Widgets` 时发现**根本配不上**：`/usr/include/GL/gl.h` 不存在 → `find_package(OpenGL)` 失败 → `WrapOpenGL` 失败 → Widgets / OpenGLWidgets 连带失败。当时验证过的只是 Qt **Core**（一个无头 console 小程序），**界面代码一行都没编译过**。**"装了个包并跑通一个探针"被写成了"GUI 可用"** | 装 `libgl-dev` 后真正打通：`mc_host` 能编译、能在 `offscreen` 下渲染出图（本轮三页截图即其产物）、能在 WSLg 下开窗。README 与本文档 §2 / §7 的口径一并更正。**教训**：模块级依赖要**逐组件**验证（`find_package(Qt6 COMPONENTS Widgets)` 单独跑一遍），不能拿"另一个组件的探针通过"外推 |

| **C32** | **CI 里一条恒真的断言。** 给 `.github/workflows/ci.yml` 写"确认没有 Qt6 时 `host/` 被安静跳过"这条检查时，我用的判据是 `build/host/CMakeCache.txt` 是否存在 —— 本地实测发现**这个文件在任何情况下都不存在**（CMake 只给调用 `project()` 的目录建 `CMakeCache.txt`，而 `host/CMakeLists.txt` 没有 `project()`）。也就是说这条检查**永远通过、什么也没验**。它与 C26（无头环境下"重绘次数 < tick 数"恒真）是同一类错 | 改为两条：① 配置输出里必须出现"跳过 host/"那句警告；② 产物 `build/host/mc_host` 必须不存在。并**反向验证**：`-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON` 模拟无 Qt 时两条放行，本机有 Qt 时两条都会红 —— 确认不是恒真。**教训**：写断言前先问"这条判据在反例下会不会也成立"；拿一个**自己都不确定是否存在**的路径当判据，等于把检查写成了装饰 |

| **C33** | **对 C26 那条记录的更正：理由写错了，结论仍然成立。** C26 说"无头环境里没有窗口系统来投递绘制事件，控件不会自己画"。本轮加 `mc_host_bench` 后实测到**离屏平台确实在投递绘制事件**：5 s 档 202 次、45 s 档 1790 次（只投给**当前可见页**）。当时自检里重绘次数为 0 的真正原因是：那 3 秒里当前页是总览页，**曲线页不可见**，所以它一次都没被画。C26 的改法（数刷新信号、另留一条"显式渲染一次"的判据）**依然正确**，但它给的理由会把后来者带偏 | 更正 `host/src/main.cpp` 里那句注释与 C26 的措辞为**准确**的原因：绘制由事件循环以**独立事件**异步投递、只投给当前可见页，因此它既不在 `onTick` 里、也不能假设"已经画过了"。`docs/PERF.md` 把"每拍耗时不含绘制成本"这条限定写在显眼处。**教训**：更正一条旧结论时，要连**理由**一起核 —— 结论对而理由错，下一个人就会按错的理由去推理 |
| **C34** | **实测里出现一个说不通的差额，追下去是"两个计数器在数不同的东西"。** 45 s 档某次运行：上位机收了 **4469 帧**遥测，而显示缓冲只写进 **3856 条**（另外两次同条件运行是 4096 + 丢弃 375/377 ≈ 4471 条，正好等于拍数）。第一反应是"丢帧了"，但拒收 0、CRC 错 0、解码总数也没少。追下去两个原因叠在一起：① 上位机把 `QElapsedTimer::elapsed()` 的**整毫秒数**当 tick 时刻传下去，真实间隔贴着 10 ms 时读数会 ±1 ms 抖动 —— 一条 9 ms 的步长让某拍收不到帧，随后的 11 ms 步长又补回来（帧数在 0/1/2 之间抖，**平均恒为 1.000**）；② 显示缓冲按设计**一拍只收最后一帧**，所以它写的是"有帧的那几拍"的条数，不是收帧数 | 把这条性质**变成一个用例**（`EndToEnd.MillisecondQuantizedClockDithersTheFrameCountWithoutLosingAny`）：喂入量化过的 0/1/2 序列，断言每拍条数**以及"63 ms / 10 ms = 6 帧"这条守恒**。同时写进 `docs/PERF.md` §5，免得读者对着"4469 帧 / 3856 条"自己猜。⚠️ 附带记录我自己的一次算错：这个用例第一版把 7 拍的和写成 7（实为 6，首拍只对齐基准），**是用例当场报的红** —— 判据自己抓住了我 |
| **C35** | **第一次性能测量把"机器正在忙"当成了机器的能力。** 我在同一个脚本里连着跑"构建 → ctest → 三个测试二进制 → 起 bench"，于是那一次的数字被前序负载污染：每拍耗时 P99 321 µs（另一次安静环境下是 238–277 µs）、整页绘制 P50 1458 µs（安静时 1296–1471 µs）—— 同一份代码，**差出一倍多**。如果不察觉，`docs/PERF.md` 就会写进一组偏高、也无法复现的数 | 重测：单独跑 bench、不并发任何东西，跑 3 次（5 s 档）+ 3 次（45 s 档），把**多次运行的区间**写进 `docs/PERF.md`，而不是单次结果；并在文档里点明"这些数字对机器负载敏感"。**教训**：性能测量与被测对象之外的负载不能共存 —— 基准数字的**可复现性**本身就是它可信度的一部分 |
| **C36** | **设备上报的速度有 3.3% 的系统性偏差（本轮最要紧的一条）。** 目标 100 deg/s，上报恒为 **96.6796875**；换算下来是每拍走 11 个计数（1 度 = 11.3778 计数），也就是**每拍被抹掉 0.3778 个计数**。根因不在控制律，在"量化"这一步：第一版**就地量化**（`position_ = round(position_*每度计数)/每度计数`），把机械位置本身按计数取整了 —— 于是不足一个计数的零头**每拍丢一次**，长期积分下来就是稳定的负偏差。真实编码器不会丢计数：机械位置是连续的，编码器只是把它**读数化** | 拆成两个量：`position_` 保持连续（机械真值），量化只写进 `measuredPosition_`（编码器读数）；位置/速度的反馈与上报一律取后者。之后偏差消失（模型在目标上稳定收敛）。**教训**：仿真里"把测量值写回状态"这种省事写法，会把传感器误差变成系统性偏差 —— 而这正是这类模型最容易骗过自己的地方 |
| **C37** | **`mc_sim --self-test` 的第三段一开始「在错误的位置通过」。** 它想验的是 F7（上位机不说话了 → 设备自己进 Fault 并锁存）。第一版没有开心跳，而上位机在命令下发完之后本来就不再说话 —— 于是**设备在断言之前就已经因为静默超时进了 Fault**，"制造一次通信丢失"这件事失去了因果，后面每一条断言都是在既成事实上通过 | 自检里让上位机按 100 ms 周期发心跳（真上位机就是这么保活的），F7 窗口压到 300 ms，链路对"短静默"的容忍设为 5 拍；断言改为先确认"有心跳保活时设备停在 Running"，再停掉上位机 400 ms 触发故障。**教训**：构造"某种原因导致的结果"时，必须先排除**别的原因也会导致同样的结果** |
| **C38** | **一次"假失败"：没有重建就拿旧二进制下结论。** 我改完代码只跑了测试二进制（脚本里少了构建那一步），于是看到的是**上一次的二进制**的结果，还照着那组陈旧输出追了一轮"为什么断言不生效"。这与 C3 / C6 / C12 是同一类错，只是换了个入口 | 复跑脚本一律**自带构建**并先判构建结果；"必须先确认构建成功再采信"这条纪律在 §6 的已固化清单里排第 1 条 —— 本轮又踩了一次，说明它写在纸上不够，得写进脚本里 |
| **C39** | **浸泡工具里"内存"这一项恒为"读不到"，而它恰恰是浸泡要看的核心指标。** 我用 `while (!status.atEnd())` 读 `/proc/self/status` —— `/proc` 里的文件 `size()` 恒为 0，而 `QFile::atEnd()` 会据此认为"已到末尾"，**循环体一次都不执行**，于是每次采样都返回 -1（表现成"本平台没有 /proc"）。这与 C32（拿 `CMakeCache.txt` 当判据）同类：判据本身恒真/恒假，检查就成了装饰 | 改成用 `readLine()` 返回空来判断结尾。修好后第一次就采到基线（`53 224 KB`），浸泡期间的走势才有意义。另外把内存基线从"建窗口之前"挪到"建窗口之后" —— 否则"起→末"的差值量的是**一次性的界面初始化开销**（约 31 MB），与"跑久了会不会涨"毫无关系 |
| **C40** | **一把尺子会往用户的真实记录里灌数据。** `mc_host_bench` 建的 `DeviceManager` 默认就把历史库开在用户数据目录 —— 跑一次基准就往那份"真实运行记录"里写几万行，跑一小时就是几十万行。而上位机用例同样如此：`ctest` 每跑一次，`~/.local/share/<测试二进制名>/history.db` 就长一截（实测 90 KB）。两者都会让人**再也分不清哪些数据是实际运行产生的** | 基准显式把库落在临时目录（跑前清空，文件增长才算这一次的）；两个测试入口切 `QStandardPaths::setTestModeEnabled(true)`，`DeviceManagerTest` 的 fixture 再把库重定向到 `QTemporaryDir`。改完之后复跑 `ctest`，用户数据目录里只剩**产品自己的** `mc_host/history.db`，测试侧只留 `~/.qttest/` 下三个空表结构的文件 |
| **C41** | **一条变异"打不红"，追下去是断言写得太弱（不是代码有错）。** M43-1 把"速度估计用未量化的真值"注入回去（等于取消量化），`EXPECT_NE(上报值, 真值)` **一条用例都没红** —— 稳态下这两个数值本来就几乎相等，断言只比"数值"，不问"出处"。这是本轮唯一一次"变异未被抓住"，也是最有价值的一条 | 换成**结构性判据**：上报速度必须落在「整数个计数 × 每计数速度」的格点上。改完同一条变异立刻变红。**教训**：断言要钉住**来源与结构**，只钉数值等于给"绕过机制的实现"留了后门（这与 C26「判据要落在机制上」是同一条纪律的第二次出现） |
| **C42** | **界面上的统计停在了"刚连上"那一刻（逐页看截图时发现的）。** 历史页顶栏写着"共 0 条样本 / 已落盘 0 条 / 0 个事务"，而同一页面下面两张表里明明有当前会话的数据 —— 页面自相矛盾。根因不是落库有问题（数据确实在库里，`session.sample_count` 与两张表都作证），而是这一页只在"链路状态变化"时刷新一次：那一次发生在刚连上、样本还没落盘的时候，之后再没有任何东西触发它。屏幕上于是显示**一个过时的快照**，而它恰好长得像"落库根本没生效" | 给页面加 `showEvent` → 切到本页时用当下的计数刷一次（仍然不按每拍刷新，理由见 `refresh()`）。并用一条界面用例钉住：切到历史页后**不按任何按钮**，顶栏也不许出现"已落盘 0 条"。这条断言**反向验证过**：把刷新去掉，用例立刻变红（`实际是：… 共 0 条样本 / 已落盘 0 条 / 0 个事务`）。**教训**："界面上的数字对不对"包括**它是什么时候算出来的** —— 一个过时的、看起来像故障的快照，与算错一样有害 |

| **C43** | **两条断言"打不红"，追下去是它们钉错了对象（不是代码有错）。** M44-3（遥测不发 `statusFlags` 字节）与 M44-5（设备不存软限位）起初用 `SimulatorModel.*` 过滤器跑，**全绿**。原因很具体：`TheHomingAttainedBitTravelsInEveryTelemetryFrame` 读的是**设备内部的结构体**、只顺带比了一下**编码后的长度**，编码那一步把字节写成 0 它察觉不到；而软限位这条线上，核心层**根本没有一条用例断言"`SET_LIMITS` 报文真的把限位存进了设备"** —— 唯一相关的拒绝用例断言的是"没落地"，而把存储整个删掉之后那句断言反而更"绿" | 改成钉**线上的字节**与**报文这条路径**：前者把设备真正发出的遥测帧解回来、逐字节确认那一位；后者新增 `SetLimitsCommandIsStoredAndComesBackInParamState`，发 `SET_LIMITS` 之后再走一遍 `READ_PARAMS` 确认回读里带的是同一组。两处改完，同样的变异立刻变红（§5.10 的 M44-3 / M44-5）。**这是 C41「断言要钉来源与结构」的第三次出现** |
| **C44** | **一次"把旧输出当成新结果"的假证据。** 我用 Git Bash 调 `wsl.exe bash /mnt/c/wslsync/xxx.sh` 跑构建/自测，其中一个脚本**始终没有产出结果文件**，而我差点把上一次留下的同名产物当成这一次的结果。查下来是 **Git Bash 会把命令行里以 `/` 开头的参数改写成 Windows 路径**（`/mnt/c/...` → `C:/mnt/c/...`），于是脚本路径根本没落到 WSL 里那个文件上；加上 `MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'` 之后立刻正常 | 采集脚本一律加这两个环境变量；**"脚本没有产出结果文件"必须当失败处理**，不能沿用上一次的同名文件（这与 C3/C6/C11/C38「先确认构建/执行真的发生了」是同一条纪律的又一次落地） |
| **C45** | **一条假红的断言：错在测试辅助函数，不在被测代码。** M44-3 强化后的第一版仍然"失败"，追下去是 `DeviceDriver::drain()` **只读了一次 4096 字节**。而 `send()` 从不取走设备的发件箱，整场会话（含长达上千拍的找零过程）的帧全堆在里面 —— 只读一次拿到的是**会话最早那一段**（回零之前的帧，那一位本来就是 0） | `drain()` 改成**循环读到空**。修好后断言立刻转绿，且对应的变异能把它打红。**教训**：观测辅助函数本身也要怀疑 —— "红"既可能是实现错、断言错，也可能是**取样方式**错（与 C5/C13/C25 同类：先分清是哪一边错） |
| **C46** | **本报告的数字整体滞后了一个大版本。** 增量 12 之后又做了 P0/P1 六批功能，但 §1 的指纹、§4 的 ctest 数、§4.1/§4.2 的套数与条数仍停在 `205 / 205`（增量 12），明细却已对不上 | 按本轮实测重填：指纹 `8d2ea465…`（124 文件 / 29913 行）、ctest `352`、gtest `257 + 75 + 18 = 350`、自检 2；§4.1 补齐到 26 个核心套 257 条、§4.2 补齐到 8 个上位机套 93 条。这正是纪律 8（计数互不自洽就是可信度问题）的又一次适用 |
| **C47** | **写界面用例时发现一个真缺陷：双轴页的连接按钮与状态标签**永远不会**在握手完成时更新。** 那一页只在 `groupChanged` 信号上重算按钮与标签，而"两轴握手好了"**不是一次阶段变化**，根本不发 `groupChanged` —— 于是按下"连接两轴"之后，按钮一直亮着、标签一直停在"正在握手…"，直到用户去点别的东西触发一次重算。屏幕上看起来像"连不上"，实际链路早就好了 | 把两轴的 `statsRefreshed`（20 Hz）接到 `refreshStatus`（它内部已经调了 `refresh`），让就绪状态跟着节拍重算。用例断言`连接按钮在两轴就绪后变灰且标签写着"就绪"`；把连线改回去，用例立刻变红（M45-8）。**教训**：界面上的状态如果**没有对应的信号**，就必须挂在某个节拍上重算 —— 否则它只在"恰好有人触发"时才更新 |
| **C48** | **双轴协调的启动一开始根本发不出去（写用例时当场暴露）。** 第一版把 `SET_MODE`（切速度环）与 `START` 放在**同一次调用**里连着发，而启动联锁里"已选控制模式"读的是**设备回读回来的**模式（`DeviceManager::interlockSnapshot` 取 `device_->runMode()`）。同一拍里设备还没报上新模式，第二道本地闸（`sendStart` 的 `startBlockReason`）当场把 `START` 拒了 —— 表现是"点了协调启动、没有任何反应" | 改成**跨拍**推进：先切模式 → 等两轴都把新模式报上来 → 再 `START` + 给速度；每一步都等上一步真的落地（`advanceStarting`）。其实核心库的 Sequencer 早就是这套做法（它把"设模式"和"启动"拆成两步），这一版是把它漏了。用例把顺序钉住：`moveTo` 返回后**不许**已经存在 `START`（M45-6） |
| **C49** | **收尾自查抓到两类"覆盖的洞"，一类是"假按钮"、一类是**我自己新写的假断言**。** ① 全量扫新页面的控件后发现**两个按钮没有任何用例点到**：「全组停止」与「清空视图」—— 底层逻辑都覆盖了，但"按钮接没接上"没有判据，连线漏了照样全绿（正是任务书"禁止仅实现界面而没有业务逻辑"盯着的地方）。② 补「全组停止」断言时，第一版谓词选了 `"已发出全组停止"`，**而"按计划走完"那一刻的文案里也有这五个字** ⇒ 按下按钮之前断言就成立，把连线拆掉**照样绿**（M45-10 第一轮）。 | ① 补两条 UI 断言：点「清空视图」后表清空、且**"环形丢弃"计数不被抹掉**（为此把日志器换成一个容量 2 的、写 5 条造出非零丢弃数）；点「全组停止」后状态栏必须改口。② 谓词改成**只有人工停止那条路才会说的话**（`行程收不回来`）并同时钉住阶段文案 `阶段：已停止`。补完再拆连线，M45-10 / M45-11 双双变红。**纪律 12（谓词必须精确匹配目标态）第二次出现，而这次是在我自己刚写的断言上。** |

**已固化的纪律**：
1. 采信测试结果前**必须先确认构建成功**（跳过构建的复跑会用旧二进制）。
2. 退出码用 `if cmd` 判断，**不要靠 `$?` 跨 shell 边界传递**。
3. 变异 harness 必须自带"还原后复跑应回到全绿"的自检 —— 否则还原失败会被静默当成"变异生效"。
4. 断言"没有坏数据"之前，先给出**同一次运行里"确实在传数据、确实在坏"**的正对照。
5. **新功能必须同时有正对照与负对照**：F7 只做"超时会停车"一个用例是不够的 ——
   还要有一条"把窗口关掉它就真的不停车"的负对照，才能说明测的是保护本身。
6. **跨模块的定值耦合要写成测试**，不能只写在注释里（F7 窗口 vs 心跳周期就是这么抓到的）。
7. **变异必须红在「目标用例」上。** 红在别处要当"判据没对准"处理，而不是当成有效证据（C21）。
8. **报告里的计数器（套数 / 条数 / ctest 数字 / 指纹）每次更新必须互相自洽**，
   不能出现"总量 129、明细停在 90"这种漂移（C19）。
9. **"判了却不做"是本项目最容易漏的一类缺陷。** 一个决策点如果没有对应的执行点，
   所有单测都会绿 —— 因为它"判断得完全正确"。所以每个判 `Retry`/判超时的地方都要追问：
   **谁真正动了手？** 这条是 C18 的教训。
10. **指纹口径本身也要跨平台核一遍。** `sha256sum` 的标记随平台变（C23），
    聚合值对不上时**先比逐文件哈希**再怀疑源码 —— 否则会把工具差异误判成"源码不一致"。
11. **指纹的覆盖面要跟着"参与构建的产物"一起长。** 新增顶层目录（本轮是 `host/`）时必须
    回头核对口径 —— 否则"绑定"会在**新代码被改坏时依然成立**（C30）。
12. **等异步状态时，谓词必须精确匹配目标态。** `contains("就绪")` 会被"**未**就绪"命中，
    于是断言根本没等（C24）。选判据时先想一遍"它的否定形式会不会也匹配"。
13. **失败先分清是哪一边错，再动手。** 本轮"启动被拒"是**用例漏了选模式**这一前置动作，
    设备拒得完全正确（C25）。看到"实现把合法操作拒了"时，先去读门禁的源码再说。
14. **判据要落在机制上，不要落在"这个环境下恰好也成立"的现象上。**
    "重绘次数 < tick 数"在无头环境里是恒真式（C26）；换成数刷新信号次数才对准 ADR-006 的机制。
15. **用例的数据点要避开"退化实现也能侥幸通过"的位置**（C27：尖峰落在桶边界上，
    去掉极值保留也照样绿）。
16. **"变异没打中"也是一个结论，必须写清楚是哪种**：是测试构造不对、还是那段代码本就冗余
    （M29 属于后者 —— 冗余守卫杀不出来，如实记为冗余，不要为了凑数改测试）。
17. **实测数字里出现的差额必须追到机制，不能留着不管。** "收帧 4469 / 缓冲 3856"看起来像丢帧，
    实际是两个计数器在数不同的东西（C34）。追清楚之后**把它变成用例**，下次再出现就有判据，
    而不是又得重新推理一遍。
18. **性能测量不能与被测对象之外的负载共存。** 同一个脚本里"构建 + 跑测试 + 起 bench"会让数字
    虚高一倍以上（C35）。基准要单独跑、跑多次、报区间、写清条件 —— **可复现性本身就是可信度的一部分**。
19. **断言要钉住"线路上真正发生的事"与"报文这条路径"，不能只钉内部结构体或构造默认值。**
    `TheHomingAttainedBitTravelsInEveryTelemetryFrame` 只看设备内部结构体，
    于是"编码那一步把它写成 0"照样绿；软限位这条线在核心层没有一条"报文真的把它存进去了"的断言（C43）。
20. **跨 shell 边界调采集脚本要禁用 MSYS 路径改写**（`MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'`），
    并且**"脚本没有产出结果文件"当失败处理** —— 不能沿用上一次的同名文件（C44）。
21. **观测辅助函数本身也要能被打红。** `drain()` 只读一次缓冲区，拿到的是会话最早那一段，
    造成一条"假红"；改法是不许它对**取样完整性**做任何假设（读到空为止）（C45）。
22. **界面上的状态若"没有对应的信号"，就必须挂在某个节拍上重算。**
    双轴页的就绪状态没有信号可挂，于是连接按钮与标签永远停在上一刻（C47）——
    屏幕上看起来像"连不上"，代价是有人会去现场重插线。
23. **多步操作要跨拍推进，每一步都等上一步真的落地。** 同一拍里连着发 `SET_MODE` 与 `START`，
    第二道本地闸会按**尚未更新的快照**把它拒掉（C48）。核心库的 `Sequencer` 早就是这么做的，
    新写的编排必须照同一条规矩来。
24. **"仿真下等价"要当成一个结论写出来，而不是当成"测过了"。**
    M45-6 第一版打不红，根因是仿真设备的 `runMode` 同拍就更新，而真串口要一次往返（§5.11）。
    这类差异必须如实写进 §7 的边界表 —— 否则"用例绿了"会被读成"真硬件上也验过了"。

## 7. 边界与未覆盖面（不得据此宣称的部分）

| 项 | 状态 |
|---|---|
| CI 是否跑过 | ✅ **增量 19 及之前跑过**（`.github/workflows/ci.yml`，2026-10-16 首次运行成功，70 秒）。增量 20 的 workflow 已扩成**四个 job**（指纹 / 协议核心 / 固件核心 / Qt 上位机），固件 job 跑 20 条宿主测试（含真 PTY）；**本次推送后重跑，结果以 Actions 为准**。本报告里 409 条结果都是本地 WSL 跑出来的；CI 绿也**不等于**"硬件上的东西验过了" |
| Qt 上位机 · 逻辑与交互 | ✅ 已覆盖：10 个套 101 条 + 13 条自检判据（连接/握手、命令结算、NAK 原因、超时、自动重连、故障确认清除、遥测入库、曲线取窗与抽稀、十二页控件联动、数据源与角色切换、历史页读回、配方下装与回读、任务逐步执行、回零与软限位、双轴协调与全组安全闸、日志分级与滚动、**每个按钮的接线都被点到**、计时诊断开关） |
| Qt 上位机 · 观感 | ⚠️ 已由人工在真显示下逐页操作过（三页时代），此后又长出九页、共**十二页**。自动判据管的是"控件存在、被点动、显示了正确结果"，**观感本身没有像素级判据**；十二页的人工验收与截图说明见 §4.4 |
| Qt 上位机 · 串口路径 | ✅ **已实现并验证**：`SerialTransport`（QSerialPort → `mc::ITransport`）+ `SerialDevice`，在**内核伪终端**上跑通"握手 → 下发 → 回读遥测"整条路（3 条用例）。⚠️ 但**伪终端不是电气层**：波特率、校验位、流控在 PTY 上只是被记录、**不起作用**，所以"115200 能不能跑满 100 Hz 遥测"这类问题在这里**测不出来**；本机没有真串口对，真串口的电气层行为**未验** |
| Qt 上位机 · 桌面性能 | ⚠️ **只测了单线程 CPU 侧，且是在离屏平台上测的**（`Release` + `offscreen`）。基线见 `docs/PERF.md`；长稳（≥1 h 浸泡）的数字也在那里。**真显示下的绘制与合成开销、真实串口的吞吐未测** —— 不得把这份基线当成"桌面性能已达标" |
| 设备侧模拟器 `mc_sim` | ✅ **`--pty` 与 `--self-test` 已验**（伪终端上的完整会话、上行损坏注入、F7 与确认清除，51 项检查）。❌ **`--port <真串口>` 未验**：本机没有真串口，这条路径属未验证代码（代码里已按 raw + 波特率表配好，但没有跑过）。伪终端**不覆盖**波特率/校验位/流控 |
| Qt 依赖边界 | ✅ 仍成立：`mc_core_tests` / `mc_loopback_demo` / `mc_sim` **三个**二进制 `ldd \| grep -ci qt6` 均为 **0**（`mc_core` 不链接 Qt） |
| 自适应超时的有效性 | ⚠️ 只在**进程内回环的注入延迟**下验证过。真实串口的 RTT 分布不同，窗口参数必须实测后再定。**不得把演示里的 640 ms 当成任何硬件的数字** |
| 上位机自动重连 | ⚠️ 只在**进程内回环 + 故障注入**下验证过。没有真串口拔插、没有设备真实重启；"设备重启后 `seq` 归零、而旧会话去重窗口还在"这条路径（C15 的场景）在真链路上**未验**。另外：**重连必须依赖 `LinkMonitor` 才能看出链路失效** —— 没开链路监控时，本层不会被通知，这是设计上的取舍而不是保证 |
| 设备状态机 / F7 / 回零 / 软限位 离真硬件还有多远 | ⚠️ **语义**已由核心单元、穷举、模拟器集成、`mc_sim` 伪终端自检与**固件宿主测试**验证；**固件已烧录并跑通真串口会话、开环转动与编码器速度闭环**。❌ 但实物上的**失联停车（F7 拔线）、真实回零找零、软限位执法、位置环、双向运动**仍未验 —— `volatile`/临界区行为只在短时联调中观察过，不得声称已在硬件上成立 |
| 工艺配方 / 任务调度 / 报警分级 / 联锁 / 权限与审计（增量 13–17） | ⚠️ **语义与上位机侧已实现并测试**（核心 26 套内的 `Recipe` / `Sequence*` / `Alarm*` / `Audit*` / `InterlockTable` + 上位机 `RecipeStoreTest` / `SequenceStore` 与界面用例）。覆盖的是**模型、校验、持久化、上位机联动**；**设备侧对配方/任务的执行**（真机上按配方跑工艺）**未接硬件、未验证** —— 设备侧目前只在模拟器里裁决命令 |
| 多轴抽象与双轴协调（增量 19） | ⚠️ **"同时到达"的数学是穷举验证过的**（`AxisGroup` 13 条纯函数用例），轴组的编排与全组安全闸在**两台进程内模拟器**上验过。**但"两轴同时出发"这件事只在单线程一拍之内成立** —— 两轴命令落在同一拍（10 ms），偏差不超过一拍，**不得说成"严格同步"**；它也**不是插补**（中间不约束两轴轨迹关系）。另外，`advanceStarting` 里那道"等设备把新控制模式报上来再发启动"的等待，在**仿真设备下与"隔一拍就发"等价**（`SimulatedDevice::runMode()` 同拍更新），只有**真串口**才需要它（`SerialDevice::runMode()` 报的是被确认过的模式）—— 所以这条等待在真硬件上的效果**未验**（见 §5.11） |
| 运行日志分级与滚动（增量 19） | ⚠️ **语义与上位机侧已实现并测试**（核心 `LogLevel` / `LogLine` / `LogRing` / `RotatingFile` / `Logger` 16 条 + 宿主的 `DeviceLoggingHost` + 界面用例）。覆盖的是**级别过滤、有界环形、按大小轮转、写失败计数**。**滚动文件只在本地文件系统上验过**：磁盘写满、目录权限变化、多进程同时写同一个日志文件这些情况**未测**；本类**不做线程安全**（设计上由单线程持有，见 `docs/PERF.md`）。日志与报警、审计是**三条独立的流**：调日志级别既不会让报警消失、也不会少一条审计流水 |
| 固件（增量 20–21） | ⚠️ **已写、已交叉编译、宿主测试 20 / 20（含真 PTY 端到端）**；ELF `text 8623 B / data 0 / bss 272 B`，入口 `0x08000425`、初始 SP `0x20005000`、向量表 59 项已核对。✅ **已烧录（Verify OK）、已接 STM32F103C8T6 + TB6612FNG + N20 编码器电机、已在 115200 真串口上建立会话**；开环转动与编码器速度闭环已实测。❌ 位置环、反向、真实回零、软限位与 F7 拔线保护**未在实物上验证**；无 RTOS、DMA、bootloader、参数掉电保存 |
| 真实串口 / 真实设备 | ✅ **已测（部分）**：CH340 → WSL `/dev/ttyUSB1`，115200 8N1，上位机会话完成握手并持续收遥测；电机由 TB6612 驱动并带编码器反馈。⚠️ 只覆盖**空载、短时、正向**；5V/3.3V 供电余量、长时间稳定性、丢线重连与电气层抗干扰**未测**。❌ 位置环与双向驱动见上 |
| 并发 / 线程安全 | ❌ **未测**。全部组件都**不是线程安全**的，设计上由单线程持有 |
| 运动模型 | ⚠️ 设备侧已是**带机械与传感环节的声明式仿真模型**（一阶惯性、编码器量化、固定窗口速度估计、PI(D) 抗积分饱和），但仍然是**声明的模型，不是物理辨识结果、也不是任何实物的测量**。参数（时间常数 40 ms、4096 计数/转、差分窗口 50 ms）是**选的**，不是量出来的 |
| 落库口径 | ⚠️ 历史库按"**一拍一条**"记录（＝界面那一刻显示的那条），不是"线上每帧一条"。100 Hz 稳态下两者比值约 **92%**（实测，见 `docs/PERF.md` §5）——走到同一拍里的两帧只留最新的一条。这是"历史与屏幕看的是同一组数字"这条设计选择的代价，**不是写入失败**（环形缓冲与落库的计数互相印证：44207 vs 44199）。若需要"线上每一帧都归档"，那是另一套口径，尚未实现 |
| 性能数字 | ⚠️ **只有上位机单线程的 CPU 侧基线**（`docs/PERF.md`，`Release` + 进程内回环 + 离屏渲染）。**没有任何硬件性能数字**，也不提供真实串口的吞吐/时延 |
| 跨平台 | 仅在 `g++ 11 / Linux x86-64` 上验证；MSVC / clang / 32 位未测。`mc_sim` 与 `FdTransport` / `pty` 是 **POSIX-only**（Windows 上不构建该可执行程序） |
| 长时间稳定性 | ✅ 已做：**1 小时浸泡**（`mc_host_bench --soak --seconds=3600`，100 Hz，含 SQLite 落库）。数字与判读见 `docs/PERF.md` §4.3。⚠️ 覆盖的是**上位机单线程在离屏平台上的长稳**，不是硬件长稳 |

**任务书要求 12「需硬件验证的功能不得宣称已验证」在本报告中得到遵守：
第 7 节列出的所有未覆盖面，本报告一律不声称通过。**
