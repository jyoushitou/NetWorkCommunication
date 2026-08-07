#pragma once
#include "Utils.h"
namespace Net
{
    // 消息ID长度
    constexpr int HEAD_ID_LENGTH = 4;
    // 消息长度长度
    constexpr int HEAD_LEN_LENGTH = 4;
    // 消息头部长度
    constexpr int HEAD_LENGTH = HEAD_ID_LENGTH + HEAD_LEN_LENGTH;
    // 最大消息长度（1M）
    constexpr int MAX_LENGTH = 1024 * 1024;
} // namespace Net