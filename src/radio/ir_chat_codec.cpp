#include "radio/ir_chat_codec.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace ir_chat::radio {
namespace {

constexpr std::array<uint8_t, 2> kMagic{'I', 'R'};
constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kCrcSize    = 2;

bool inRange(uint32_t value, uint32_t minimum, uint32_t maximum)
{
    return value >= minimum && value <= maximum;
}

bool isLeaderMark(uint32_t duration)
{
    return inRange(duration, 6300, 11700);
}

bool isLeaderSpace(uint32_t duration)
{
    return inRange(duration, 3150, 5850);
}

bool isBitMark(uint32_t duration)
{
    return inRange(duration, 300, 850);
}

std::optional<bool> decodeBitSpace(uint32_t duration)
{
    if (inRange(duration, 300, 950)) {
        return false;
    }
    if (inRange(duration, 1100, 2400)) {
        return true;
    }
    return std::nullopt;
}

bool isPrintablePayload(const std::vector<uint8_t>& payload)
{
    return std::all_of(payload.begin(), payload.end(), [](uint8_t value) { return value >= 0x20 && value <= 0x7e; });
}

void appendByteBits(std::vector<uint32_t>& durations, uint8_t value)
{
    for (int bit = 7; bit >= 0; --bit) {
        durations.push_back(kIrChatBitMarkUs);
        durations.push_back((value & (1U << bit)) != 0 ? kIrChatOneSpaceUs : kIrChatZeroSpaceUs);
    }
}

}  // namespace

uint16_t irChatCrc16(const uint8_t* data, std::size_t size)
{
    uint16_t crc = 0xffff;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<uint16_t>(data[index]) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc =
                (crc & 0x8000U) != 0 ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U) : static_cast<uint16_t>(crc << 1U);
        }
    }
    return crc;
}

std::vector<uint32_t> encodeIrChatFrame(uint16_t sequence, const std::vector<uint8_t>& payload)
{
    if (payload.empty() || payload.size() > kMaxPayloadSize) {
        throw std::invalid_argument("IR Chat payload must contain 1 to " + std::to_string(kMaxPayloadSize) + " bytes");
    }
    if (!isPrintablePayload(payload)) {
        throw std::invalid_argument("IR Chat payload must contain printable ASCII bytes");
    }

    std::vector<uint8_t> frame;
    frame.reserve(kHeaderSize + payload.size() + kCrcSize);
    frame.insert(frame.end(), kMagic.begin(), kMagic.end());
    frame.push_back(kIrChatProtocolVersion);
    frame.push_back(static_cast<uint8_t>(sequence >> 8U));
    frame.push_back(static_cast<uint8_t>(sequence & 0xffU));
    frame.push_back(static_cast<uint8_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    const uint16_t crc = irChatCrc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc >> 8U));
    frame.push_back(static_cast<uint8_t>(crc & 0xffU));

    std::vector<uint32_t> durations;
    durations.reserve(3 + frame.size() * 16);
    durations.push_back(kIrChatLeaderMarkUs);
    durations.push_back(kIrChatLeaderSpaceUs);
    for (const uint8_t value : frame) {
        appendByteBits(durations, value);
    }
    durations.push_back(kIrChatBitMarkUs);
    return durations;
}

std::optional<IrChatFrame> IrChatStreamDecoder::feed(IrPulse sample)
{
    if (sample.duration_us == 0) {
        ++_stats.noise_samples;
        return std::nullopt;
    }

    switch (_state) {
        case State::SeekingLeader:
            if (sample.pulse && isLeaderMark(sample.duration_us)) {
                beginFrame();
                _state = State::LeaderSpace;
            } else {
                ++_stats.noise_samples;
            }
            break;

        case State::LeaderSpace:
            ++_frame_samples;
            if (!sample.pulse && isLeaderSpace(sample.duration_us)) {
                _state = State::BitMark;
            } else {
                rejectFrame(false);
                restartFrom(sample);
            }
            break;

        case State::BitMark:
            ++_frame_samples;
            if (sample.pulse && isBitMark(sample.duration_us)) {
                _state = State::BitSpace;
            } else {
                rejectFrame(false);
                restartFrom(sample);
            }
            break;

        case State::BitSpace: {
            ++_frame_samples;
            if (sample.pulse) {
                rejectFrame(false);
                restartFrom(sample);
                break;
            }
            const auto bit = decodeBitSpace(sample.duration_us);
            if (!bit) {
                rejectFrame(false);
                break;
            }
            if (!consumeBit(*bit)) {
                break;
            }
            _state = _pending_frame ? State::TrailerMark : State::BitMark;
            break;
        }

        case State::TrailerMark:
            ++_frame_samples;
            if (sample.pulse && isBitMark(sample.duration_us) && _pending_frame) {
                _pending_frame->sample_count = _frame_samples;
                auto frame                   = std::move(_pending_frame);
                ++_stats.decoded_frames;
                reset();
                return frame;
            }
            rejectFrame(false);
            restartFrom(sample);
            break;
    }

    return std::nullopt;
}

void IrChatStreamDecoder::reset() noexcept
{
    _state = State::SeekingLeader;
    _bytes.clear();
    _current_byte         = 0;
    _bits_in_current_byte = 0;
    _expected_bytes       = 0;
    _frame_samples        = 0;
    _pending_frame.reset();
}

void IrChatStreamDecoder::beginFrame() noexcept
{
    reset();
    _frame_samples = 1;
}

void IrChatStreamDecoder::rejectFrame(bool crc_error) noexcept
{
    if (crc_error) {
        ++_stats.crc_errors;
    } else {
        ++_stats.malformed_frames;
    }
    reset();
}

bool IrChatStreamDecoder::consumeBit(bool one) noexcept
{
    _current_byte = static_cast<uint8_t>((_current_byte << 1U) | (one ? 1U : 0U));
    ++_bits_in_current_byte;
    if (_bits_in_current_byte != 8) {
        return true;
    }

    _bytes.push_back(_current_byte);
    _current_byte         = 0;
    _bits_in_current_byte = 0;

    if ((_bytes.size() == 1 && _bytes[0] != kMagic[0]) || (_bytes.size() == 2 && _bytes[1] != kMagic[1]) ||
        (_bytes.size() == 3 && _bytes[2] != kIrChatProtocolVersion)) {
        rejectFrame(false);
        return false;
    }

    if (_bytes.size() == kHeaderSize) {
        const std::size_t payload_size = _bytes[5];
        if (payload_size == 0 || payload_size > kMaxPayloadSize) {
            rejectFrame(false);
            return false;
        }
        _expected_bytes = kHeaderSize + payload_size + kCrcSize;
    }

    if (_expected_bytes == 0 || _bytes.size() < _expected_bytes) {
        return true;
    }
    if (_bytes.size() != _expected_bytes) {
        rejectFrame(false);
        return false;
    }

    const std::size_t data_size   = _bytes.size() - kCrcSize;
    const uint16_t expected_crc   = static_cast<uint16_t>(_bytes[data_size]) << 8U | _bytes[data_size + 1];
    const uint16_t calculated_crc = irChatCrc16(_bytes.data(), data_size);
    if (expected_crc != calculated_crc) {
        rejectFrame(true);
        return false;
    }

    std::vector<uint8_t> payload(_bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                                 _bytes.begin() + static_cast<std::ptrdiff_t>(data_size));
    if (!isPrintablePayload(payload)) {
        rejectFrame(false);
        return false;
    }

    _pending_frame = IrChatFrame{
        static_cast<uint16_t>(static_cast<uint16_t>(_bytes[3]) << 8U | _bytes[4]),
        std::move(payload),
        0,
    };
    return true;
}

void IrChatStreamDecoder::restartFrom(IrPulse sample) noexcept
{
    if (sample.pulse && isLeaderMark(sample.duration_us)) {
        beginFrame();
        _state = State::LeaderSpace;
    }
}

}  // namespace ir_chat::radio
