#pragma once

#include "protoscope/data/file_output.hpp"

namespace protoscope::plot {

// 保留既有绘图文件 API 的名称，存储模块直接使用独立数据层实现。
using data::DataFileOutput;
using data::dataFileStopToken;
using data::dataFileProgressCallback;
using data::reportDataFileProgress;

}
