#include "core/ir_chat_router.hpp"

namespace ir_chat {

void IRChatRouter::replace(PageId page)
{
    if (_current_page.get() != page) {
        _current_page.set(page);
    }
}

void IRChatRouter::push(PageId page)
{
    if (_current_page.get() == page) {
        return;
    }
    _history.push_back(_current_page.get());
    _current_page.set(page);
}

void IRChatRouter::back()
{
    if (_history.empty()) {
        replace(PageId::Chat);
        return;
    }

    const PageId previous = _history.back();
    _history.pop_back();
    _current_page.set(previous);
}

}  // namespace ir_chat
