#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ir_chat::radio {

// Linux rc-core caps a raw IR transmission at 500 ms. A 23-byte printable
// ASCII payload fits even with the longest possible pulse-distance frame.
inline constexpr std::size_t kMaxPayloadSize = 23;

enum class RadioState {
    Stopped,
    Initializing,
    Idle,
    Receiving,
    Sending,
    Error,
    Stopping,
};

struct RadioInfo {
    std::string backend_name;
    bool mock = false;
    std::string rx_device;
    std::string tx_device;
    std::string rx_driver;
    std::string tx_driver;
    uint32_t rx_features        = 0;
    uint32_t tx_features        = 0;
    uint32_t carrier_hz         = 38000;
    uint32_t duty_cycle_percent = 33;
    std::string protocol_name   = "IR Chat v1";
};

struct RadioPacket {
    uint64_t sequence     = 0;
    uint64_t timestamp_ms = 0;
    std::vector<uint8_t> data;
    uint16_t protocol_sequence  = 0;
    std::size_t raw_event_count = 0;
    bool crc_ok                 = false;
};

class RadioCancelled final : public std::exception {
public:
    const char* what() const noexcept override
    {
        return "radio operation cancelled";
    }
};

class CancellationToken {
public:
    CancellationToken() = default;
    explicit CancellationToken(const std::atomic_bool& cancelled) : _cancelled(&cancelled)
    {
    }

    bool stopRequested() const noexcept
    {
        return _cancelled != nullptr && _cancelled->load(std::memory_order_acquire);
    }

    void throwIfCancellationRequested() const
    {
        if (stopRequested()) {
            throw RadioCancelled{};
        }
    }

    const std::atomic_bool* nativeFlag() const noexcept
    {
        return _cancelled;
    }

private:
    const std::atomic_bool* _cancelled = nullptr;
};

struct RadioRetryCommand {};

struct RadioSetReceiveCommand {
    bool enabled = true;
};

struct RadioSendCommand {
    uint64_t id = 0;
    std::vector<uint8_t> payload;
};

struct RadioShutdownCommand {};

using RadioCommand = std::variant<RadioRetryCommand, RadioSetReceiveCommand, RadioSendCommand, RadioShutdownCommand>;

struct RadioStateEvent {
    RadioState state = RadioState::Stopped;
    std::string detail;
};

struct RadioInitializedEvent {
    RadioInfo info;
};

struct RadioErrorEvent {
    std::string operation;
    std::string message;
};

struct RadioRxPacketEvent {
    RadioPacket packet;
};

struct RadioTxStartedEvent {
    uint64_t id      = 0;
    std::size_t size = 0;
};

struct RadioTxCompletedEvent {
    uint64_t id = 0;
};

struct RadioTxFailedEvent {
    uint64_t id = 0;
    std::string message;
};

struct RadioQueueOverflowEvent {
    std::size_t dropped = 0;
};

using RadioEvent =
    std::variant<RadioStateEvent, RadioInitializedEvent, RadioErrorEvent, RadioRxPacketEvent, RadioTxStartedEvent,
                 RadioTxCompletedEvent, RadioTxFailedEvent, RadioQueueOverflowEvent>;

}  // namespace ir_chat::radio
