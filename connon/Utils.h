#pragma once

#include <iostream>
#include "message.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Utils
{
    // 初始化
    void init();

    // 输出信息
    void Out_Msg(std::string, int);
    // 错误信息
    void Out_Err(std::string, int);

    // 网络输出
    // 网络部分输出
    void Out_Net_Msg(unsigned long long, std::string, int);
} // namespace Utils