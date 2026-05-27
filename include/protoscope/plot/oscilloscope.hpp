#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::plot {

struct WaveSample {
    double time{0.0};
    double value{0.0};
};

struct WaveAppendRequest {
    std::string source;
    std::vector<WaveSample> samples;
};

struct ChannelSpec {
    std::string label;
    std::string unit;
    double scale{1.0};
    double offset{0.0};
};

double applyChannelDisplayTransform(double rawValue, const ChannelSpec& spec);

struct ViewConfig {
    double timeScale{1.0};
    std::string timeUnit{"s"};
    double verticalMin{-1.0};
    double verticalMax{1.0};
    std::string verticalUnit{"V"};
    std::size_t historyLimit{200000};
};

struct WaveStats {
    std::size_t totalSamples{0};
    std::size_t visibleSamples{0};
    double sampleRateHz{0.0};
    double minValue{0.0};
    double maxValue{0.0};
};

struct CursorReadout {
    bool valid{false};
    std::size_t channelIndex{0};
    std::size_t sampleIndex{0};
    double time{0.0};
    double value{0.0};
};

struct DeltaReadout {
    bool valid{false};
    double deltaTime{0.0};
    double deltaValue{0.0};
    double frequencyHz{0.0};
};

struct MeasurementReadout {
    bool valid{false};
    std::size_t channelIndex{0};
    std::size_t sampleCount{0};
    double minValue{0.0};
    double maxValue{0.0};
    double peakToPeak{0.0};
    double meanValue{0.0};
    double rmsValue{0.0};
    double duration{0.0};
};

struct ChannelView {
    std::string label;
    std::string unit;
    double scale{1.0};
    double offset{0.0};
    std::size_t totalSamples{0};
    std::size_t visibleBegin{0};
    std::size_t visibleEnd{0};
    const WaveSample* samples{nullptr};
    WaveStats stats{};
};

struct EnvelopePoint {
    double time{0.0};
    double minValue{0.0};
    double maxValue{0.0};
    std::size_t sampleCount{1};
};

struct EnvelopeView {
    std::vector<EnvelopePoint> points;
    std::size_t sourceSampleCount{0};
};

struct WaveSnapshot {
    std::string source;
    ViewConfig config{};
    std::vector<ChannelView> channels;
};

class OscilloscopeBuffer {
public:
    void clear();
    void configureChannels(std::size_t channelCount);
    void setChannelSpec(std::size_t channelIndex, ChannelSpec spec);
    void setViewConfig(const ViewConfig& config);

    std::size_t channelCount() const;
    std::optional<ChannelSpec> channelSpec(std::size_t channelIndex) const;
    const ViewConfig& viewConfig() const;

    bool append(std::size_t channelIndex, WaveAppendRequest request);
    WaveSnapshot snapshot(double visibleMinTime, double visibleMaxTime) const;
    EnvelopeView buildEnvelope(std::size_t channelIndex,
                               double visibleMinTime,
                               double visibleMaxTime,
                               std::size_t pixelWidth) const;
    EnvelopeView buildLimitedEnvelope(std::size_t channelIndex,
                                      double visibleMinTime,
                                      double visibleMaxTime,
                                      std::size_t pixelWidth,
                                      std::size_t maxSamples) const;
    std::optional<CursorReadout> findNearest(std::size_t channelIndex,
                                             double time,
                                             double value,
                                             double maxTimeDistance,
                                             double maxValueDistance) const;
    std::optional<CursorReadout> findNearestByTime(std::size_t channelIndex,
                                                   double time,
                                                   double maxTimeDistance) const;
    MeasurementReadout measureWindow(std::size_t channelIndex,
                                     double beginTime,
                                     double endTime) const;

    static DeltaReadout makeDelta(const CursorReadout& left, const CursorReadout& right);

private:
    struct ChannelBuffer {
        ChannelSpec spec{};
        std::vector<WaveSample> samples;
    };

    std::size_t lowerBoundByTime(const std::vector<WaveSample>& samples, double time) const;
    std::size_t upperBoundByTime(const std::vector<WaveSample>& samples, double time) const;
    void trimHistory(ChannelBuffer& channel);
    WaveStats makeStats(const std::vector<WaveSample>& samples,
                        std::size_t begin,
                        std::size_t end,
                        const ChannelSpec& spec) const;

private:
    std::string source_;
    ViewConfig config_{};
    std::vector<ChannelBuffer> channels_;
};

} // namespace protoscope::plot
