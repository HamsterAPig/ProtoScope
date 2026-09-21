#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace protoscope::plot { struct WaveDockState; }

namespace protoscope::ui {

struct WaveStatusSnapshot {
    std::string context;
    std::uint64_t epoch{0};
    std::string modes;
    bool fftEnabled{false};
    bool fftPending{false};
    std::string fftError;
    bool statisticsEnabled{false};
    bool statisticsPending{false};
};

struct WaveStatusDisplay {
    std::string modes;
    std::string fft;
    std::string statistics;
    bool fftError{false};

    std::string text() const
    {
        std::string result;
        for (const auto* part : {&modes, &fft, &statistics}) {
            if (part->empty()) continue;
            if (!result.empty()) result += " | ";
            result += *part;
        }
        return result;
    }
};

WaveStatusSnapshot makeWaveStatusSnapshot(const plot::WaveDockState& wave, std::string context);

class WaveStatusPresenter {
public:
    const WaveStatusDisplay& update(const WaveStatusSnapshot& snapshot, std::uint64_t nowMs)
    {
        // 协议切换或历史清空立即失效，不能让旧等待的最短停留跨越上下文。
        if (!initialized_ || context_ != snapshot.context || epoch_ != snapshot.epoch) {
            display_ = {};
            fftWait_ = {};
            statisticsWait_ = {};
            lastPublished_.reset();
            context_ = snapshot.context;
            epoch_ = snapshot.epoch;
            initialized_ = true;
        }
        if (!snapshot.fftEnabled) {
            display_.fft.clear();
            display_.fftError = false;
            fftWait_ = {};
        }
        if (!snapshot.statisticsEnabled) {
            display_.statistics.clear();
            statisticsWait_ = {};
        }
        fftWait_.observe(snapshot.fftEnabled && snapshot.fftPending, nowMs);
        statisticsWait_.observe(snapshot.statisticsEnabled && snapshot.statisticsPending, nowMs);

        // 新错误越过普通文字节流；重复错误不会延长等待或重置发布时间。
        if (snapshot.fftEnabled && !snapshot.fftError.empty() &&
            (!display_.fftError || display_.fft != snapshot.fftError)) {
            display_.fft = snapshot.fftError;
            display_.fftError = true;
            fftWait_.visibleSince.reset();
        }
        if (lastPublished_ && nowMs - *lastPublished_ < 500) return display_;

        auto next = display_;
        next.modes = snapshot.modes;
        if (snapshot.fftEnabled) {
            next.fftError = !snapshot.fftError.empty();
            next.fft = next.fftError ? snapshot.fftError :
                fftWait_.waiting(nowMs) ? "FFT 待更新" : "FFT";
        }
        if (snapshot.statisticsEnabled) {
            next.statistics = statisticsWait_.waiting(nowMs) ? "统计待更新" : "";
        }
        if (next.modes != display_.modes || next.fft != display_.fft ||
            next.statistics != display_.statistics || next.fftError != display_.fftError) {
            fftWait_.published(next.fft == "FFT 待更新", nowMs);
            statisticsWait_.published(next.statistics == "统计待更新", nowMs);
            display_ = std::move(next);
            lastPublished_ = nowMs;
        }
        return display_;
    }

    const WaveStatusDisplay& display() const { return display_; }

private:
    struct Waiting {
        std::optional<std::uint64_t> pendingSince;
        std::optional<std::uint64_t> visibleSince;

        void observe(bool pending, std::uint64_t now)
        {
            if (!pending) pendingSince.reset();
            else if (!pendingSince) pendingSince = now;
        }
        bool waiting(std::uint64_t now) const
        {
            // 短于 300ms 不显示；实际发布后至少保留 1000ms。
            return (pendingSince && now - *pendingSince >= 300) ||
                   (visibleSince && now - *visibleSince < 1000);
        }
        void published(bool waiting, std::uint64_t now)
        {
            if (!waiting) visibleSince.reset();
            else if (!visibleSince) visibleSince = now;
        }
    };
    WaveStatusDisplay display_;
    Waiting fftWait_;
    Waiting statisticsWait_;
    std::optional<std::uint64_t> lastPublished_;
    std::string context_;
    std::uint64_t epoch_{0};
    bool initialized_{false};
};

} // namespace protoscope::ui
