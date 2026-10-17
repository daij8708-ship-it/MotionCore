#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// Sequence —— 自动任务调度（P0）
//
// 解决的问题很具体：一次完整的动作不能靠人在界面上按顺序点六个按钮。
// 那六个按钮之间没有顺序约束、也没有"上一步没成功就不许走下一步"这件事，
// 全靠操作员记着 —— 而"记着"不是一种工程手段。
//
// 一部程序是**一串有序的步**，每步只做一件事，跑起来遵守三条铁律：
//
//   ① **一步在飞，绝不并行**。同一时刻只允许有一条命令等结局。
//      两条在飞的时候，一个 NACK 说到不了底是谁的 —— 而这正是自动流程
//      最需要能说清的一件事。
//   ② **不确定就停**。任何一步的结局不是"已确认"（被拒 / 超时 / 本地没发出去），
//      整部程序当场停在那一-步上，说明原因。运动指令**从不自动重发**（ADR-003），
//      所以流程也不会自己再来一遍 —— 要继续就得有人看明白之后重新启动。
//   ③ **"中止"不是"撤销"**。中止只让后面的事不再发生；已经下发过并被确认的命令
//      发生在过去，收不回来，设备就停在那时的状态。界面必须把这句话说出来。
//
// 它住在核心库里，且**不认识设备**：只回答两个问题 —— "现在该做哪一步"
// 和"刚那一步的结果说明接下来怎么办"。真正发命令的是宿主那侧（那里的门禁、
// 链路状态、会话就绪只有它知道）。这样这部引擎可以脱离界面和串口穷举验证。
//
// 与配方（Recipe）的分工：配方回答"**按什么参数**干活"（一份可存可读的参数集），
// 程序回答"**按什么顺序**干活"。两者正交：同一份配方可以被多部程序引用，
// 同一部程序也可以在下装不同配方之后跑。
// ---------------------------------------------------------------------------

// 一部程序的步数上限。有界不是为了省内存，是为了让"这部程序要干什么"
// 能被人在屏幕上从头到尾复核一遍 —— 一百步的程序没人核得完，
// 而没人核得完的自动流程不该跑在设备上。
inline constexpr std::size_t kMaxSequenceSteps = 32U;

// 规范字节里每一步的**定长部分**：类型(1) + 模式(1) + 等待(8) + 四个 float(16) + noteLen(2)。
// 说明文字另算（不定长）。解码时先按它判"够不够长"，所以它必须与上面的布局一致。
inline constexpr std::size_t kSequenceStepFixedBytes = 28U;

// 单个 Wait 步的上限（10 分钟）。等待不产生任何动作，所以它上限的意义是拦住
// "把 60 写成 60000" 这类笔误 —— 那种程序会安静地卡在原地，而且看起来一切正常。
inline constexpr std::uint64_t kMaxWaitMs = 10U * 60U * 1000U;

// 一步说明文字的最大字节数（UTF-8 字节）。它不上线，所以只是个防失控的上限。
inline constexpr std::size_t kMaxStepNoteBytes = 64U;

// 程序名的最大字节数。名字不上线（只有主机库里存它），所以它只是个上限，
// 不是线格式常量。
inline constexpr std::size_t kMaxSequenceNameBytes = 48U;

enum class SequenceStepKind : std::uint8_t {
    SetMode = 0U,
    SetPid = 1U,
    SetTarget = 2U,
    Start = 3U,
    Stop = 4U,
    // 盲等一段时间。它是"给机械/工艺一点时间"，**不是**"等设备停下来"。
    Wait = 5U,
    // 等设备**真的**回到 Ready（输出未使能、不运动中），最多等 waitMs。
    //
    // 为什么必须单独有这一步：STOP 被接受 ≠ 设备已经停下 —— 设备收到 STOP 之后
    // 先进 Stopping，把速度收到零才回 Ready（见 DeviceState）。而 SET_MODE 与 START
    // 都只在 Ready 放行。所以一部以 STOP 结尾、后面不等待的程序，会让**下一部**
    // 程序的第一条命令被设备以"状态不允许"拒掉 —— 而那个报错指向的地方
    // （下一部程序）根本不是病因。等一下就对了。
    WaitReady = 6U,
    // 回零（P1-6）。它用的是 `target` 与 `waitMs` 两个字段，含义随类型变：
    //   target = **回零速度**（deg/s，幅值，> 0）
    //   waitMs = **超时上限**（回零走不完就算这一步失败）
    // 复用字段而不是新开两个：这个结构本来就是"每种步只用得上其中几个字段"
    // （见上面的说明），而速度和时间在概念上正是同一对参数。
    Home = 7U,
};

const char* sequenceStepKindToken(SequenceStepKind kind) noexcept;

// 一步。**不是一个联合体**：每种步只用得上其中几个字段，其余保持默认值。
// 用联合体省下的那几个字节换不来任何东西，却会换来"读错字段"这种查不出来的错。
struct SequenceStep {
    SequenceStepKind kind = SequenceStepKind::Wait;
    std::uint8_t mode = 0U;                  // SetMode
    float target = 0.0F;                     // SetTarget＝目标值；Home＝回零速度
    float kp = 0.0F;
    float ki = 0.0F;
    float kd = 0.0F;                         // SetPid
    std::uint64_t waitMs = 0U;               // Wait＝时长；Home＝超时上限
    std::string note;                        // 这一步是干什么的（给人看，可为空）
};

struct SequenceProgram {
    std::string name;                        // 空名不合法：名字是人选它来跑的依据
    std::uint16_t version = 1U;              // 0 不是合法版本，它只可能是"没填"
    std::vector<SequenceStep> steps;
};

enum class SequenceError : std::uint8_t {
    None = 0U,
    // --- 结构层（decodeSequence）---
    Truncated,       // 字节流不够读完一个字段
    BadStepCount,    // 步数为 0 或超过 kMaxSequenceSteps（格式层面的界）
    BadKind,         // 步类型字节不认识 —— 字节流与这个结构对不上
    TrailingBytes,   // 读完一部程序还剩字节
    // --- 语义层（validateSequence / validateSequenceStep）---
    EmptyName,
    NameTooLong,
    BadVersion,
    NoSteps,
    TooManySteps,
    BadMode,
    TargetNotFinite,
    TargetOutOfRange,
    GainNotFinite,
    GainNegative,
    GainTooLarge,
    WaitWithoutDuration,
    WaitTooLong,
    NoteTooLong,
    // --- Home 步 ---
    // 这一组与 Wait 步的 WaitWithoutDuration / WaitTooLong 是**同一个字段**
    // （Home 步的 waitMs 就是超时上限），但**不共用错误码**：错误码是要显示给
    // 操作员看的一句话说清"哪一步、错在哪"，而"等待步没写时长"在一部回零步
    // 上会把人引向完全错误的字段。
    HomeSpeedMissing,      // 没给速度（或给了 0 / 负数）：那是永远走不到的回零
    HomeSpeedTooLarge,
    HomeTimeoutMissing,
    HomeTimeoutTooLarge,
    // 回零步后面必须跟一步「等设备就绪」（整部程序层面的规则，见 validateSequence）。
    HomeWithoutWaitReady,
};

const char* sequenceErrorName(SequenceError error) noexcept;

// --- 存库用的规范字节（**不是线格式**）-----------------------------------------
//
// 协议 v1 是冻结的，里面没有"下载整部程序"这条消息；这个编码只有两个用途：
// 让一部程序在库里有一个**确定的**表示，以及让校验和有意义。
// "确定"的含义与配方相同：同一部程序永远得到同一串字节 —— 否则校验和只能
// 证明"字节没变"，证明不了"语义没变"。
//
// 布局（小端）：
//   0        2    nameLen
//   2    nameLen  名称字节（UTF-8，无终止符）
//   2+n      2    程序版本
//   4+n      2    步数
//   6+n     ...   每步：
//                   1   类型
//                   1   模式
//                   8   等待毫秒
//                   4   target
//                   4   kp
//                   4   ki
//                   4   kd
//                   2   noteLen
//              noteLen note 字节（UTF-8）
std::vector<std::uint8_t> encodeSequence(const SequenceProgram& program);

// 只做结构校验。允许直接对"界面刚编辑完、还没编码"的结构体调用。
SequenceError decodeSequence(const std::uint8_t* data, std::size_t len,
                             SequenceProgram& out) noexcept;

// 校验和 = CRC-16/CCITT-FALSE 覆盖 encodeSequence() 的**全部字节**。
// 复用协议与配方同一套 CRC：多写一套多项式只会多一个"两边不一致"的机会。
std::uint16_t sequenceChecksum(const SequenceProgram& program) noexcept;

// 单步校验。target 的合法区间**取决于当时生效的模式**，所以要把那个模式传进来 ——
// 校验不能凭空猜"这个 200 是 200 deg/s 还是 200 deg"。
SequenceError validateSequenceStep(const SequenceStep& step, std::uint8_t effectiveMode) noexcept;

// 整部程序校验。它自己会在程序里跟踪"当前模式"：遇到 SetMode 就更新，
// SetTarget 就按那一刻的模式判。initialMode 是程序开跑时设备上已经生效的模式。
//
// 它还管一条**跨步**的规则：回零步后面必须紧跟一步 WaitReady。
// 依据不是"保险起见"，而是 HOME 的应答语义本身（见 protocol.hpp 的 Home）：
// 应答只表示**回零开始了**，而"回零完成、零点建立"这件事只能从设备状态看出来。
// 不等待就往下走，程序会在下一步撞上"设备不 Ready"——而那个报错指向的是下一步，
// 不是"回零还没走完"这个真正的病因。
//
// 为什么只有 HOME 有这条强制、STOP 没有：STOP 是**单向**报文（没有应答），
// 它的"步已结算"说的是"命令发出去了"，程序不会因此宣称任何与实际不符的事；
// 而 HOME 有应答、且应答的含义被明确定义成"开始"。同一件事不能有两种说法。
SequenceError validateSequence(const SequenceProgram& program,
                               std::uint8_t initialMode) noexcept;

// 一步的结局。**只有 Acked 才是成功**；剩下四种都会让整部程序停下。
enum class StepOutcome : std::uint8_t {
    Acked = 0U,      // 设备确认了
    Nacked = 1U,     // 设备明确拒绝（状态非法 / 参数不合法）
    TimedOut = 2U,   // 等超时了，结局未知（**不会自动重发**）
    NotSent = 3U,    // 本地就没发出去（未连接 / 会话未就绪 / 门禁拦下）
    Waiting = 4U,    // 还没有结局（内部用，不会出现在报告里）
};

const char* stepOutcomeName(StepOutcome outcome) noexcept;

enum class SequencePhase : std::uint8_t {
    Idle = 0U,       // 没有装载，或者装载了但还没启动
    Running,         // 正在跑
    Succeeded,       // 全部步都拿到了"已确认"
    Failed,          // 某一步的结局不是"已确认"，停在它上面
    Aborted,         // 人工/宿主中止
};

const char* sequencePhaseName(SequencePhase phase) noexcept;

// 一步跑完之后留下来的一条流水。它既是给人看的，也是用例的判据来源 ——
// "哪一步、什么结局、为什么"这三件事必须能逐条查出来。
struct StepReport {
    std::size_t index = 0U;
    SequenceStepKind kind = SequenceStepKind::Wait;
    StepOutcome outcome = StepOutcome::Acked;
    std::string detail;
};

struct SequenceStatus {
    SequencePhase phase = SequencePhase::Idle;
    // 当前（或最后碰过的）步下标。终态时它就是失败/中止的那一步。
    std::size_t stepIndex = 0U;
    std::size_t completedSteps = 0U;
    // 有一条命令在飞、等它的结局。
    bool waitingOutcome = false;
    // Wait 步的到点时刻（会话时刻）。0 = 当前没有在等时间。
    std::uint64_t waitUntilMs = 0U;
    std::string detail;
};

// ---------------------------------------------------------------------------
// 引擎。宿主每拍调一次 tick()，并在中间按需要报告两件事：
//   · 本拍要发的那一步发出去了没有（reportDispatched）；
//   · 上一条命令的结局（reportOutcome）。
// 除 tick 之外没有任何"自己找时间"的东西 —— 所有推进都由调用方的节拍驱动，
// 这样它才可能被确定性地测。
// ---------------------------------------------------------------------------
class Sequencer {
public:
    Sequencer() = default;

    // 装载。不合法就**整份拒掉**（原因在 error()），且不会把原有状态改坏 ——
    // 半份程序比没有程序更危险：它看起来是能跑的。
    bool load(SequenceProgram program, std::uint8_t initialMode) noexcept;
    bool loaded() const noexcept { return loaded_; }
    const SequenceProgram& program() const noexcept { return program_; }
    SequenceError loadError() const noexcept { return loadError_; }
    const std::string& loadErrorDetail() const noexcept { return loadErrorDetail_; }

    // 启动。返回 false = 没启动（没装载 / 已经有一部在跑）。原因在 status().detail。
    // ⚠️ 它**不管门禁**（报警未确认、设备状态不对、会话没就绪）—— 那些只有宿主知道。
    // 宿主该在调它之前判完；判不过就别调，并把它自己的理由说给操作员听。
    bool start(std::uint64_t nowMs);
    // 中止。只能由人发起，或宿主发现链路断了 / 门禁跳了。不存在"自动中止后自动重来"。
    void abort(std::string reason);

    // 当前这一步是不是一条**还没发出去的设备命令**（Wait 不在此列：
    // 它不产生任何下行报文，只由时间推进）。
    bool awaitingDispatch() const noexcept;
    // 当前步（仅当 phase == Running）。调用方必须在 awaitingDispatch() 为真时用它。
    const SequenceStep& currentStep() const noexcept { return program_.steps[currentStepIndex_]; }

    // 报告：当前那一步发出去了没有。sent=false 时 detail 是没发出去的原因。
    void reportDispatched(bool sent, std::string detail);
    // 报告：上一条命令的结局。
    void reportOutcome(StepOutcome outcome, std::string detail);
    // 报告：设备现在是不是停在 Ready（输出未使能、不运动中）。WaitReady 步靠它推进；
    // 引擎自己**看不见设备**，所以这件事只能由宿主告诉它。
    void reportDeviceReady(bool ready) noexcept;

    // 推进（Wait / WaitReady 步靠它到点；其余步靠 reportOutcome 推进）。
    void tick(std::uint64_t nowMs);

    const SequenceStatus& status() const noexcept { return status_; }
    const std::vector<StepReport>& reports() const noexcept { return reports_; }

private:
    // 落到一个终态。**终态只能进一次**：先到的原因才是真原因，
    // 后面再来的（例如同一条命令的超时与链路的失效）不该把它改写成别的事。
    void finish(SequencePhase phase, StepOutcome outcome, std::string detail);
    // 当前步完成，进入下一步。没有下一步了就是成功。
    void advance();
    // 把当前步摆成"等宿主发出去"（Wait 步则摆成"等时间到"）。
    void armCurrentStep();

    SequenceProgram program_;
    bool loaded_ = false;
    SequenceError loadError_ = SequenceError::None;
    std::string loadErrorDetail_;

    SequenceStatus status_;
    std::size_t currentStepIndex_ = 0U;
    bool awaitingDispatch_ = false;
    // 最近一次被告知的时刻。Wait / WaitReady 步要用它算到点时刻 —— 引擎没有自己的钟，
    // 钟在调用方那里（与设备侧的仿真时钟同一个口径）。
    std::uint64_t nowMs_ = 0U;
    // 最近一次被告知的"设备是不是停在 Ready"。
    bool deviceReady_ = false;
    std::vector<StepReport> reports_;
};

}  // namespace mc
