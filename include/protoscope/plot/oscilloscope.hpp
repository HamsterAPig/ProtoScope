#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::plot {

enum class WaveControlMode {
    Oscilloscope,
    LegacyGlobal,
};

enum class WaveDisplayFormula {
    OffsetThenScale,
    ScaleThenOffset,
};

struct WaveSample {
    double time{0.0};
    double value{0.0};
};

struct WaveAppendRequest {
    std::string source{};
    std::vector<WaveSample> samples{};
};

struct ChannelSpec {
    std::string label{};
    std::string unit{};
    double ratio{1.0};
    double scale{1.0};
    double offset{0.0};
    std::optional<std::array<float, 4>> color{};
    std::optional<float> lineWidth{};
};

inline constexpr float kDefaultChannelLineWidth{1.5F};
inline constexpr float kMinChannelLineWidth{0.5F};
inline constexpr float kMaxChannelLineWidth{8.0F};

float sanitizeChannelLineWidth(double lineWidth);
float resolveChannelLineWidth(const std::optional<float>& lineWidth);
float resolveChannelLineWidth(const ChannelSpec& spec);
double applyChannelActualValue(double rawValue, const ChannelSpec& spec);
double applyChannelDisplayTransform(double rawValue,
                                    const ChannelSpec& spec,
                                    WaveDisplayFormula formula = WaveDisplayFormula::OffsetThenScale);

struct ViewConfig {
    double timeScale{1.0};
    std::string timeUnit{"s"};
    double verticalMin{-1.0};
    double verticalMax{1.0};
    std::string verticalUnit{"V"};
    std::size_t historyLimit{0};
    WaveDisplayFormula displayFormula{WaveDisplayFormula::OffsetThenScale};
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
    double displayValue{0.0};
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
    double medianValue{0.0};
    double p95Value{0.0};
    double p99Value{0.0};
    double variance{0.0};
    double stddev{0.0};
    std::optional<double> cv{};
    double mad{0.0};
    double medianAbsDev{0.0};
    double iqr{0.0};
    double p95Spread{0.0};
    double highWidth{0.0};
    double lowWidth{0.0};
    std::optional<double> dutyCycle{};
    std::optional<double> riseTime{};
    std::optional<double> fallTime{};
    std::size_t edgeCount{0};
    std::optional<double> absoluteError{};
    std::optional<double> relativeErrorPercent{};
    std::optional<double> meanError{};
    std::optional<double> mse{};
    std::optional<double> rmse{};
    std::optional<double> mae{};
    std::optional<double> maxAbsError{};
    std::optional<double> bias{};
    double duration{0.0};
};

MeasurementReadout makeMeasurementReadout(std::size_t channelIndex,
                                          const std::vector<double>& times,
                                          const std::vector<double>& values,
                                          const std::vector<double>* referenceValues = nullptr);

struct ChannelView {
    std::string label{};
    std::string unit{};
    double ratio{1.0};
    double scale{1.0};
    double offset{0.0};
    std::optional<std::array<float, 4>> color{};
    std::optional<float> lineWidth{};
    std::size_t totalSamples{0};
    std::size_t sampleIndexOffset{0};
    std::size_t visibleBegin{0};
    std::size_t visibleEnd{0};
    const WaveSample* samples{nullptr};
    WaveStats stats{};
};

float resolveChannelLineWidth(const ChannelView& channel);

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
    void setHistoryTrimSuspended(bool suspended);
    void setMaxTotalSamples(std::size_t maxTotalSamples);
    void setResetHistoryOnTimeReset(bool enabled);
    void preserveHistoryLimitAtLeast(std::size_t sampleCount);

    std::size_t channelCount() const;
    std::optional<ChannelSpec> channelSpec(std::size_t channelIndex) const;
    const ViewConfig& viewConfig() const;
    std::uint64_t dataRevision() const;
    std::optional<double> latestTime() const;

    bool append(std::size_t channelIndex, WaveAppendRequest request);
    WaveSnapshot snapshot(double visibleMinTime, double visibleMaxTime, bool computeStats = true) const;
    EnvelopeView buildEnvelope(std::size_t channelIndex,
                               double visibleMinTime,
                               double visibleMaxTime,
                               std::size_t pixelWidth) const;
    EnvelopeView buildLimitedEnvelope(std::size_t channelIndex,
                                      double visibleMinTime,
                                      double visibleMaxTime,
                                      std::size_t pixelWidth,
                                      std::size_t maxSamples) const;
    std::optional<CursorReadout> findNearest(
        std::size_t channelIndex, double time, double value, double maxTimeDistance, double maxValueDistance) const;
    std::optional<CursorReadout> findNearestByTime(std::size_t channelIndex, double time, double maxTimeDistance) const;
    MeasurementReadout measureWindow(std::size_t channelIndex, double beginTime, double endTime) const;

    static DeltaReadout makeDelta(const CursorReadout& left, const CursorReadout& right);

private:
    struct ChannelBuffer {
        ChannelSpec spec{};
        std::vector<WaveSample> samples;
        std::size_t sampleIndexOffset{0};
    };

    ChannelBuffer& ensureChannel(std::size_t channelIndex);
    void applyAppendSource(const std::string& source);
    bool appendAfterHistoryReset(std::size_t channelIndex, const WaveAppendRequest& request);
    bool appendPreparedSamples(ChannelBuffer& channel, const std::vector<WaveSample>& samples);
    std::size_t lowerBoundByTime(const std::vector<WaveSample>& samples, double time) const;
    std::size_t upperBoundByTime(const std::vector<WaveSample>& samples, double time) const;
    std::size_t effectiveHistoryLimit() const;
    bool trimHistory(ChannelBuffer& channel);
    WaveStats makeStats(const std::vector<WaveSample>& samples,
                        std::size_t begin,
                        std::size_t end,
                        const ChannelSpec& spec) const;

private:
    std::string source_;
    ViewConfig config_{};
    std::vector<ChannelBuffer> channels_;
    std::uint64_t dataRevision_{0};
    std::size_t preservedHistoryLimit_{0};
    std::size_t maxTotalSamples_{0};
    bool resetHistoryOnTimeReset_{true};
    bool historyTrimSuspended_{false};
};

} // namespace protoscope::plot
