#pragma once

namespace protoscope::plot {
struct WaveViewState;
}

namespace protoscope::ui::wave_detail {

// 核心流程说明：波形目录下只保留可替换组件接口与少量绘图细节，不把纯算法 helper 抽象成基类。
void applyFrequencyInput(plot::WaveViewState& view);

} // namespace protoscope::ui::wave_detail
