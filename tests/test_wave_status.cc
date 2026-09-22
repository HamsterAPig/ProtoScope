#include "protoscope/ui/wave_status.hpp"
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    using namespace protoscope::ui;
    try {
        WaveStatusPresenter presenter;
        WaveStatusSnapshot snapshot;
        snapshot.context = "protocol";
        snapshot.fftEnabled = true;
        require(presenter.update(snapshot, 0).fft == "FFT", "initial ready");
        snapshot.fftPending = true;
        presenter.update(snapshot, 1000);
        require(presenter.update(snapshot, 1299).fft == "FFT", "wait hidden before 300ms");
        require(presenter.update(snapshot, 1300).fft == "FFT 待更新", "wait visible at 300ms");
        snapshot.fftPending = false;
        require(presenter.update(snapshot, 1301).fft == "FFT 待更新", "minimum wait residence");
        require(presenter.update(snapshot, 2299).fft == "FFT 待更新", "residence below 1000ms");
        require(presenter.update(snapshot, 2300).fft == "FFT", "residence ends at 1000ms");
        snapshot.modes = "A";
        require(presenter.update(snapshot, 2799).modes.empty(), "ordinary throttled at 499ms");
        snapshot.modes = "B";
        require(presenter.update(snapshot, 2800).modes == "B", "latest only at 500ms");
        snapshot.modes = "C";
        presenter.update(snapshot, 2900);
        snapshot.modes = "D";
        presenter.update(snapshot, 3000);
        snapshot.modes = "E";
        require(presenter.update(snapshot, 3300).modes == "E", "no stale queue");
        snapshot.fftPending = true;
        presenter.update(snapshot, 3400);
        snapshot.fftPending = false;
        require(presenter.update(snapshot, 3699).fft == "FFT", "short wait never displayed");
        snapshot.fftError = "FFT error A";
        require(presenter.update(snapshot, 3700).fft == "FFT error A", "new error immediate");
        snapshot.fftError = "FFT error B";
        require(presenter.update(snapshot, 3701).fft == "FFT error B", "changed error immediate");
        snapshot.modes = "F";
        presenter.update(snapshot, 3799);
        require(presenter.update(snapshot, 3800).modes == "F", "same error does not reset ordinary timer");
        snapshot.fftEnabled = false;
        require(presenter.update(snapshot, 3801).fft.empty(), "disable clears immediately");

        snapshot.fftError.clear();
        snapshot.fftEnabled = true;
        snapshot.fftPending = true;
        presenter.update(snapshot, 4300);
        presenter.update(snapshot, 4800);
        require(presenter.display().fft == "FFT 待更新", "pending shown");
        snapshot.epoch++;
        snapshot.fftPending = false;
        require(presenter.update(snapshot, 4801).fft == "FFT", "clear history bypasses residence");
        snapshot.fftError = "old protocol error";
        presenter.update(snapshot, 4802);
        snapshot.context = "new protocol";
        snapshot.fftError.clear();
        require(!presenter.update(snapshot, 4803).fftError, "protocol change clears error");

        WaveStatusPresenter statistics;
        snapshot = {};
        snapshot.statisticsEnabled = true;
        snapshot.statisticsPending = true;
        require(statistics.update(snapshot, 0).statistics.empty(), "statistics initially hidden");
        require(statistics.update(snapshot, 299).statistics.empty(), "statistics below threshold");
        require(statistics.update(snapshot, 300).statistics == "统计待更新", "statistics threshold");
        snapshot.statisticsPending = false;
        statistics.update(snapshot, 301);
        snapshot.statisticsPending = true;
        statistics.update(snapshot, 500);
        snapshot.statisticsPending = false;
        statistics.update(snapshot, 501);
        require(statistics.update(snapshot, 1300).statistics.empty(), "repeat wait does not reset residence");
        snapshot.statisticsPending = true;
        statistics.update(snapshot, 1500);
        statistics.update(snapshot, 1800);
        snapshot.statisticsEnabled = false;
        require(statistics.update(snapshot, 1801).statistics.empty(), "close statistics immediately");
        std::cout << "wave status timing passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
