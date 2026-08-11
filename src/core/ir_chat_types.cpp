#include "core/ir_chat_types.hpp"

namespace ir_chat {

const char* pageIdName(PageId page)
{
    switch (page) {
        case PageId::Chat:
            return "chat";
    }
    return "unknown";
}

const char* radioUiStateName(RadioUiState state)
{
    switch (state) {
        case RadioUiState::Initializing:
            return "INITIALIZING";
        case RadioUiState::Receiving:
            return "RECEIVING";
        case RadioUiState::Sending:
            return "SENDING";
        case RadioUiState::Error:
            return "IR OFF";
        case RadioUiState::Stopped:
            return "STOPPED";
    }
    return "UNKNOWN";
}

}  // namespace ir_chat
