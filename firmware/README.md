# 固件支线（STM32F103C8T6）

这是同一套设备协议的另一端：跑在 STM32F103C8T6（Blue Pill）上的最小固件。
它与上位机共用 `common/` 里的状态机与协议常量，收发、拆帧、命令处理、
运动模型都在 `firmware/` 里；宿主端测试用 `common/` 的编解码器做独立参照。

> **验证边界**：已烧录到 Blue Pill（OpenOCD `Verify OK`），并与真上位机通过 CH340 串口
> 完成握手、命令与遥测；TB6612FNG + N20 编码器电机已接入，速度环使用 TIM4 的真实
> 编码器反馈。位置环、真机回零/软限位与拔线保护**尚未验证**。

## 已实现

- 帧格式：`SOF 0xAA55 + VER 0x03 + TYPE + SEQ + LEN + PAYLOAD + CRC-16/CCITT-FALSE`
- 命令：`HELLO`、`HEARTBEAT`、`SET_MODE`、`SET_TARGET`、`START`、`STOP`、
  `SET_PID`、`READ_PARAMS`、`HOME`、`SET_LIMITS`、`CLEAR_FAULT`
- 设备 → 上位机：100 Hz 遥测、故障事件周期重发
- 状态机：直接复用 `common/src/device_state.cpp`，与上位机/模拟器同一张门禁表
- 安全：通信丢失 3 s 停机、软限位、回零超时、超速保护、故障锁存
- 重复 SEQ 去重并重放上一次应答；`HELLO` 清空去重窗口
- 外设：USART1 中断收 + 环形缓冲、SysTick 1 ms 时基、TIM3 CH1 10 kHz PWM、
  TIM4 CH1/CH2 四倍频编码器输入（PB6/PB7）、TB6612 双方向输出（AIN1=PA7、AIN2=PB0）
- 真实反馈：编码器计数按 2800 计数/输出轴转换算位置，速度在 50 ms 窗口上差分；
  `Config::externalFeedback` 关闭时退回虚拟运动模型（宿主测试与 `mc_fw_sim` 用）
- 板载 LED 指示状态：Ready 慢闪、Running/Home 快闪、Fault 常亮

## 目录

```
firmware/
  include/mc_fw/       帧解析/编码、设备核心接口
  src/                 固件核心实现（不碰 GPIO/UART）
  port/host/           mc_fw_sim：用固件核心在 PTY/串口上扮演设备
  port/stm32f103/      裸机端口、启动文件、链接脚本
  tests/               宿主端单元测试 + 真 PTY 端到端
  cmake/               交叉编译工具链文件
```

## 宿主构建与测试

```sh
cmake -S firmware -B firmware/build -DCMAKE_BUILD_TYPE=Debug
cmake --build firmware/build -j"$(nproc)"
ctest --test-dir firmware/build --output-on-failure
```

共 20 条：帧/CRC 6 条、设备命令与安全 12 条、外部反馈 1 条、真 PTY 端到端 1 条。

## 与真上位机联调（没有板子也能先跑）

```sh
./firmware/build/mc_fw_sim --pty
# 输出：从端: /dev/pts/N
```

在上位机“系统设置”页把串口设成这个 `/dev/pts/N`，然后连接。
这时上位机对面跑的就是**将来要烧进 STM32 的那一份固件核心**，
只是 UART 换成了 PTY。拔掉 `mc_fw_sim` 相当于拔线，可以用来验证通信丢失故障。

## 交叉编译

标准安装（`arm-none-eabi-g++` 在 PATH 上）：

```sh
cmake -S firmware -B firmware/build-stm32 \
  -DCMAKE_TOOLCHAIN_FILE=firmware/cmake/arm-none-eabi.cmake \
  -DMC_FW_PLATFORM=stm32f103 \
  -DMC_FW_BUILD_TESTS=OFF \
  -DMC_FW_BUILD_SIM=OFF
cmake --build firmware/build-stm32 -j"$(nproc)"
```

产物：`firmware/build-stm32/mc_fw_stm32f103.elf`。

本机实测（ARM GNU 10.3.1，Cortex-M3）：

```
text 8623 B, data 0 B, bss 272 B
入口 0x08000425（Reset_Handler，Thumb 位）
向量表首项 0x20005000（栈顶），Reset 向量 0x08000425
```

## 接线（STM32F103C8T6）

| STM32 引脚 | 作用 | 接到 |
|---|---|---|
| PA9 | USART1_TX | USB-TTL 的 RX |
| PA10 | USART1_RX | USB-TTL 的 TX |
| GND | 地 | USB-TTL 的 GND、公共地 |
| PA5 | TB6612 使能 | `STBY` |
| PA6 | TIM3_CH1 PWM | `PWMA` |
| PA7 | 方向位 1 | `AIN1` |
| PB0 | 方向位 0 | `AIN2` |
| PB6 | TIM4_CH1 | 编码器 `C1` |
| PB7 | TIM4_CH2 | 编码器 `C2` |
| 3.3 V | 逻辑电源 | TB6612 `VCC`、编码器 `VCC` |
| PC13 | 板载 LED | 板载 LED（低电平点亮） |

- 串口固定 **115200 8N1**；固件用 8 MHz 内部 RC，没有配外部晶振。
- 逻辑电平 3.3 V；PA10 不要直接接 5 V 的 TTL 输出。
- TB6612 A 通道真值表：`AIN1/AIN2 = 1/0` 正转、`0/1` 反转、`0/0` 滑行；
  `STBY` 低电平为待机。禁用输出时固件把两个方向脚都拉低。
- 编码器 `VCC` 只能接 3.3 V；`PPR = 2800` 计数/输出轴转（7 线 × 4 倍频 × 1:100）。
- 5 V 电机电源只进 TB6612 的 `VM`，其负极必须与逻辑地共地。
- ⚠️ 当前台架接线仍是 `AIN2` 接 GND（单方向）。要走反转/位置环的负目标，
  必须**断电后**把 `AIN2` 改接到 PB0；未改接时固件虽然会置 PB0，但引脚上没有线。

## 烧录

以下命令已在本机执行（Windows 侧 xpack OpenOCD 0.12 + ST-Link V2）：

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build-fw-stm32/mc_fw_stm32f103.elf verify reset exit"
```

实测关键输出：`device id = 0x00006410`、`flash size = 128 KiB`、`Programming Finished`、
`Verified OK`、`Resetting Target`。

另两条备选路径（本机未使用）：

```sh
# STM32CubeProgrammer CLI
STM32_Programmer_CLI -c port=SWD -w build-fw-stm32/mc_fw_stm32f103.elf -v -rst

# 串口 ISP（BOOT0=1，重启后进入系统存储器）
arm-none-eabi-objcopy -O binary build-fw-stm32/mc_fw_stm32f103.elf build-fw-stm32/mc_fw.bin
stm32flash -w build-fw-stm32/mc_fw.bin -v -g 0x0 /dev/ttyUSB0
```

## 当前未覆盖

- 位置环定位、双向运行、真机回零/软限位、拔线后的通信丢失保护、长时间稳定性尚未验证
- 编码器只有 A/B 相、没有 Z 相；回零仍依赖软件逻辑，没有原点/限位开关
- 没有 RTOS、DMA、bootloader、参数掉电保存
- 运动模型是单轴最小实现，没有多轴插补/电子齿轮
