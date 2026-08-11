#pragma once

#include "radio/radio_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ir_chat::radio {

inline constexpr uint32_t kIrChatCarrierHz      = 38000;
inline constexpr uint32_t kIrChatDutyCycle      = 33;
inline constexpr uint32_t kIrChatLeaderMarkUs   = 9000;
inline constexpr uint32_t kIrChatLeaderSpaceUs  = 4500;
inline constexpr uint32_t kIrChatBitMarkUs      = 560;
inline constexpr uint32_t kIrChatZeroSpaceUs    = 560;
inline constexpr uint32_t kIrChatOneSpaceUs     = 1690;
inline constexpr uint8_t kIrChatProtocolVersion = 1;

struct IrPulse {
    bool pulse           = false;
    uint32_t duration_us = 0;
};

struct IrChatFrame {
    uint16_t sequence = 0;
    std::vector<uint8_t> payload;
    std::size_t sample_count = 0;
};

struct IrChatDecoderStats {
    uint64_t decoded_frames   = 0;
    uint64_t crc_errors       = 0;
    uint64_t malformed_frames = 0;
    uint64_t noise_samples    = 0;
};

uint16_t irChatCrc16(const uint8_t* data, std::size_t size);
std::vector<uint32_t> encodeIrChatFrame(uint16_t sequence, const std::vector<uint8_t>& payload);

class IrChatStreamDecoder {
public:
    std::optional<IrChatFrame> feed(IrPulse sample);
    void reset() noexcept;

    const IrChatDecoderStats& stats() const noexcept
    {
        return _stats;
    }

private:
    enum class State {
        SeekingLeader,
        LeaderSpace,
        BitMark,
        BitSpace,
        TrailerMark,
    };

    State _state = State::SeekingLeader;
    std::vector<uint8_t> _bytes;
    uint8_t _current_byte         = 0;
    uint8_t _bits_in_current_byte = 0;
    std::size_t _expected_bytes   = 0;
    std::size_t _frame_samples    = 0;
    std::optional<IrChatFrame> _pending_frame;
    IrChatDecoderStats _stats;

    void beginFrame() noexcept;
    void rejectFrame(bool crc_error) noexcept;
    bool consumeBit(bool one) noexcept;
    void restartFrom(IrPulse sample) noexcept;
};

}  // namespace ir_chat::radio
