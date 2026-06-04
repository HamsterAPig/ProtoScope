#include "protoscope/transport/transport.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace protoscope::transport {

namespace {

    std::optional<unsigned int> comPortNumber(std::string_view portName)
    {
        if (portName.size() <= 3) {
            return std::nullopt;
        }
        if (std::toupper(static_cast<unsigned char>(portName[0])) != 'C' ||
            std::toupper(static_cast<unsigned char>(portName[1])) != 'O' ||
            std::toupper(static_cast<unsigned char>(portName[2])) != 'M') {
            return std::nullopt;
        }

        unsigned int number = 0;
        for (std::size_t i = 3; i < portName.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(portName[i]))) {
                return std::nullopt;
            }
            number = number * 10 + static_cast<unsigned int>(portName[i] - '0');
        }
        return number;
    }

    bool serialPortLess(const std::string& left, const std::string& right)
    {
        const auto leftCom = comPortNumber(left);
        const auto rightCom = comPortNumber(right);
        if (leftCom.has_value() && rightCom.has_value()) {
            return *leftCom < *rightCom;
        }
        if (leftCom.has_value() != rightCom.has_value()) {
            return leftCom.has_value();
        }
        return left < right;
    }

#if defined(_WIN32)
    std::vector<std::string> queryWindowsSerialPorts()
    {
        HKEY key = nullptr;
        const LONG openResult = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key);
        if (openResult != ERROR_SUCCESS) {
            return {};
        }

        std::vector<std::string> ports;
        char valueName[256]{};
        unsigned char valueData[256]{};
        for (DWORD index = 0;; ++index) {
            DWORD valueNameSize = sizeof(valueName);
            DWORD valueDataSize = sizeof(valueData);
            DWORD valueType = 0;
            const LONG enumResult =
                RegEnumValueA(key, index, valueName, &valueNameSize, nullptr, &valueType, valueData, &valueDataSize);
            if (enumResult == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (enumResult != ERROR_SUCCESS || valueType != REG_SZ || valueDataSize == 0) {
                continue;
            }

            ports.emplace_back(reinterpret_cast<const char*>(valueData));
        }

        RegCloseKey(key);
        return ports;
    }
#else
    void appendMatchingDevices(std::vector<std::string>& ports,
                               const std::filesystem::path& directory,
                               std::string_view prefix)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                break;
            }
            const auto filename = entry.path().filename().string();
            if (filename.rfind(prefix, 0) == 0) {
                ports.push_back(entry.path().string());
            }
        }
    }

    std::vector<std::string> queryUnixSerialPorts()
    {
        std::vector<std::string> ports;
        appendMatchingDevices(ports, "/dev", "ttyUSB");
        appendMatchingDevices(ports, "/dev", "ttyACM");
        appendMatchingDevices(ports, "/dev", "ttyS");
        appendMatchingDevices(ports, "/dev", "cu.");
        appendMatchingDevices(ports, "/dev", "tty.");
        return ports;
    }
#endif

} // namespace

std::vector<std::string> normalizeSerialPortNames(std::vector<std::string> ports)
{
    // 串口枚举可能来自注册表或设备目录，这里统一过滤、自然排序并去重，方便 UI 直接展示。
    ports.erase(std::remove_if(ports.begin(), ports.end(), [](const std::string& port) { return port.empty(); }),
                ports.end());
    std::sort(ports.begin(), ports.end(), serialPortLess);
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

std::vector<std::string> listAvailableSerialPorts()
{
#if defined(_WIN32)
    return normalizeSerialPortNames(queryWindowsSerialPorts());
#else
    return normalizeSerialPortNames(queryUnixSerialPorts());
#endif
}

} // namespace protoscope::transport
