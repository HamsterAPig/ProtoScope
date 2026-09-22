#pragma once

#include "protoscope/scripting/script_host.hpp"

namespace protoscope::ui {

struct ControlEditState {
    scripting::ControlValue value{false};
    std::uint64_t generation{0};
    bool editing{false};
    bool dirty{false};
    int lastFrame{-1};

    void cancel(const scripting::ControlSnapshot& snapshot)
    {
        editing = false;
        prepare(snapshot);
    }

    void prepare(const scripting::ControlSnapshot& snapshot, int frame = -1)
    {
        if (frame >= 0 && lastFrame != frame && lastFrame != frame - 1) editing = false;
        lastFrame = frame;
        // 编辑中保留草稿；代次变化、禁用或只读则取消草稿，不能提交给另一运行时。
        if (!editing || generation != snapshot.descriptor.runtimeGeneration ||
            value.index() != snapshot.value.index() || snapshot.descriptor.disabled || snapshot.descriptor.readOnly) {
            value = snapshot.value;
            generation = snapshot.descriptor.runtimeGeneration;
            editing = false;
            dirty = false;
        }
    }

    bool finish(bool changed, bool active, bool explicitCommit, scripting::ControlCommitMode mode)
    {
        dirty = dirty || changed;
        editing = active;
        const bool submit = dirty && (mode == scripting::ControlCommitMode::Change || explicitCommit || !active);
        if (submit) dirty = false;
        return submit;
    }
};
} // namespace protoscope::ui
