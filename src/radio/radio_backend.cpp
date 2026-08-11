#include "radio/radio_backend.hpp"

#include <stdexcept>

namespace ir_chat::radio {

std::unique_ptr<RadioBackend> makeDefaultRadioBackend()
{
#if defined(IR_CHAT_USE_MOCK_RADIO) && IR_CHAT_USE_MOCK_RADIO
    return makeMockRadioBackend();
#elif defined(IR_CHAT_ENABLE_LINUX_RADIO) && IR_CHAT_ENABLE_LINUX_RADIO
    return makeLinuxIrBackend();
#else
    throw std::runtime_error("no IR backend selected; define IR_CHAT_USE_MOCK_RADIO or IR_CHAT_ENABLE_LINUX_RADIO");
#endif
}

}  // namespace ir_chat::radio
