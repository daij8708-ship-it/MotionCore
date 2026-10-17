#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// Recipe —— 工艺配方（P0）
//
// 设备软件里"配方"解决的是一件事：**动作参数不写死在代码里**。
// 同一台设备换一种活儿，靠的是一份带名字、带版本、能被校验的参数集，
// 而不是重新烧一版固件或者让人在界面上照着纸条敲七个数字。
//
// 这里的字段**就是下行的三条命令的载荷**（SET_MODE / SET_PID / SET_TARGET），
// 不多一个也不少一个：
//   · 多一个字段就意味着协议里要再开一条消息，而协议是冻结的 v1；
//   · 少一个字段就意味着"配方"根本不足以描述一次完整的设定。
//
// 三条不变式（都由用例钉住，不靠注释自觉）：
//   ① **序列化是确定的**：同一份配方永远得到同一串字节。否则校验和毫无意义 ——
//      校验和只能证明"字节没变"，不能证明"语义没变"。
//   ② **校验和覆盖全部字段**（含名称与版本）。只盖参数的校验和挡不住
//      "改了个名字但校验和没变"，而名字恰恰是操作员用来选配方的依据。
//   ③ **校验分两层**：`decodeRecipe` 只管字节流结构（够不够长、有没有多余），
//      `validateRecipe` 只管数值语义（范围、内部矛盾）。理由与 payload.hpp 相同：
//      NaN 的四个字节是完全合法的 float32，结构上没毛病，但它绝不能进设备。
//
// ⚠️ 一个刻意写进去的"矛盾检查"：模式 0（停止控制）配着一个非零目标值，
//    说明这份配方自己就没想清楚 —— 它要么执行不了，要么执行出别的意思。
//    这类配方在装载时就被拒，而不是等到下装时由设备回一个含糊的拒绝。
// ---------------------------------------------------------------------------

// 配方名的最大**字节**数（UTF-8 字节，不是字符数）。它进了线格式，所以是格式常量。
inline constexpr std::size_t kMaxRecipeNameBytes = 48U;

// 公式：名称长度(2) + 名称 + 版本(2) + 模式(1) + target(4) + kp/ki/kd(12)
inline constexpr std::size_t kRecipeFixedBytes = 21U;
inline constexpr std::size_t kMaxRecipeBytes = kRecipeFixedBytes + kMaxRecipeNameBytes;

struct Recipe {
    std::string name;              // 配方名（UTF-8）。空名不合法 —— 名字是操作员选它的依据。
    std::uint16_t version = 1U;    // 配方版本。**0 不是合法版本**：它只可能是"没填"。
    std::uint8_t mode = 0U;        // 0 停止控制 / 1 速度环 / 2 位置环（与 SetMode 同一套编号）
    float target = 0.0F;           // 速度环 = deg/s；位置环 = deg
    float kp = 0.0F;
    float ki = 0.0F;
    float kd = 0.0F;
};

// 显式错误枚举，不用 bool 或"空返回值"表示失败：失败原因必须可分辨，
// 因为它直接决定操作员下一步该改哪个字段。界面负责把 token 翻译成人话。
enum class RecipeError : std::uint8_t {
    None = 0U,
    // --- 结构层（decodeRecipe）---
    Truncated,        // 字节流不够读完一个字段
    NameTooLong,      // 名称长度超过 kMaxRecipeNameBytes
    TrailingBytes,    // 读完一条配方还剩字节 —— 线格式与该结构不一致
    // --- 语义层（validateRecipe）---
    EmptyName,
    BadVersion,       // version == 0
    BadMode,          // 超出 0..kMaxRunMode
    TargetNotFinite,  // NaN / ±Inf
    TargetOutOfRange, // 超出当前模式对应的量纲上界
    TargetMustBeZero, // 模式 0 却带着非零目标
    GainNotFinite,
    GainNegative,
    GainTooLarge,
};

const char* recipeErrorName(RecipeError error) noexcept;

// --- 线格式 -----------------------------------------------------------------
//
//  偏移            长度  字段
//   0               2    nameLen（小端）
//   2            nameLen 名称字节（UTF-8，无终止符）
//   2+nameLen       2    版本（小端）
//   4+nameLen       1    模式
//   5+nameLen       4    target（float32 小端）
//   9+nameLen       4    kp
//  13+nameLen       4    ki
//  17+nameLen       4    kd
//
// 与 payload 一样**逐字段显式序列化**，不对结构体做 memcpy：
// 结构体里的 padding 是 ABI 细节，把它写进线格式等于让配方文件绑死编译器。
//
// 编码是**全函数**：合法配方总有编码结果，不返回错误码。
// 数值是否合理不属于编码的职责 —— 那是 validateRecipe 的事。
std::vector<std::uint8_t> encodeRecipe(const Recipe& value);

// 只做结构性校验。不足/多余都给得出具体原因，不是笼统的"格式错"。
RecipeError decodeRecipe(const std::uint8_t* data, std::size_t len, Recipe& out) noexcept;

// 只做语义校验。允许直接对"界面刚编辑完、还没编码"的结构体调用。
RecipeError validateRecipe(const Recipe& value) noexcept;

// 校验和 = CRC-16/CCITT-FALSE 覆盖 encodeRecipe() 的**全部字节**。
// 复用协议同一套 CRC：多写一套多项式只会多一个"两边不一致"的机会。
std::uint16_t recipeChecksum(const Recipe& value) noexcept;

}  // namespace mc
