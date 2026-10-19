#include <cstddef>
#include <cstdint>

#include "board.hpp"
#include "mc_fw/device.hpp"
#include "mc_fw/frame_io.hpp"

int main(void) {
    mcfw::board::init();

    // 真机反馈：N20 编码器 7 PPR × 4 倍频 × 1:100 = 2800 计数/输出轴转。
    // 方向系数先取 +1；若正转时位置递减，把它改成 -1（等价于交换 C1/C2）。
    constexpr float kEncoderCountsPerRev = 2800.0F;
    constexpr float kEncoderDirection = 1.0F;

    mcfw::Config config;
    config.encoderCountsPerRev = kEncoderCountsPerRev;
    config.externalFeedback = true;
    mcfw::Device device(config);
    mcfw::FrameParser parser;
    std::uint32_t lastTick = mcfw::board::millis();

    std::uint16_t lastCount = mcfw::board::encoderCount();
    std::int32_t totalCounts = 0;

    for (;;) {
        std::uint8_t incoming = 0U;
        while (mcfw::board::readByte(incoming)) {
            mcfw::ParsedFrame frame;
            if (parser.push(incoming, frame)) {
                device.handleFrame(frame, mcfw::board::millis());
            }
        }

        const std::uint32_t now = mcfw::board::millis();
        if (now != lastTick) {
            const std::uint16_t count = mcfw::board::encoderCount();
            const std::uint16_t wrapped = static_cast<std::uint16_t>(count - lastCount);
            totalCounts += static_cast<std::int16_t>(wrapped);
            lastCount = count;

            const float deg = kEncoderDirection * static_cast<float>(totalCounts) * 360.0F /
                              kEncoderCountsPerRev;
            device.setExternalPositionDeg(deg);
            device.tick(now);
            lastTick = now;
        }

        std::uint8_t outgoing[mcfw::kMaxFrameBytes]{};
        std::size_t written = 0U;
        while ((written = device.popFrame(outgoing, sizeof(outgoing))) > 0U) {
            mcfw::board::write(outgoing, written);
        }

        const mc::DeviceState state = device.state();
        bool led = false;
        if (state == mc::DeviceState::Fault) {
            led = true;
        } else if (state == mc::DeviceState::Running || state == mc::DeviceState::Home) {
            led = ((now / 100U) % 2U) == 0U;
        } else if (state == mc::DeviceState::Ready) {
            led = ((now / 500U) % 2U) == 0U;
        }
        mcfw::board::setLed(led);

        const bool enabled = state == mc::DeviceState::Running || state == mc::DeviceState::Home;
        // 方向取指令符号：测速符号在起转前恒为 0，负目标会被误判成正向。
        mcfw::board::setMotor(device.pwmOutput(), device.command() >= 0.0F, enabled);
    }
}
