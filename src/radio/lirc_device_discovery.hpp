#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ir_chat::radio {

enum class LircDeviceRole {
    Receiver,
    Transmitter,
};

struct LircDeviceCandidate {
    std::string rc_path;
    std::string device_path;
    std::string driver;
    uint32_t features = 0;
};

bool lircDeviceSupportsRole(uint32_t features, LircDeviceRole role);
std::optional<LircDeviceCandidate> selectLircDevice(const std::vector<LircDeviceCandidate>& candidates,
                                                    LircDeviceRole role);

LircDeviceCandidate discoverLircDevice(LircDeviceRole role, const char* device_environment, const char* rc_environment);
std::string formatLircFeatures(uint32_t features);
const char* lircRoleName(LircDeviceRole role);

}  // namespace ir_chat::radio
