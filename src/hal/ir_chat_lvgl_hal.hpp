#pragma once

#include <lvgl.h>
#include <cstdint>

namespace ir_chat {

bool initLvglHal(int32_t width, int32_t height);
void shutdownLvglHal();

}  // namespace ir_chat
