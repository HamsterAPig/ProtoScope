#include "../src/ui/wave/wave_render_service.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <thread>

using namespace protoscope;
namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void settle(plot::WaveDockState& wave)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        ui::prepareWaveFrame(wave, 900);
        if (wave.cachedFftKeyValid && !wave.view.fftUpdatePending) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("FFT did not settle");
}

void run(plot::WaveFftDisplayMode mode)
{
    plot::WaveDockState wave;
    auto& view = wave.view;
    view.initialized = true;
    view.defaultViewportPending = false;
    view.autoFollowLatest = true;
    view.visibleDuration = 1;
    view.sampleFrequencyHz = 1024;
    view.fft.enabled = true;
    view.fft.pointCount = plot::WaveFftPointCount::N1024;
    view.fft.displayMode = mode;
    std::size_t index = 0;
    auto append = [&](std::size_t count, double hz) {
        plot::WaveAppendRequest request;
        for (std::size_t i = 0; i < count; ++i, ++index)
            request.samples.push_back({index / 1024.0,
                std::sin(2 * std::numbers::pi * hz * index / 1024.0)});
        require(wave.buffer.append(0, std::move(request)), "append failed");
    };
    auto peak = [&]() {
        require(wave.cachedFftFrame.fundamentalHz.has_value(), "no spectrum peak");
        return *wave.cachedFftFrame.fundamentalHz;
    };
    append(2048, 32);
    settle(wave);
    require(std::abs(peak() - 32) < 1, "initial peak wrong");
    const auto generation = wave.fftRequestGeneration;
    const auto firstRevision = wave.cachedFftKey.dataRevision;
    bool updatedWhileStreaming = false;
    for (int frame = 0; frame < 160; ++frame) {
        append(64, 96);
        ui::prepareWaveFrame(wave, 900);
        require(wave.fftRequestGeneration == generation, "streaming invalidated in-flight result");
        updatedWhileStreaming |= wave.cachedFftKey.dataRevision > firstRevision &&
            wave.cachedFftFrame.fundamentalHz && std::abs(peak() - 96) < 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(updatedWhileStreaming, "streaming results starved or peak never updated");
    settle(wave);
    require(std::abs(view.fftSourceMaxTime - (index - 1) / 1024.0) < 1e-9,
            "FFT source window stopped following");
    require(std::abs(peak() - 96) < 1, "latest peak wrong");

    view.autoFollowLatest = false;
    settle(wave);
    const double frozenMin = view.fftSourceMinTime;
    const double frozenMax = view.fftSourceMaxTime;
    append(2048, 180);
    view.fftFrequencyMin = 20;
    view.fftFrequencyMax = 220;
    view.fftMagnitudeMin = -10;
    view.fftMagnitudeMax = 10;
    settle(wave);
    require(view.fftSourceMinTime == frozenMin && view.fftSourceMaxTime == frozenMax,
            "stopped FFT input moved with data or frequency viewport");
    require(std::abs(peak() - 96) < 1, "stopped FFT no longer uses history");
    view.autoFollowLatest = true;
    settle(wave);
    require(std::abs(peak() - 180) < 1, "resumed FFT did not catch latest signal");

    const auto oldGeneration = wave.fftRequestGeneration;
    view.fft.window = plot::WaveFftWindow::BlackmanHarris;
    ui::prepareWaveFrame(wave, 900);
    require(wave.fftRequestGeneration > oldGeneration && !wave.cachedFftKeyValid,
            "parameter change accepted old result");
    settle(wave);
    wave.fftRefreshRequested = true;
    const auto refreshGeneration = wave.fftRequestGeneration;
    ui::prepareWaveFrame(wave, 900);
    require(wave.fftRequestGeneration > refreshGeneration, "manual refresh did not invalidate old work");
    settle(wave);
    wave.fftChannelEnabled[0] = 0;
    settle(wave);
    require(!wave.cachedFftFrame.valid, "disabled channel retained old spectrum");
    wave.fftChannelEnabled[0] = 1;
    settle(wave);
    append(64, 200);
    ui::prepareWaveFrame(wave, 900);
    wave.buffer.clear();
    ui::prepareWaveFrame(wave, 900);
    require(!wave.cachedFftFrame.valid, "cleared history retained old spectrum");
    settle(wave);
    require(!wave.cachedFftFrame.valid, "old in-flight spectrum resurrected after clear");
    index = 0;
    append(2048, 48);
    settle(wave);
    require(std::abs(peak() - 48) < 1, "new history spectrum wrong");
}
}

int main()
{
    try {
        run(plot::WaveFftDisplayMode::FullSpectrum);
        run(plot::WaveFftDisplayMode::CursorSplit);
        std::cout << "Full and split FFT: live peaks, pause/resume, conditions, refresh and history passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
