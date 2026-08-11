#include "radio/ir_chat_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ir_chat::radio::IrChatFrame;
using ir_chat::radio::IrChatStreamDecoder;
using ir_chat::radio::IrPulse;

constexpr uint64_t kLinuxLircMaxDurationUs = 500000;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::optional<IrChatFrame> feedDurations(IrChatStreamDecoder& decoder, const std::vector<uint32_t>& durations,
                                         std::size_t begin = 0, std::size_t end = std::string::npos)
{
    end = std::min(end, durations.size());
    std::optional<IrChatFrame> decoded;
    for (std::size_t index = begin; index < end; ++index) {
        if (auto frame = decoder.feed(IrPulse{index % 2 == 0, durations[index]})) {
            decoded = std::move(frame);
        }
    }
    return decoded;
}

std::vector<uint8_t> bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

std::size_t bitCount(uint8_t value)
{
    std::size_t count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

uint64_t worstCasePrintableFrameDuration(std::size_t payloadSize)
{
    constexpr std::size_t kHeaderAndCrcBytes      = 8;
    constexpr std::size_t kMaximumSequenceOneBits = 16;
    constexpr std::size_t kMaximumCrcOneBits      = 16;

    std::size_t maximumPrintableOneBits = 0;
    for (uint16_t value = 0x20; value <= 0x7e; ++value) {
        maximumPrintableOneBits = std::max(maximumPrintableOneBits, bitCount(static_cast<uint8_t>(value)));
    }

    const std::size_t frameBits = (kHeaderAndCrcBytes + payloadSize) * 8;
    const std::size_t maximumOneBits =
        bitCount('I') + bitCount('R') + bitCount(ir_chat::radio::kIrChatProtocolVersion) + kMaximumSequenceOneBits +
        bitCount(static_cast<uint8_t>(payloadSize)) + payloadSize * maximumPrintableOneBits + kMaximumCrcOneBits;

    return ir_chat::radio::kIrChatLeaderMarkUs + ir_chat::radio::kIrChatLeaderSpaceUs +
           ir_chat::radio::kIrChatBitMarkUs +
           frameBits * (ir_chat::radio::kIrChatBitMarkUs + ir_chat::radio::kIrChatZeroSpaceUs) +
           maximumOneBits * (ir_chat::radio::kIrChatOneSpaceUs - ir_chat::radio::kIrChatZeroSpaceUs);
}

void testRoundTrip()
{
    const auto payload   = bytes("hello infrared");
    const auto durations = ir_chat::radio::encodeIrChatFrame(0x1234, payload);

    IrChatStreamDecoder decoder;
    const auto frame = feedDurations(decoder, durations);
    require(frame.has_value(), "encoded frame must decode");
    require(frame->sequence == 0x1234, "sequence must round-trip");
    require(frame->payload == payload, "payload must round-trip");
    require(frame->sample_count == durations.size(), "sample count must describe the complete waveform");
    require(decoder.stats().decoded_frames == 1, "decoded frame count must advance");
}

void testKnownCrcVector()
{
    const auto input = bytes("123456789");
    require(ir_chat::radio::irChatCrc16(input.data(), input.size()) == 0x29b1,
            "CRC-16/CCITT-FALSE must match its standard check value");
}

void testStreamingAcrossCalls()
{
    const auto payload           = bytes("split input");
    const auto durations         = ir_chat::radio::encodeIrChatFrame(7, payload);
    const std::size_t first_end  = durations.size() / 3;
    const std::size_t second_end = durations.size() * 2 / 3;

    IrChatStreamDecoder decoder;
    require(!feedDurations(decoder, durations, 0, first_end), "first partial read must not emit a frame");
    require(!feedDurations(decoder, durations, first_end, second_end), "second partial read must retain state");
    const auto frame = feedDurations(decoder, durations, second_end);
    require(frame && frame->payload == payload, "final partial read must complete the retained frame");
}

void testTimingToleranceAndNoiseRecovery()
{
    const auto payload = bytes("jitter");
    auto durations     = ir_chat::radio::encodeIrChatFrame(91, payload);
    for (std::size_t index = 0; index < durations.size(); ++index) {
        const int adjustment = index % 3 == 0 ? -18 : (index % 3 == 1 ? 16 : 0);
        durations[index]     = static_cast<uint32_t>(static_cast<int64_t>(durations[index]) * (100 + adjustment) / 100);
    }

    IrChatStreamDecoder decoder;
    decoder.feed({false, 32000});
    decoder.feed({true, 1100});
    decoder.feed({false, 720});
    const auto frame = feedDurations(decoder, durations);
    require(frame && frame->payload == payload, "decoder must tolerate jitter and preceding remote-control noise");
}

void testCrcFailureIsIgnoredAndDecoderRecovers()
{
    auto corrupted                    = ir_chat::radio::encodeIrChatFrame(3, bytes("bad crc"));
    const std::size_t first_crc_space = 2 + (6 + bytes("bad crc").size()) * 16 + 1;
    require(first_crc_space < corrupted.size(), "CRC test index must be inside waveform");
    corrupted[first_crc_space] = corrupted[first_crc_space] == ir_chat::radio::kIrChatZeroSpaceUs
                                     ? ir_chat::radio::kIrChatOneSpaceUs
                                     : ir_chat::radio::kIrChatZeroSpaceUs;

    IrChatStreamDecoder decoder;
    require(!feedDurations(decoder, corrupted), "CRC-corrupted frame must not be emitted");
    require(decoder.stats().crc_errors == 1, "CRC failure must be counted");

    const auto valid = ir_chat::radio::encodeIrChatFrame(4, bytes("recovered"));
    const auto frame = feedDurations(decoder, valid);
    require(frame && frame->sequence == 4, "decoder must recover after a corrupt frame");
}

void testLimitsAndPrintablePayload()
{
    const std::vector<uint8_t> maximum(ir_chat::radio::kMaxPayloadSize, '_');
    IrChatStreamDecoder decoder;
    const auto frame = feedDurations(decoder, ir_chat::radio::encodeIrChatFrame(0xffff, maximum));
    require(frame && frame->payload == maximum, "maximum payload must round-trip");
    require(worstCasePrintableFrameDuration(ir_chat::radio::kMaxPayloadSize) <= kLinuxLircMaxDurationUs,
            "maximum payload must fit Linux LIRC's 500 ms transmit limit for every printable payload");
    require(worstCasePrintableFrameDuration(ir_chat::radio::kMaxPayloadSize + 1) > kLinuxLircMaxDurationUs,
            "the payload limit must retain its 500 ms safety boundary");

    bool oversized_rejected = false;
    try {
        ir_chat::radio::encodeIrChatFrame(0, std::vector<uint8_t>(ir_chat::radio::kMaxPayloadSize + 1, 'x'));
    } catch (const std::invalid_argument&) {
        oversized_rejected = true;
    }
    require(oversized_rejected, "oversized payload must be rejected");

    bool binary_rejected = false;
    try {
        ir_chat::radio::encodeIrChatFrame(0, std::vector<uint8_t>{'x', 0, 'y'});
    } catch (const std::invalid_argument&) {
        binary_rejected = true;
    }
    require(binary_rejected, "non-printable payload must be rejected");
}

}  // namespace

int main()
{
    testKnownCrcVector();
    testRoundTrip();
    testStreamingAcrossCalls();
    testTimingToleranceAndNoiseRecovery();
    testCrcFailureIsIgnoredAndDecoderRecovers();
    testLimitsAndPrintablePayload();
    std::cout << "IR Chat codec tests passed\n";
    return 0;
}
