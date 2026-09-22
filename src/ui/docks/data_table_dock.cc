#include "protoscope/ui/ui_theme.hpp"
#include "../runtime/gui_runtime_detail.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/icons.hpp"

#include <charconv>
#include <iomanip>
#include <locale>
#include <sstream>

namespace protoscope::ui {
namespace {
std::string cellText(const data::Value* value,int precision)
{
    if (!value) return "--";
    return std::visit([&](const auto& item)->std::string {
        using T=std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T,std::monostate>) return "null";
        else if constexpr (std::is_same_v<T,std::int64_t>) return std::to_string(item);
        else if constexpr (std::is_same_v<T,double>) {
            std::ostringstream stream;stream.imbue(std::locale::classic());
            stream<<std::fixed<<std::setprecision(precision)<<item;return stream.str();
        } else if constexpr (std::is_same_v<T,bool>) return item ? "true":"false";
        else if constexpr (std::is_same_v<T,std::string>) {
            std::string text;
            for (const auto c:item) {
                // 只在 UTF-8 字符边界截断，保留末尾字符的全部续字节。
                if (text.size()>=256 && (static_cast<unsigned char>(c)&0xC0)!=0x80) {text+="...";break;}
                if (c=='\n') text+="\\n";else if(c=='\r') text+="\\r";else if(c=='\0') text+="\\0";else text+=c;
            }
            return text;
        } else if constexpr (std::is_same_v<T,data::Bytes>) {
            const std::vector<std::uint8_t> bytes(item.begin(),item.begin()+std::min<std::size_t>(item.size(),64));
            return protocol_utils::bytesToHex(bytes)+(item.size()>64 ? "...":"");
        } else return "?";
    },value->value);
}
data::Value filterValue(data::FieldType type,const char* text,bool boolean)
{
    if (type==data::FieldType::String) return {std::string(text)};
    if (type==data::FieldType::Bool) return {boolean};
    if (type==data::FieldType::Bytes) {
        auto bytes=protocol_utils::hexToBytes(text);
        if (!bytes) throw std::invalid_argument("Invalid HEX bytes");
        return {std::move(*bytes)};
    }
    const auto end=text+std::strlen(text);
    if (type==data::FieldType::Int64) {
        std::int64_t number=0;
        const auto parsed=std::from_chars(text,end,number);
        if (parsed.ec!=std::errc{} || parsed.ptr!=end) throw std::invalid_argument("Invalid int64");
        return {number};
    }
    double number=0;
    const auto parsed=std::from_chars(text,end,number);
    if (parsed.ec!=std::errc{} || parsed.ptr!=end || !std::isfinite(number)) throw std::invalid_argument("Invalid number");
    return {number};
}
bool iconButton(const char* label,const char* tooltip)
{
    const bool clicked=ImGui::Button(label,ImVec2(ImGui::GetFrameHeight(),ImGui::GetFrameHeight()));
    ImGui::SetItemTooltip("%s",tooltip);
    return clicked;
}
}

bool GuiRuntime::drawDataTableControl(const scripting::ControlSnapshot& control)
{
    if (!control.descriptor.dataTable || !control.tablePage || !control.tableView) return false;
    const auto& descriptor=control.descriptor;
    const auto& config=*descriptor.dataTable;
    const auto& page=*control.tablePage;
    if (dataTableUiGeneration_!=descriptor.runtimeGeneration) {
        dataTableUiStates_.clear();dataTableUiGeneration_=descriptor.runtimeGeneration;
    }
    auto& state=dataTableUiStates_[descriptor.id];
    auto view=*control.tableView;
    bool updated=false;
    const auto send=[&](scripting::DataTableAction action,std::uint64_t row=0) {
        application_.interactDataTable({descriptor.id,descriptor.runtimeGeneration,page.revision,action,view,row});
        updated=true;
    };
    const auto scope=descriptor.id+"_"+std::to_string(descriptor.runtimeGeneration);
    ImGui::PushID(scope.c_str());
    ImGui::BeginGroup();
    ImGui::TextUnformatted(descriptor.label.c_str());
    protoscope::ui::beginDisabled(descriptor.disabled || descriptor.readOnly);
    if (!state.initialized && !descriptor.disabled && !descriptor.readOnly) {
        state.initialized=true;
        if (config.history) send(scripting::DataTableAction::Refresh);
    }
    const float width=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(40.0F,(width-ImGui::GetStyle().ItemSpacing.x)*0.6F));
    if (ImGui::BeginCombo("##field",config.columns[state.filterColumn].label.c_str())) {
        for (std::size_t i=0;i<config.columns.size();++i)
            if (ImGui::Selectable(config.columns[i].label.c_str(),state.filterColumn==static_cast<int>(i)))
                state.filterColumn=static_cast<int>(i);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Filter column");
    ImGui::SameLine();
    const char* operations[]={"=","!=","<","<=",">",">=","contains","is null","not null"};
    ImGui::SetNextItemWidth(std::max(40.0F,ImGui::GetContentRegionAvail().x));
    ImGui::Combo("##operation",&state.filterOperation,operations,9);
    ImGui::SetItemTooltip("Filter comparison");
    const float button=ImGui::GetFrameHeight();
    const bool unary=state.filterOperation>=7;
    protoscope::ui::beginDisabled(unary);
    const auto type=config.columns[state.filterColumn].type;
    ImGui::SetNextItemWidth(std::max(30.0F,width-2*button-2*ImGui::GetStyle().ItemSpacing.x));
    if (type==data::FieldType::Bool) ImGui::Checkbox("Value",&state.filterBoolean);
    else ImGui::InputText("##value",state.filterValue.data(),state.filterValue.size());
    ImGui::SetItemTooltip(type==data::FieldType::Bytes ? "HEX filter value":"Filter value");
    protoscope::ui::endDisabled();
    ImGui::SameLine();
    if (iconButton(PROTOSCOPE_ICON_FILTER "##apply","Apply filter")) {
        try {
            data::FieldCondition condition{config.columns[state.filterColumn].field,
                static_cast<data::CompareOp>(state.filterOperation),
                unary ? data::Value{} : filterValue(type,state.filterValue.data(),state.filterBoolean)};
            data::validateConditions({condition});
            view.conditions={std::move(condition)};view.offset=0;state.error.clear();
            send(scripting::DataTableAction::View);
        } catch(const std::exception& error) {state.error=error.what();}
    }
    ImGui::SameLine();
    if (iconButton(PROTOSCOPE_ICON_CLOSE "##clear","Clear filter")) {
        view.conditions.clear();view.offset=0;state.error.clear();send(scripting::DataTableAction::View);
    }
    const auto flags=ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|
        ImGuiTableFlags_Reorderable|ImGuiTableFlags_Hideable|ImGuiTableFlags_Sortable|ImGuiTableFlags_SortTristate|
        ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingFixedFit;
    const ImVec2 size(width,ImGui::GetTextLineHeightWithSpacing()*(config.visibleRows+1)+8);
    if (ImGui::BeginTable("##records",static_cast<int>(config.columns.size()),flags,size)) {
        ImGui::TableSetupScrollFreeze(0,1);
        for (std::size_t i=0;i<config.columns.size();++i) {
            const auto& column=config.columns[i];
            const auto label=column.label+(column.unit.empty() ? "":" ("+column.unit+")");
            ImGuiTableColumnFlags columnFlags=ImGuiTableColumnFlags_WidthFixed;
            if (view.sort && view.sort->field==column.field) {
                columnFlags|=ImGuiTableColumnFlags_DefaultSort;
                if (view.sort->descending) columnFlags|=ImGuiTableColumnFlags_PreferSortDescending;
            }
            ImGui::TableSetupColumn(label.c_str(),columnFlags,120,static_cast<ImGuiID>(i+1));
        }
        ImGui::TableHeadersRow();
        if (auto* sort=ImGui::TableGetSortSpecs();sort && sort->SpecsDirty) {
            std::optional<data::FieldSort> next;
            if (sort->SpecsCount>0) {
                const auto index=sort->Specs[0].ColumnUserID-1;
                if (index<config.columns.size())
                    next=data::FieldSort{config.columns[index].field,sort->Specs[0].SortDirection==ImGuiSortDirection_Descending};
            }
            if (next.has_value()!=view.sort.has_value() ||
                (next && (next->field!=view.sort->field || next->descending!=view.sort->descending))) {
                view.sort=std::move(next);view.offset=0;send(scripting::DataTableAction::View);
            }
            sort->SpecsDirty=false;
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(page.rows.size()));
        while (clipper.Step()) for (int index=clipper.DisplayStart;index<clipper.DisplayEnd;++index) {
            const auto& row=page.rows[static_cast<std::size_t>(index)];
            const auto id=std::to_string(row.id);
            ImGui::PushID(id.c_str());ImGui::TableNextRow();
            bool selectionDrawn=false;
            for (std::size_t column=0;column<config.columns.size();++column) {
                if (!ImGui::TableSetColumnIndex(static_cast<int>(column))) continue;
                const auto text=cellText(data::fieldValue(*row.record,*row.schema,config.columns[column].field),
                                         config.columns[column].precision);
                if (!selectionDrawn) {
                    selectionDrawn=true;
                    const auto cursor=ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##select",std::get<std::string>(control.value)==id,
                                          ImGuiSelectableFlags_SpanAllColumns,ImVec2(0,ImGui::GetTextLineHeight())))
                        send(scripting::DataTableAction::Select,row.id);
                    ImGui::SetCursorScreenPos(cursor);
                }
                ImGui::TextUnformatted(text.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",text.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    protoscope::ui::beginDisabled(view.offset==0 || page.loading);
    if (ImGui::ArrowButton("##previous",ImGuiDir_Left)) {
        view.offset=view.offset>view.limit ? view.offset-view.limit:0;send(scripting::DataTableAction::View);
    }
    ImGui::SetItemTooltip("Previous page");protoscope::ui::endDisabled();ImGui::SameLine();
    protoscope::ui::beginDisabled(!page.more || page.loading);
    if (ImGui::ArrowButton("##next",ImGuiDir_Right)) {view.offset+=view.limit;send(scripting::DataTableAction::View);}
    ImGui::SetItemTooltip("Next page");protoscope::ui::endDisabled();ImGui::SameLine();
    if (iconButton(PROTOSCOPE_ICON_REFRESH "##refresh","Refresh snapshot")) {
        send(scripting::DataTableAction::Refresh);
    }
    ImGui::SameLine();
    protoscope::ui::beginDisabled(!page.loading);
    if (iconButton(PROTOSCOPE_ICON_CLOSE "##cancel","Cancel query")) send(scripting::DataTableAction::Cancel);
    protoscope::ui::endDisabled();ImGui::SameLine();
    if (page.loading) ImGui::TextUnformatted("Loading...");
    else ImGui::Text("%llu-%llu",static_cast<unsigned long long>(page.rows.empty()?0:page.offset+1),
                     static_cast<unsigned long long>(page.offset+page.rows.size()));
    const auto& exporting=control.tableExport;
    protoscope::ui::beginDisabled(exporting.choosingPath || exporting.running || page.loading ||
                         !page.error.empty() || (config.history && !page.snapshot));
    if (iconButton(PROTOSCOPE_ICON_DOWNLOAD "##export","Export matching rows")) ImGui::OpenPopup("##export_format");
    if (ImGui::BeginPopup("##export_format")) {
        if (ImGui::MenuItem("CSV")) send(scripting::DataTableAction::ExportCsv);
        if (ImGui::MenuItem("PSREC")) send(scripting::DataTableAction::ExportPsrec);
        ImGui::EndPopup();
    }
    protoscope::ui::endDisabled();
    ImGui::SameLine();
    protoscope::ui::beginDisabled(!exporting.running);
    if (iconButton(PROTOSCOPE_ICON_CLOSE "##cancel_export","Cancel export"))
        send(scripting::DataTableAction::CancelExport);
    protoscope::ui::endDisabled();
    if (!exporting.message.empty()) ImGui::TextWrapped("%s",exporting.message.c_str());
    protoscope::ui::endDisabled();
    if (!state.error.empty()) ImGui::TextWrapped("%s",state.error.c_str());
    if (!page.error.empty()) ImGui::TextWrapped("%s",page.error.c_str());
    ImGui::EndGroup();ImGui::PopID();
    return updated;
}
} // namespace protoscope::ui
