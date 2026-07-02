#pragma once

#include <string>

namespace protoscope::app {

#if defined(_WIN32)
std::string wideArgToUtf8(const wchar_t* wideArg);
#endif

} // namespace protoscope::app
