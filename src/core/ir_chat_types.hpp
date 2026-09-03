#pragma once

#include "radio/radio_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ir_chat {

namespace ir_chat_key {

constexpr uint32_t Up    = 0x10001;
constexpr uint32_t Down  = 0x10002;
constexpr uint32_t Left  = 0x10003;
constexpr uint32_t Right = 0x10004;
// Linux input KEY_HELP event for the keyboard help action.
constexpr uint32_t Help  = 0x10005;

}  // namespace ir_chat_key

enum class PageId {
    Chat = 0,
};

enum class ChatSection {
    Messages,
    Info,
};

enum class RadioUiState {
    Initializing,
    Receiving,
    Sending,
    Error,
    Stopped,
};

struct ChatMessage {
    uint64_t id = 0;
    std::string text;
    uint64_t sequence = 0;
    bool outgoing     = false;
    bool sendFailed   = false;
};

struct ChatRadioInfo {
    RadioUiState state        = RadioUiState::Initializing;
    bool ready                = false;
    bool initializationFailed = false;
    std::string backendName{"IR"};
    std::string rxDevice{"Unavailable"};
    std::string txDevice{"Unavailable"};
    uint32_t carrierHz = 38000;
    std::string protocolName{"IR Chat"};
    bool mock             = false;
    uint64_t rxCount      = 0;
    uint64_t txCount      = 0;
    uint64_t droppedCount = 0;
    std::string diagnostics{"Starting IR"};
};

struct ChatScrollRequest {
    uint32_t serial = 0;
    int32_t amount  = 0;
    bool toBottom   = false;
};

constexpr std::size_t kMessageHistoryLimit = 64;
constexpr std::size_t kMaxMessageBytes     = radio::kMaxPayloadSize;
constexpr int32_t kMessageScrollStep       = 36;

const char* pageIdName(PageId page);
const char* radioUiStateName(RadioUiState state);

}  // namespace ir_chat
