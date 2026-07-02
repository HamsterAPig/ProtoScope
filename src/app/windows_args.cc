#include "protoscope/app/windows_args.hpp"

#if defined(_WIN32)
#include <windows.h>

namespace protoscope::app {

std::string wideArgToUtf8(const wchar_t* wideArg)
{
    if (wideArg == nullptr) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, wideArg, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string buffer(static_cast<std::size_t>(size), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, wideArg, -1, buffer.data(), static_cast<int>(buffer.size()), nullptr, nullptr);
    if (converted <= 0) {
        return {};
    }

    buffer.resize(static_cast<std::size_t>(converted));
    if (!buffer.empty() && buffer.back() == '\0') {
        buffer.pop_back();
    }
    return buffer;
}

} // namespace protoscope::app
#endif
