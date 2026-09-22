#include "control_data_binding.hpp"
#include "control_properties.hpp"
#include "industrial_control.hpp"
#include "script_host_internal.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace protoscope::scripting {
bool parseControlBinding(ControlDescriptor& control, const sol::table& table, std::string& error)
{
    const sol::object object = table["binding"];
    if (!object.valid() || object.get_type() == sol::type::lua_nil) return true;
    try {
        if (!isOutputControl(control.type) || object.get_type() != sol::type::table)
            throw std::invalid_argument("binding requires an output control and a table");
        const auto entry = object.as<sol::table>();
        for (const auto& [key,value] : entry) {
            if (key.get_type() != sol::type::string) throw std::invalid_argument("invalid binding key");
            const auto name=key.as<std::string>();
            if (name!="dataset" && name!="field" && name!="device")
                throw std::invalid_argument("unknown binding property");
        }
        auto text=[&](const char* key) {
            const sol::object value=entry[key];
            if (value.get_type()!=sol::type::string) throw std::invalid_argument("binding requires string fields");
            const auto result=value.as<std::string>();
            if (result.size()>4096 || result.find('\0')!=result.npos) throw std::invalid_argument("invalid binding text");
            return result;
        };
        ControlFieldBinding binding;
        binding.dataset=text("dataset"); binding.field=text("field");
        if (binding.dataset.empty() || binding.field.empty()) throw std::invalid_argument("binding fields must not be empty");
        const sol::object device=entry["device"];
        if (device.valid() && device.get_type()!=sol::type::lua_nil) binding.device=text("device");
        control.binding=std::move(binding);
        return true;
    } catch (const std::exception& exception) {error=exception.what();return false;}
}

void validateControlBindings(std::vector<DockDescriptor>& docks, const std::vector<data::Schema>& schemas)
{
    for (auto& dock:docks) for (auto& control:dock.controls) {
        if (!control.binding) continue;
        auto& binding=*control.binding;
        const auto schema=std::find_if(schemas.begin(),schemas.end(),[&](const auto& s) {return s.dataset==binding.dataset;});
        if (schema==schemas.end()) throw std::invalid_argument("unknown binding dataset: "+binding.dataset);
        const auto field=std::find_if(schema->fields.begin(),schema->fields.end(),[&](const auto& f) {return f.name==binding.field;});
        if (field==schema->fields.end()) throw std::invalid_argument("unknown binding field: "+binding.field);
        const auto type=field->type;
        const bool numeric=type==data::FieldType::Int64 || type==data::FieldType::Double;
        if ((control.type==ControlType::Readout && !numeric && type!=data::FieldType::String) ||
            (control.type==ControlType::Label && type!=data::FieldType::String) ||
            (control.type==ControlType::Indicator && type!=data::FieldType::Bool) ||
            (control.type==ControlType::Progress && !numeric))
            throw std::invalid_argument("incompatible binding field type: "+control.id);
        binding.fieldIndex=static_cast<std::size_t>(field-schema->fields.begin());
    }
}

void ScriptHost::configureDataBindings()
{
    for (std::size_t i=0;i<controls_.size();++i) {
        if (controls_[i].binding) {
            runtime_->boundControls[controls_[i].binding->dataset].push_back(i);
            runtime_->bindingStatus.emplace(controls_[i].id,ControlBindingStatus{});
        }
    }
    runtime_->data->setPublishObserver([this](const data::Record& record) {applyPublishedRecord(record);});
}

void ScriptHost::applyPublishedRecord(const data::Record& record)
{
    runtime_->tables->publish(record);
    const auto subscribers=runtime_->boundControls.find(record.dataset);
    if (subscribers==runtime_->boundControls.end()) return;
    for (const auto index:subscribers->second) {
        const auto& control=controls_[index];
        const auto& binding=*control.binding;
        if (binding.device && *binding.device!=record.device) continue;
        auto& status=runtime_->bindingStatus.at(control.id);
        runtime_->controlUpdatedAtMs[control.id]=static_cast<std::uint64_t>(record.receivedAtUs/1000);
        const auto& field=record.values.at(binding.fieldIndex).value;
        // 空值和转换失败均清除旧读数，不能把旧设备值冒充当前记录。
        controlValues_[control.id]=defaultValueFor(control);
        status={ControlDataState::Null,{}};
        if (std::holds_alternative<std::monostate>(field)) continue;
        try {
            ControlValue value;
            if (control.type==ControlType::Readout) {
                sol::state_view lua(runtime_->lua.lua_state());
                sol::object object=std::visit([&](const auto& item)->sol::object {
                    using T=std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T,std::int64_t> || std::is_same_v<T,double> || std::is_same_v<T,std::string>)
                        return sol::make_object(lua,item);
                    else return sol::make_object(lua,sol::nil);
                },field);
                std::string error;
                auto formatted=formatReadout(object,control.precision,error);
                if (!formatted) throw std::invalid_argument(error);
                value=std::move(*formatted);
            } else if (control.type==ControlType::Label) value=std::get<std::string>(field);
            else if (control.type==ControlType::Indicator) value=std::get<bool>(field);
            else {
                const auto* integer=std::get_if<std::int64_t>(&field);
                const double number=integer ? static_cast<double>(*integer) : std::get<double>(field);
                if (number < -std::numeric_limits<float>::max() || number > std::numeric_limits<float>::max())
                    throw std::invalid_argument("value exceeds display range");
                value=static_cast<float>(number);
            }
            if (!validateControlValue(control,value)) throw std::invalid_argument("value violates display constraints");
            controlValues_[control.id]=std::move(value);
            status={ControlDataState::Valid,{}};
        } catch (const std::exception& exception) {
            // 显示转换错误不阻断原始类型化记录入队，也不能因日志刷屏拖慢数据链路。
            status={ControlDataState::Invalid,exception.what()};
        }
    }
}

ControlSnapshot ScriptHost::makeControlSnapshot(const ControlDescriptor& control) const
{
    const auto iter=controlValues_.find(control.id);
    ControlSnapshot snapshot{
        .descriptor=control,
        .value=iter==controlValues_.end() ? defaultValueFor(control) : iter->second,
        .updatedAtMs=runtime_->controlUpdatedAtMs.contains(control.id) ? runtime_->controlUpdatedAtMs.at(control.id) : 0,
    };
    if (control.binding) {
        const auto& status=runtime_->bindingStatus.at(control.id);
        snapshot.dataState=status.state;
        snapshot.dataError=status.error;
    }
    if (control.dataTable) {
        snapshot.tablePage=runtime_->tables->page(control.id);
        snapshot.tableView=runtime_->tables->view(control.id);
    }
    return snapshot;
}
} // namespace protoscope::scripting
