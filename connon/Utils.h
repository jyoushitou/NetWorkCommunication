#pragma once

#include <iostream>
#include "message.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Utils
{
    void init();

    void Out_Msg(std::string, int); // 输出信息
    void Out_Err(std::string, int); // 错误信息
} // namespace Utils