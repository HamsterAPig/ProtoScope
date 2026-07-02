#include "protoscope/ui/algorithm_help.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace protoscope::ui {

namespace {

    constexpr std::array<AlgorithmHelpEntry, 7> kAlgorithmHelpEntries{{
        {
            "FFT 输入窗口与横轴",
            "FFT 对当前选定的输入窗口计算频谱。默认来自可视波形窗口，游标分屏时使用 A~B 时间窗口。"
            "点数 N 决定频率分辨率：delta f = Fs / N；全部可视样本和手动点数都允许非 2^n。"
            "窗函数用于控制频谱泄漏，Hann 适合常规观察，Rectangular 适合整周期采样，Blackman-Harris 旁瓣更低。"
            "相干增益会用于幅值校正，避免窗函数本身让稳定正弦幅值系统性偏小。"
            "横轴支持 Hz、次数和 log10Hz：Hz 是实际频率，次数是相对基波或窗口内参考频率的阶次，log10Hz "
            "用于跨数量级观察。",
            "FFT frequency window Hann Rectangular Blackman-Harris coherent gain Hz log10Hz 次数 频率 分辨率 窗函数 "
            "相干增益",
        },
        {
            "FFT 幅值单位",
            "频谱线性幅值来自波形实际值：actualValue = raw * ratio，单位沿用通道实际单位。"
            "dB 幅值使用 20 * log10(max(magnitude, 1e-12))，参考数值为 1，因此它表达的是相对 1 个实际单位的幅值。"
            "% 基波是无量纲百分比，用当前基波幅值作为 100%，便于查看谐波相对强度。",
            "FFT magnitude amplitude dB ratio actualValue raw % 基波 幅值 单位 线性 谐波",
        },
        {
            "FFT 相位与角度",
            "相位来自复数谱 atan2(imag, real)。内部同时保留弧度和角度，界面显示角度。"
            "角度会包到 (-180, 180]，超过范围的值按整圈折回。"
            "相位只相对当前 FFT 输入窗口成立；改变窗口、采样段或窗函数后，相位参考点也会随之变化。",
            "FFT phase angle atan2 imag real rad degree 相位 角度 弧度 复数谱",
        },
        {
            "波形值与测量统计",
            "波形时间轴由采样序号和采样率换算，显示单位通常为秒或毫秒。"
            "原始值 raw 先通过 ratio 变成实际值，显示层再应用 scale 和 offset；测量以当前通道实际值为基准。"
            "游标 A/B "
            "用于读取时间差、值差和窗口范围。RMS、均值、方差、峰峰值等统计量使用实际值单位，方差使用单位的平方。",
            "wave sample time raw actual ratio scale offset cursor RMS mean variance peak-to-peak 波形 游标 均值 方差 "
            "峰峰值",
        },
        {
            "渲染与交互",
            "可见样本超过预算时，波形会按屏幕像素做降采样包络，保留每个桶内的最小值和最大值，避免长历史绘制过重。"
            "视口缩放只改变当前可见时间范围，不修改原始采样数据。"
            "游标吸附会在设定通道范围内寻找最近采样点；bit lane 显示会优先吸附到跳变边沿，便于定位数字信号变化。",
            "render downsample envelope viewport zoom cursor snap bit lane transition 渲染 降采样 包络 视口 缩放 吸附 "
            "跳变",
        },
        {
            "协议处理与校验",
            "HEX 输入会归一化为空格分隔的大写字节，忽略多余空白并拒绝非十六进制字符。"
            "协议解析按配置处理字节序和帧长度，固定长度、表达式长度和运行时 profile 都会先经过边界检查。"
            "CRC16 和 CRC32 用于帧校验，校验失败的候选帧会触发重同步，解析器继续寻找后续合法帧。",
            "protocol HEX endian frame length runtime profile CRC16 CRC32 checksum resync 协议 字节序 帧长度 校验 "
            "重同步",
        },
        {
            "请求链路与原始回放",
            "send/request 会把发送动作、等待条件和状态流转记录到请求追踪中。guarded request 在 guard "
            "条件激活时串行执行，"
            "attempt 记录当前尝试次数，timeout ms 是单次等待超时时间。"
            ".psraw 保存 payload 和 events，timestamp ms 记录事件时间。导入回放会恢复 profile、plot setup 和采集事件，"
            "按事件时间轴复现当时的输入过程。",
            "send request guarded attempt timeout ms state psraw payload events timestamp profile plot setup replay "
            "请求 超时 回放 原始采集",
        },
    }};

    bool isAsciiSpace(char ch)
    {
        return static_cast<unsigned char>(ch) < 128U && std::isspace(static_cast<unsigned char>(ch)) != 0;
    }

    char lowerAscii(char ch)
    {
        const auto value = static_cast<unsigned char>(ch);
        if (value < 128U) {
            return static_cast<char>(std::tolower(value));
        }
        return ch;
    }

    std::string normalizeAsciiCase(std::string_view text)
    {
        std::string normalized;
        normalized.reserve(text.size());
        for (const char ch : text) {
            normalized.push_back(lowerAscii(ch));
        }
        return normalized;
    }

    std::vector<std::string> splitSearchTerms(std::string_view query)
    {
        std::vector<std::string> terms;
        std::size_t cursor = 0;
        while (cursor < query.size()) {
            while (cursor < query.size() && isAsciiSpace(query[cursor])) {
                ++cursor;
            }
            const std::size_t begin = cursor;
            while (cursor < query.size() && !isAsciiSpace(query[cursor])) {
                ++cursor;
            }
            if (begin != cursor) {
                terms.push_back(normalizeAsciiCase(query.substr(begin, cursor - begin)));
            }
        }
        return terms;
    }

    std::string entrySearchText(const AlgorithmHelpEntry& entry)
    {
        std::string text;
        text.reserve(entry.title.size() + entry.body.size() + entry.keywords.size() + 2U);
        text.append(entry.title);
        text.push_back(' ');
        text.append(entry.body);
        text.push_back(' ');
        text.append(entry.keywords);
        return normalizeAsciiCase(text);
    }

} // namespace

std::span<const AlgorithmHelpEntry> algorithmHelpEntries()
{
    return kAlgorithmHelpEntries;
}

std::vector<std::size_t> findAlgorithmHelpMatches(std::string_view query)
{
    const auto terms = splitSearchTerms(query);
    if (terms.empty()) {
        return {};
    }

    std::vector<std::size_t> matches;
    const auto entries = algorithmHelpEntries();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string text = entrySearchText(entries[index]);
        const bool allTermsMatched = std::all_of(
            terms.begin(), terms.end(), [&](const std::string& term) { return text.find(term) != std::string::npos; });
        if (allTermsMatched) {
            matches.push_back(index);
        }
    }
    return matches;
}

std::size_t nextAlgorithmHelpMatchOrdinal(std::span<const std::size_t> matches, std::size_t currentOrdinal)
{
    if (matches.empty()) {
        return kNoAlgorithmHelpMatch;
    }
    if (currentOrdinal == kNoAlgorithmHelpMatch || currentOrdinal + 1U >= matches.size()) {
        return 0U;
    }
    return currentOrdinal + 1U;
}

std::size_t previousAlgorithmHelpMatchOrdinal(std::span<const std::size_t> matches, std::size_t currentOrdinal)
{
    if (matches.empty()) {
        return kNoAlgorithmHelpMatch;
    }
    if (currentOrdinal == kNoAlgorithmHelpMatch || currentOrdinal == 0U || currentOrdinal >= matches.size()) {
        return matches.size() - 1U;
    }
    return currentOrdinal - 1U;
}

} // namespace protoscope::ui
