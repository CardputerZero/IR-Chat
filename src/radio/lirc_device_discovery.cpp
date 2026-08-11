#include "radio/lirc_device_discovery.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/lirc.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ir_chat::radio {
namespace {

constexpr const char* kRcClassPath = "/sys/class/rc";
constexpr const char* kDevicePath  = "/dev";

std::string environmentValue(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? value : "";
}

std::string readTextFile(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return "";
    }

    char buffer[512]   = {};
    const size_t bytes = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[bytes] = '\0';
    return buffer;
}

std::string trim(std::string value)
{
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    const auto first = value.find_first_not_of(" \t");
    return first == std::string::npos ? "" : value.substr(first);
}

std::string sysfsKey(const std::string& rc_path, const std::string& key)
{
    const std::string uevent = readTextFile(rc_path + "/uevent");
    std::size_t position     = 0;
    while (position < uevent.size()) {
        const std::size_t end  = uevent.find('\n', position);
        const std::string line = uevent.substr(position, end == std::string::npos ? std::string::npos : end - position);
        position               = end == std::string::npos ? uevent.size() : end + 1;

        const std::string prefix = key + "=";
        if (line.rfind(prefix, 0) == 0) {
            return trim(line.substr(prefix.size()));
        }
    }
    return "";
}

std::string rcDriverName(const std::string& rc_path)
{
    std::string driver = sysfsKey(rc_path, "DRV_NAME");
    if (!driver.empty()) {
        return driver;
    }
    driver = trim(readTextFile(rc_path + "/drv_name"));
    if (!driver.empty()) {
        return driver;
    }
    return trim(readTextFile(rc_path + "/name"));
}

bool isNumberedName(const std::string& name, const char* prefix)
{
    const std::string prefix_text = prefix;
    if (name.size() <= prefix_text.size() || name.rfind(prefix_text, 0) != 0) {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix_text.size()), name.end(),
                       [](char value) { return value >= '0' && value <= '9'; });
}

std::vector<std::string> numberedEntries(const std::string& directory, const char* prefix)
{
    std::vector<std::string> entries;
    DIR* handle = ::opendir(directory.c_str());
    if (!handle) {
        return entries;
    }

    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (isNumberedName(name, prefix)) {
            entries.push_back(name);
        }
    }
    ::closedir(handle);
    std::sort(entries.begin(), entries.end());
    return entries;
}

std::vector<std::string> listRcPaths()
{
    std::vector<std::string> paths;
    for (const auto& name : numberedEntries(kRcClassPath, "rc")) {
        paths.push_back(std::string(kRcClassPath) + "/" + name);
    }
    return paths;
}

std::vector<std::string> listLircDevices(const std::string& rc_path)
{
    std::vector<std::string> devices;
    for (const auto& name : numberedEntries(rc_path, "lirc")) {
        devices.push_back(std::string(kDevicePath) + "/" + name);
    }
    return devices;
}

std::string errnoMessage(const std::string& action, int error_number)
{
    return action + ": " + std::strerror(error_number) + " (errno=" + std::to_string(error_number) + ")";
}

int openLircForProbe(const std::string& device_path)
{
    constexpr std::array<int, 3> kOpenModes{
        O_RDWR | O_NONBLOCK | O_CLOEXEC,
        O_RDONLY | O_NONBLOCK | O_CLOEXEC,
        O_WRONLY | O_NONBLOCK | O_CLOEXEC,
    };
    for (const int flags : kOpenModes) {
        const int fd = ::open(device_path.c_str(), flags);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

uint32_t probeLircFeatures(const std::string& device_path)
{
    const int fd = openLircForProbe(device_path);
    if (fd < 0) {
        const int error_number = errno;
        throw std::runtime_error(errnoMessage("failed to open " + device_path, error_number));
    }

    uint32_t features = 0;
    if (::ioctl(fd, LIRC_GET_FEATURES, &features) != 0) {
        const int error_number = errno;
        ::close(fd);
        throw std::runtime_error(errnoMessage("LIRC_GET_FEATURES failed for " + device_path, error_number));
    }
    ::close(fd);
    return features;
}

LircDeviceCandidate candidateMetadata(const std::string& device_path, const std::vector<std::string>& rc_paths)
{
    for (const auto& rc_path : rc_paths) {
        const auto devices = listLircDevices(rc_path);
        if (std::find(devices.begin(), devices.end(), device_path) != devices.end()) {
            return {rc_path, device_path, rcDriverName(rc_path), 0};
        }
    }
    return {"", device_path, "lirc", 0};
}

void appendCandidate(std::vector<LircDeviceCandidate>& candidates, LircDeviceCandidate candidate)
{
    const auto duplicate = std::find_if(candidates.begin(), candidates.end(), [&candidate](const auto& existing) {
        return existing.device_path == candidate.device_path;
    });
    if (duplicate == candidates.end()) {
        candidates.push_back(std::move(candidate));
    }
}

void probeCandidate(LircDeviceCandidate candidate, std::vector<LircDeviceCandidate>& candidates)
{
    try {
        candidate.features = probeLircFeatures(candidate.device_path);
        spdlog::info("IR Chat LIRC: candidate device={} rc={} driver={} features={}", candidate.device_path,
                     candidate.rc_path.empty() ? "unmapped" : candidate.rc_path,
                     candidate.driver.empty() ? "unknown" : candidate.driver, formatLircFeatures(candidate.features));
        appendCandidate(candidates, std::move(candidate));
    } catch (const std::exception& exception) {
        spdlog::warn("IR Chat LIRC: skipped device={} rc={} error={}", candidate.device_path,
                     candidate.rc_path.empty() ? "unmapped" : candidate.rc_path, exception.what());
    }
}

std::vector<LircDeviceCandidate> probeCandidates(const std::vector<std::string>& search_rc_paths,
                                                 const std::vector<std::string>& all_rc_paths, bool include_unmapped)
{
    std::vector<LircDeviceCandidate> candidates;
    for (const auto& rc_path : search_rc_paths) {
        for (const auto& device_path : listLircDevices(rc_path)) {
            probeCandidate({rc_path, device_path, rcDriverName(rc_path), 0}, candidates);
        }
    }

    if (include_unmapped) {
        for (const auto& name : numberedEntries(kDevicePath, "lirc")) {
            const std::string device_path = std::string(kDevicePath) + "/" + name;
            const bool already_seen =
                std::any_of(candidates.begin(), candidates.end(),
                            [&device_path](const auto& candidate) { return candidate.device_path == device_path; });
            if (!already_seen) {
                probeCandidate(candidateMetadata(device_path, all_rc_paths), candidates);
            }
        }
    }
    return candidates;
}

const char* requiredCapabilityName(LircDeviceRole role)
{
    return role == LircDeviceRole::Receiver ? "LIRC_CAN_REC_MODE2" : "LIRC_CAN_SEND_PULSE";
}

}  // namespace

bool lircDeviceSupportsRole(uint32_t features, LircDeviceRole role)
{
    return role == LircDeviceRole::Receiver ? (features & LIRC_CAN_REC_MODE2) != 0
                                            : (features & LIRC_CAN_SEND_PULSE) != 0;
}

std::optional<LircDeviceCandidate> selectLircDevice(const std::vector<LircDeviceCandidate>& candidates,
                                                    LircDeviceRole role)
{
    const auto match = std::find_if(candidates.begin(), candidates.end(), [role](const auto& candidate) {
        return lircDeviceSupportsRole(candidate.features, role);
    });
    return match == candidates.end() ? std::nullopt : std::optional<LircDeviceCandidate>(*match);
}

LircDeviceCandidate discoverLircDevice(LircDeviceRole role, const char* device_environment, const char* rc_environment)
{
    const std::string explicit_device = environmentValue(device_environment);
    const std::string explicit_rc     = environmentValue(rc_environment);
    const auto all_rc_paths           = listRcPaths();

    if (!explicit_device.empty()) {
        const std::vector<std::string> metadata_paths =
            explicit_rc.empty() ? all_rc_paths : std::vector<std::string>{explicit_rc};
        auto selected     = candidateMetadata(explicit_device, metadata_paths);
        selected.features = probeLircFeatures(explicit_device);
        if (!lircDeviceSupportsRole(selected.features, role)) {
            throw std::runtime_error(std::string(device_environment) + "=" + explicit_device + " has features " +
                                     formatLircFeatures(selected.features) + ", missing " +
                                     requiredCapabilityName(role));
        }
        spdlog::info("IR Chat LIRC: strict {} override selected device={} rc={} driver={} features={}",
                     lircRoleName(role), selected.device_path, selected.rc_path.empty() ? "unmapped" : selected.rc_path,
                     selected.driver.empty() ? "unknown" : selected.driver, formatLircFeatures(selected.features));
        return selected;
    }

    const std::vector<std::string> search_rc_paths =
        explicit_rc.empty() ? all_rc_paths : std::vector<std::string>{explicit_rc};
    const auto candidates = probeCandidates(search_rc_paths, all_rc_paths, explicit_rc.empty());
    const auto selected   = selectLircDevice(candidates, role);
    if (selected) {
        spdlog::info("IR Chat LIRC: selected {} device={} rc={} driver={} features={}", lircRoleName(role),
                     selected->device_path, selected->rc_path.empty() ? "unmapped" : selected->rc_path,
                     selected->driver.empty() ? "unknown" : selected->driver, formatLircFeatures(selected->features));
        return *selected;
    }

    if (!explicit_rc.empty() && candidates.empty()) {
        throw std::runtime_error("no LIRC nodes found under strict " + std::string(rc_environment) + "=" + explicit_rc);
    }
    if (candidates.empty()) {
        throw std::runtime_error("no usable LIRC nodes found; verify the IR RX/TX overlays and device permissions");
    }
    throw std::runtime_error(std::string("no LIRC ") + lircRoleName(role) + " advertises " +
                             requiredCapabilityName(role));
}

std::string formatLircFeatures(uint32_t features)
{
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", features);
    return buffer;
}

const char* lircRoleName(LircDeviceRole role)
{
    return role == LircDeviceRole::Receiver ? "receiver" : "transmitter";
}

}  // namespace ir_chat::radio
