#include "radio/radio_backend.hpp"
#include "radio/ir_chat_codec.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace ir_chat::radio {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t monotonicMilliseconds()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

long envLong(const char* name, long fallback, long minimum, long maximum)
{
    const char* text = std::getenv(name);
    if (!text || text[0] == '\0') {
        return fallback;
    }

    char* end        = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!end || end == text || *end != '\0') {
        return fallback;
    }
    return std::clamp(value, minimum, maximum);
}

bool envBool(const char* name, bool fallback)
{
    const char* text = std::getenv(name);
    if (!text || text[0] == '\0') {
        return fallback;
    }
    const std::string value(text);
    return value != "0" && value != "false" && value != "off" && value != "no";
}

void cancellableSleepUntil(Clock::time_point deadline, const CancellationToken& cancellation)
{
    while (Clock::now() < deadline) {
        cancellation.throwIfCancellationRequested();
        const auto remaining = deadline - Clock::now();
        const auto slice =
            std::min(remaining, std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(10)));
        if (slice > Clock::duration::zero()) {
            std::this_thread::sleep_for(slice);
        }
    }
    cancellation.throwIfCancellationRequested();
}

class MockRadioBackend final : public RadioBackend {
public:
    RadioInfo open(const CancellationToken& cancellation) override
    {
        close();
        cancellableSleepUntil(Clock::now() + std::chrono::milliseconds(250), cancellation);
        if (envBool("IR_CHAT_MOCK_INIT_FAIL", false)) {
            throw std::runtime_error("mock initialization failure requested by environment");
        }

        _rx_interval           = std::chrono::milliseconds(envLong("IR_CHAT_MOCK_RX_INTERVAL_MS", 1600, 50, 60000));
        _loopback              = envBool("IR_CHAT_MOCK_LOOPBACK", true);
        _generate_samples      = envBool("IR_CHAT_MOCK_GENERATE", true);
        _open                  = true;
        _next_generated_packet = Clock::now() + _rx_interval;

        RadioInfo info;
        info.backend_name       = "Mock IR";
        info.mock               = true;
        info.rx_device          = "mock://rx";
        info.tx_device          = "mock://tx";
        info.rx_driver          = "loopback";
        info.tx_driver          = "loopback";
        info.carrier_hz         = kIrChatCarrierHz;
        info.duty_cycle_percent = kIrChatDutyCycle;
        info.protocol_name      = "IR Chat v1 pulse-distance";
        return info;
    }

    void close() noexcept override
    {
        _open      = false;
        _receiving = false;
        _pending_loopback.reset();
    }

    void startReceive(const CancellationToken& cancellation) override
    {
        cancellation.throwIfCancellationRequested();
        requireOpen();
        _receiving = true;
        if (_next_generated_packet < Clock::now()) {
            _next_generated_packet = Clock::now() + _rx_interval;
        }
    }

    void stopReceive() override
    {
        _receiving = false;
    }

    bool receive(RadioPacket& packet, std::chrono::milliseconds timeout, const CancellationToken& cancellation) override
    {
        cancellation.throwIfCancellationRequested();
        requireOpen();
        if (!_receiving) {
            return false;
        }

        const auto deadline = Clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
        while (true) {
            cancellation.throwIfCancellationRequested();
            const auto now = Clock::now();
            if (_pending_loopback && now >= _pending_loopback->ready_at) {
                fillPacket(packet, std::move(_pending_loopback->payload), _pending_loopback->protocol_sequence,
                           _pending_loopback->sample_count);
                _pending_loopback.reset();
                return true;
            }
            if (_generate_samples && now >= _next_generated_packet) {
                const std::string text = "mock-" + std::to_string(_next_sequence + 1);
                fillPacket(packet, std::vector<uint8_t>(text.begin(), text.end()), ++_next_protocol_sequence);
                _next_generated_packet = now + _rx_interval;
                return true;
            }
            if (now >= deadline) {
                return false;
            }

            auto wake = _generate_samples ? std::min(deadline, _next_generated_packet) : deadline;
            if (_pending_loopback) {
                wake = std::min(wake, _pending_loopback->ready_at);
            }
            cancellableSleepUntil(std::min(wake, now + std::chrono::milliseconds(10)), cancellation);
        }
    }

    void transmit(const std::vector<uint8_t>& payload, const CancellationToken& cancellation) override
    {
        cancellation.throwIfCancellationRequested();
        requireOpen();
        if (payload.empty()) {
            throw std::runtime_error("payload is empty");
        }
        if (payload.size() > kMaxPayloadSize) {
            throw std::runtime_error("payload exceeds the " + std::to_string(kMaxPayloadSize) +
                                     "-byte transmit-safe limit");
        }
        if (envBool("IR_CHAT_MOCK_TX_FAIL", false)) {
            throw std::runtime_error("mock transmit failure requested by environment");
        }

        const uint16_t protocol_sequence = ++_next_protocol_sequence;
        const auto durations             = encodeIrChatFrame(protocol_sequence, payload);
        uint64_t airtime_us              = 0;
        for (const uint32_t duration : durations) {
            airtime_us += duration;
        }
        const auto airtime_ms = std::chrono::milliseconds((airtime_us + 999U) / 1000U);
        cancellableSleepUntil(Clock::now() + airtime_ms, cancellation);
        if (_loopback) {
            _pending_loopback = PendingLoopback{Clock::now() + std::chrono::milliseconds(100), payload,
                                                protocol_sequence, durations.size()};
        }
    }

private:
    struct PendingLoopback {
        Clock::time_point ready_at;
        std::vector<uint8_t> payload;
        uint16_t protocol_sequence = 0;
        std::size_t sample_count   = 0;
    };

    bool _open                       = false;
    bool _receiving                  = false;
    bool _loopback                   = true;
    bool _generate_samples           = true;
    uint64_t _next_sequence          = 0;
    uint16_t _next_protocol_sequence = 0;
    std::chrono::milliseconds _rx_interval{1600};
    Clock::time_point _next_generated_packet{};
    std::optional<PendingLoopback> _pending_loopback;

    void requireOpen() const
    {
        if (!_open) {
            throw std::runtime_error("mock radio is not open");
        }
    }

    void fillPacket(RadioPacket& packet, std::vector<uint8_t> payload, uint16_t protocol_sequence,
                    std::size_t sample_count = 0)
    {
        ++_next_sequence;
        packet.sequence          = _next_sequence;
        packet.timestamp_ms      = monotonicMilliseconds();
        packet.data              = std::move(payload);
        packet.protocol_sequence = protocol_sequence;
        packet.raw_event_count   = sample_count;
        packet.crc_ok            = true;
    }
};

}  // namespace

std::unique_ptr<RadioBackend> makeMockRadioBackend()
{
    return std::make_unique<MockRadioBackend>();
}

}  // namespace ir_chat::radio
