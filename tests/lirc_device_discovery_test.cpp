#include "radio/lirc_device_discovery.hpp"

#include <cstdlib>
#include <iostream>
#include <linux/lirc.h>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testCardputerZeroEnumerationOrder()
{
    const std::vector<ir_chat::radio::LircDeviceCandidate> candidates{
        {"/sys/class/rc/rc2", "/dev/lirc0", "gpio-ir-tx", 0x00000302},
        {"/sys/class/rc/rc1", "/dev/lirc1", "gpio_ir_recv", 0x10040000},
    };

    const auto receiver    = ir_chat::radio::selectLircDevice(candidates, ir_chat::radio::LircDeviceRole::Receiver);
    const auto transmitter = ir_chat::radio::selectLircDevice(candidates, ir_chat::radio::LircDeviceRole::Transmitter);
    require(receiver && receiver->device_path == "/dev/lirc1", "receiver must be selected by MODE2 capability");
    require(transmitter && transmitter->device_path == "/dev/lirc0",
            "transmitter must be selected by PULSE capability");
}

void testNodeNumberDoesNotDefineRole()
{
    const std::vector<ir_chat::radio::LircDeviceCandidate> candidates{
        {"/sys/class/rc/rc8", "/dev/lirc8", "receiver", LIRC_CAN_REC_MODE2},
        {"/sys/class/rc/rc3", "/dev/lirc3", "transmitter", LIRC_CAN_SEND_PULSE},
    };

    const auto receiver    = ir_chat::radio::selectLircDevice(candidates, ir_chat::radio::LircDeviceRole::Receiver);
    const auto transmitter = ir_chat::radio::selectLircDevice(candidates, ir_chat::radio::LircDeviceRole::Transmitter);
    require(receiver && receiver->device_path == "/dev/lirc8", "receiver selection must ignore node number");
    require(transmitter && transmitter->device_path == "/dev/lirc3", "transmitter selection must ignore node number");
}

void testUnsupportedCandidatesAreRejected()
{
    const std::vector<ir_chat::radio::LircDeviceCandidate> candidates{
        {"/sys/class/rc/rc0", "/dev/lirc0", "cec", LIRC_CAN_REC_SCANCODE},
        {"/sys/class/rc/rc1", "/dev/lirc1", "unknown", 0},
    };

    require(!ir_chat::radio::selectLircDevice(candidates, ir_chat::radio::LircDeviceRole::Receiver),
            "SCANCODE-only device cannot receive chat waveforms");
    require(!ir_chat::radio::selectLircDevice(candidates, ir_chat::radio::LircDeviceRole::Transmitter),
            "device without PULSE capability cannot transmit chat waveforms");
}

}  // namespace

int main()
{
    testCardputerZeroEnumerationOrder();
    testNodeNumberDoesNotDefineRole();
    testUnsupportedCandidatesAreRejected();
    std::cout << "LIRC device discovery tests passed\n";
    return 0;
}
