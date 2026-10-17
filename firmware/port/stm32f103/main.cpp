#include <cstddef>
#include <cstdint>

#include "board.hpp"
#include "mc_fw/device.hpp"
#include "mc_fw/frame_io.hpp"

int main(void) {
    mcfw::board::init();

    mcfw::Device device;
    mcfw::FrameParser parser;
    std::uint32_t lastTick = mcfw::board::millis();

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
        mcfw::board::setMotor(device.pwmOutput(), device.velocityDegPerSec() >= 0.0F, enabled);
    }
}
