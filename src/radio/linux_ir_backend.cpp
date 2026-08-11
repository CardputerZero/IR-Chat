#include "radio/radio_backend.hpp"

#include "radio/ir_chat_codec.hpp"
#include "radio/lirc_device_discovery.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <linux/lirc.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ir_chat::radio {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t monotonicMilliseconds()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

std::string errnoMessage(const std::string& action, int error_number)
{
    return action + ": " + std::strerror(error_number) + " (errno=" + std::to_string(error_number) + ")";
}

uint32_t readFeatures(int fd, const std::string& device)
{
    uint32_t features = 0;
    if (::ioctl(fd, LIRC_GET_FEATURES, &features) != 0) {
        const int error_number = errno;
        throw std::runtime_error(errnoMessage("LIRC_GET_FEATURES failed for " + device, error_number));
    }
    return features;
}

void setRequiredIoctl(int fd, unsigned long request, uint32_t value, const std::string& action)
{
    if (::ioctl(fd, request, &value) != 0) {
        const int error_number = errno;
        throw std::runtime_error(errnoMessage(action, error_number));
    }
}

bool setOptionalIoctl(int fd, unsigned long request, uint32_t value, const std::string& action)
{
    if (::ioctl(fd, request, &value) == 0) {
        return true;
    }
    const int error_number = errno;
    spdlog::warn("IR Chat backend: {}: {} (errno={})", action, std::strerror(error_number), error_number);
    return false;
}

int timeoutMilliseconds(std::chrono::milliseconds timeout)
{
    const auto value = std::max(timeout, std::chrono::milliseconds::zero()).count();
    return static_cast<int>(std::min<int64_t>(value, std::numeric_limits<int>::max()));
}

class LinuxIrBackend final : public RadioBackend {
public:
    ~LinuxIrBackend() override
    {
        close();
    }

    RadioInfo open(const CancellationToken& cancellation) override
    {
        close();
        cancellation.throwIfCancellationRequested();

        spdlog::info("IR Chat backend: discovering capability-based LIRC RX/TX nodes");
        const auto receiver =
            discoverLircDevice(LircDeviceRole::Receiver, "IR_CHAT_LIRC_RX_DEVICE", "IR_CHAT_LIRC_RX_RC");
        cancellation.throwIfCancellationRequested();
        const auto transmitter =
            discoverLircDevice(LircDeviceRole::Transmitter, "IR_CHAT_LIRC_TX_DEVICE", "IR_CHAT_LIRC_TX_RC");
        cancellation.throwIfCancellationRequested();

        try {
            openReceiver(receiver);
            cancellation.throwIfCancellationRequested();
            openTransmitter(transmitter);
            cancellation.throwIfCancellationRequested();
        } catch (...) {
            close();
            throw;
        }

        _rx_candidate = receiver;
        _tx_candidate = transmitter;
        _open         = true;

        RadioInfo info;
        info.backend_name       = "LIRC IR";
        info.rx_device          = receiver.device_path;
        info.tx_device          = transmitter.device_path;
        info.rx_driver          = receiver.driver;
        info.tx_driver          = transmitter.driver;
        info.rx_features        = _rx_features;
        info.tx_features        = _tx_features;
        info.carrier_hz         = kIrChatCarrierHz;
        info.duty_cycle_percent = kIrChatDutyCycle;
        info.protocol_name      = "IR Chat v1 pulse-distance";

        spdlog::info(
            "IR Chat backend: open complete (RX={} driver={} features={}, TX={} driver={} features={}, carrier={} "
            "Hz, duty={}%)",
            info.rx_device, info.rx_driver.empty() ? "unknown" : info.rx_driver, formatLircFeatures(info.rx_features),
            info.tx_device, info.tx_driver.empty() ? "unknown" : info.tx_driver, formatLircFeatures(info.tx_features),
            info.carrier_hz, info.duty_cycle_percent);
        return info;
    }

    void close() noexcept override
    {
        _receiving = false;
        _decoder.reset();
        _decoded_packets.clear();
        closeFd(_rx_fd);
        closeFd(_tx_fd);
        _rx_features = 0;
        _tx_features = 0;
        _open        = false;
    }

    void startReceive(const CancellationToken& cancellation) override
    {
        cancellation.throwIfCancellationRequested();
        requireOpen();
        drainReceiver();
        _decoder.reset();
        _decoded_packets.clear();
        _receiving = true;
        spdlog::debug("IR Chat backend: receive active on {}", _rx_candidate.device_path);
    }

    void stopReceive() override
    {
        _receiving = false;
        _decoder.reset();
        _decoded_packets.clear();
    }

    bool receive(RadioPacket& packet, std::chrono::milliseconds timeout, const CancellationToken& cancellation) override
    {
        cancellation.throwIfCancellationRequested();
        requireOpen();
        if (!_receiving) {
            return false;
        }
        if (popDecodedPacket(packet)) {
            return true;
        }

        pollfd descriptor{_rx_fd, POLLIN, 0};
        int result;
        do {
            result = ::poll(&descriptor, 1, timeoutMilliseconds(timeout));
        } while (result < 0 && errno == EINTR && !cancellation.stopRequested());
        cancellation.throwIfCancellationRequested();

        if (result < 0) {
            const int error_number = errno;
            throw std::runtime_error(errnoMessage("LIRC receiver poll failed", error_number));
        }
        if (result == 0) {
            return false;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("LIRC receiver poll reported revents=" +
                                     std::to_string(static_cast<unsigned>(descriptor.revents)));
        }
        if ((descriptor.revents & POLLIN) == 0) {
            return false;
        }

        readAvailableEvents();
        cancellation.throwIfCancellationRequested();
        return popDecodedPacket(packet);
    }

    void transmit(const std::vector<uint8_t>& payload, const CancellationToken& cancellation) override
    {
        cancellation.throwIfCancellationRequested();
        requireOpen();

        const uint16_t protocol_sequence = ++_next_tx_protocol_sequence;
        const auto encoded               = encodeIrChatFrame(protocol_sequence, payload);
        std::vector<lirc_t> durations(encoded.begin(), encoded.end());

        uint64_t airtime_us = 0;
        for (const lirc_t duration : durations) {
            airtime_us += duration;
        }
        spdlog::info("IR Chat backend: transmitting sequence={} bytes={} events={} airtime={} ms via {}",
                     protocol_sequence, payload.size(), durations.size(), (airtime_us + 999U) / 1000U,
                     _tx_candidate.device_path);

        const std::size_t byte_count = durations.size() * sizeof(lirc_t);
        ssize_t written;
        do {
            written = ::write(_tx_fd, durations.data(), byte_count);
        } while (written < 0 && errno == EINTR && !cancellation.stopRequested());
        cancellation.throwIfCancellationRequested();

        if (written < 0) {
            const int error_number = errno;
            throw std::runtime_error(
                errnoMessage("LIRC transmit failed on " + _tx_candidate.device_path, error_number));
        }
        if (written != static_cast<ssize_t>(byte_count)) {
            throw std::runtime_error("LIRC transmit short write on " + _tx_candidate.device_path + ": " +
                                     std::to_string(written) + "/" + std::to_string(byte_count) + " bytes");
        }

        ++_transmitted_frames;
        spdlog::info("IR Chat backend: transmit complete sequence={} total_tx={}", protocol_sequence,
                     _transmitted_frames);
    }

private:
    bool _open            = false;
    bool _receiving       = false;
    int _rx_fd            = -1;
    int _tx_fd            = -1;
    uint32_t _rx_features = 0;
    uint32_t _tx_features = 0;
    LircDeviceCandidate _rx_candidate;
    LircDeviceCandidate _tx_candidate;
    IrChatStreamDecoder _decoder;
    std::deque<RadioPacket> _decoded_packets;
    uint64_t _received_frames           = 0;
    uint64_t _transmitted_frames        = 0;
    uint64_t _reported_rejected_frames  = 0;
    uint16_t _next_tx_protocol_sequence = 0;

    static void closeFd(int& fd) noexcept
    {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    void requireOpen() const
    {
        if (!_open || _rx_fd < 0 || _tx_fd < 0) {
            throw std::runtime_error("IR backend is not open");
        }
    }

    void openReceiver(const LircDeviceCandidate& receiver)
    {
        _rx_fd = ::open(receiver.device_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (_rx_fd < 0) {
            const int error_number = errno;
            throw std::runtime_error(errnoMessage("failed to open IR receiver " + receiver.device_path, error_number));
        }

        _rx_features = readFeatures(_rx_fd, receiver.device_path);
        if (!lircDeviceSupportsRole(_rx_features, LircDeviceRole::Receiver)) {
            throw std::runtime_error("IR receiver " + receiver.device_path + " lost LIRC_CAN_REC_MODE2 capability");
        }
        setRequiredIoctl(_rx_fd, LIRC_SET_REC_MODE, LIRC_MODE_MODE2,
                         "LIRC_SET_REC_MODE failed for " + receiver.device_path);

        if ((_rx_features & LIRC_CAN_SET_REC_TIMEOUT) != 0) {
            constexpr uint32_t kReceiveTimeoutUs = 100000;
            if (setOptionalIoctl(_rx_fd, LIRC_SET_REC_TIMEOUT, kReceiveTimeoutUs,
                                 "LIRC_SET_REC_TIMEOUT failed for " + receiver.device_path)) {
                setOptionalIoctl(_rx_fd, LIRC_SET_REC_TIMEOUT_REPORTS, 1,
                                 "LIRC_SET_REC_TIMEOUT_REPORTS failed for " + receiver.device_path);
            }
        }
        spdlog::info("IR Chat backend: RX open device={} rc={} driver={} features={} mode=MODE2", receiver.device_path,
                     receiver.rc_path.empty() ? "unmapped" : receiver.rc_path,
                     receiver.driver.empty() ? "unknown" : receiver.driver, formatLircFeatures(_rx_features));
    }

    void openTransmitter(const LircDeviceCandidate& transmitter)
    {
        _tx_fd = ::open(transmitter.device_path.c_str(), O_WRONLY | O_CLOEXEC);
        if (_tx_fd < 0) {
            const int error_number = errno;
            throw std::runtime_error(
                errnoMessage("failed to open IR transmitter " + transmitter.device_path, error_number));
        }

        _tx_features = readFeatures(_tx_fd, transmitter.device_path);
        if (!lircDeviceSupportsRole(_tx_features, LircDeviceRole::Transmitter)) {
            throw std::runtime_error("IR transmitter " + transmitter.device_path +
                                     " lost LIRC_CAN_SEND_PULSE capability");
        }
        setRequiredIoctl(_tx_fd, LIRC_SET_SEND_MODE, LIRC_MODE_PULSE,
                         "LIRC_SET_SEND_MODE failed for " + transmitter.device_path);

        if ((_tx_features & LIRC_CAN_SET_SEND_CARRIER) != 0) {
            setOptionalIoctl(_tx_fd, LIRC_SET_SEND_CARRIER, kIrChatCarrierHz,
                             "LIRC_SET_SEND_CARRIER failed for " + transmitter.device_path);
        } else {
            spdlog::warn("IR Chat backend: TX {} cannot set carrier; current kernel carrier must be 38 kHz",
                         transmitter.device_path);
        }
        if ((_tx_features & LIRC_CAN_SET_SEND_DUTY_CYCLE) != 0) {
            setOptionalIoctl(_tx_fd, LIRC_SET_SEND_DUTY_CYCLE, kIrChatDutyCycle,
                             "LIRC_SET_SEND_DUTY_CYCLE failed for " + transmitter.device_path);
        }
        spdlog::info("IR Chat backend: TX open device={} rc={} driver={} features={} mode=PULSE",
                     transmitter.device_path, transmitter.rc_path.empty() ? "unmapped" : transmitter.rc_path,
                     transmitter.driver.empty() ? "unknown" : transmitter.driver, formatLircFeatures(_tx_features));
    }

    void drainReceiver() noexcept
    {
        if (_rx_fd < 0) {
            return;
        }

        std::array<lirc_t, 128> values{};
        while (true) {
            const ssize_t bytes = ::read(_rx_fd, values.data(), values.size() * sizeof(lirc_t));
            if (bytes > 0) {
                continue;
            }
            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    void readAvailableEvents()
    {
        std::array<lirc_t, 128> values{};
        while (true) {
            const ssize_t bytes = ::read(_rx_fd, values.data(), values.size() * sizeof(lirc_t));
            if (bytes > 0) {
                if (bytes % static_cast<ssize_t>(sizeof(lirc_t)) != 0) {
                    throw std::runtime_error("LIRC receiver returned an unaligned MODE2 read");
                }
                const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(lirc_t);
                for (std::size_t index = 0; index < count; ++index) {
                    processMode2Event(values[index]);
                }
                continue;
            }
            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            if (bytes == 0) {
                throw std::runtime_error("LIRC receiver reached end of stream");
            }
            const int error_number = errno;
            throw std::runtime_error(errnoMessage("LIRC receiver read failed", error_number));
        }
    }

    void processMode2Event(lirc_t value)
    {
        if (LIRC_IS_TIMEOUT(value)) {
            _decoder.reset();
            return;
        }
        if (LIRC_IS_OVERFLOW(value)) {
            _decoder.reset();
            spdlog::warn("IR Chat backend: LIRC receive overflow; partial frame discarded");
            return;
        }
        if (!LIRC_IS_PULSE(value) && !LIRC_IS_SPACE(value)) {
            return;
        }

        const auto before       = _decoder.stats();
        auto decoded            = _decoder.feed({LIRC_IS_PULSE(value), LIRC_VALUE(value)});
        const auto after        = _decoder.stats();
        const uint64_t rejected = after.crc_errors + after.malformed_frames;
        if (rejected != _reported_rejected_frames) {
            _reported_rejected_frames = rejected;
            if (after.crc_errors != before.crc_errors || rejected == 1 || rejected % 64 == 0) {
                spdlog::debug("IR Chat backend: ignored non-chat/invalid IR frame (malformed={}, CRC={}, noise={})",
                              after.malformed_frames, after.crc_errors, after.noise_samples);
            }
        }
        if (!decoded) {
            return;
        }

        RadioPacket packet;
        packet.sequence          = ++_received_frames;
        packet.timestamp_ms      = monotonicMilliseconds();
        packet.data              = std::move(decoded->payload);
        packet.protocol_sequence = decoded->sequence;
        packet.raw_event_count   = decoded->sample_count;
        packet.crc_ok            = true;
        spdlog::info("IR Chat backend: received sequence={} bytes={} events={} total_rx={}", packet.protocol_sequence,
                     packet.data.size(), packet.raw_event_count, _received_frames);
        _decoded_packets.push_back(std::move(packet));
    }

    bool popDecodedPacket(RadioPacket& packet)
    {
        if (_decoded_packets.empty()) {
            return false;
        }
        packet = std::move(_decoded_packets.front());
        _decoded_packets.pop_front();
        return true;
    }
};

}  // namespace

std::unique_ptr<RadioBackend> makeLinuxIrBackend()
{
    return std::make_unique<LinuxIrBackend>();
}

}  // namespace ir_chat::radio
