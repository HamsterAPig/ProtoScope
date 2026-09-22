#pragma once

#include "protoscope/data/table.hpp"

namespace protoscope::scripting {
struct DataTableColumn {
    std::string field;
    std::string label;
    std::string unit;
    int precision{3};
    data::FieldType type{data::FieldType::String};
};
struct DataTableConfig {
    std::string dataset;
    std::optional<std::string> device;
    bool history{false};
    std::vector<DataTableColumn> columns;
    std::size_t maxRows{200};
    std::size_t maxBytes{4U*1024U*1024U};
    std::size_t pageSize{200};
    int visibleRows{10};
};
struct DataTableExportState {
    bool choosingPath{false};
    bool running{false};
    std::string message;
};
enum class DataTableAction { Refresh, View, Select, Cancel, ExportCsv, ExportPsrec, CancelExport };
struct DataTableEvent {
    std::string id;
    std::uint64_t runtimeGeneration{0};
    std::uint64_t pageRevision{0};
    DataTableAction action{DataTableAction::Refresh};
    data::TableView view;
    std::uint64_t selectedRow{0};
};
} // namespace protoscope::scripting
