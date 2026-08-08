#include "Utils.h"

namespace Utils
{
    void init()
    {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
#endif
    }

    // 正常输出
    void Out_Msg(std::string msg, int MSID)
    {
        std::cout << '[' << ServiceID[MSID] << ']' << msg << std::endl;
    }

    // 错误输出
    void Out_Err(std::string msg, int MSID)
    {
        std::cerr << '[' << ServiceID[MSID] << ']' << msg << std::endl;
    }

    // 网络输出
    // 网络部分输出
    void Out_Net_Msg(unsigned long long msg_id, std::string msg, int MSID)
    {
        Out_Msg(std::to_string(msg_id) + ":" + msg, MSID);
    }
} // namespace Utils
