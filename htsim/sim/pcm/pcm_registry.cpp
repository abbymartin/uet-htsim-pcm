// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_registry.h"

#include <map>

namespace {

// Algorithms by name. A function-local static, so it exists before any handler
// file's static objects register themselves, whatever the initialization order.
std::map<std::string, std::vector<PcmAlgorithmInfo*>>& registry() {
    static std::map<std::string, std::vector<PcmAlgorithmInfo*>> r;
    return r;
}

}  // namespace

const char* pcmBindingKindName(PcmBindingKind k) {
    switch (k) {
    case PcmBindingKind::SIGNAL: return "signal";
    case PcmBindingKind::VARIABLE: return "variable";
    case PcmBindingKind::PARAMETER: return "parameter";
    }
    return "?";
}

PcmAlgorithmInfo::PcmAlgorithmInfo(const char* name) : _name(name) {
    registry()[_name].push_back(this);
}

unsigned PcmAlgorithmInfo::addBinding(PcmBindingKind kind, const char* macro, PcmDtype dtype) {
    _bindings.push_back(Binding{kind, macro, dtype});
    return unsigned(_bindings.size() - 1);
}

void PcmAlgorithmInfo::setHandler(const char* name, PcmHandlerFn fn) {
    // Errors can't be thrown during program start, so a mismatch is reported
    // when the algorithm is looked up.
    if (_name != name)
        _handler_name_mismatch = name;
    _handler = fn;
}

const PcmAlgorithmInfo& pcmFindAlgorithm(const std::string& name) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        std::string list;
        for (const auto& n : pcmRegisteredAlgorithms())
            list += (list.empty() ? "" : ", ") + n;
        throw PcmSpecError("no PCM handler named '" + name +
                           "' is compiled into the simulator (available: " +
                           (list.empty() ? "none" : list) + ")");
    }
    if (it->second.size() > 1)
        throw PcmSpecError("more than one PCM handler file declares PCM_ALGORITHM(" + name + ")");
    const PcmAlgorithmInfo& algo = *it->second[0];
    if (!algo.handlerNameMismatch().empty())
        throw PcmSpecError("PCM handler file declares PCM_ALGORITHM(" + name +
                           ") but PCM_HANDLER(" + algo.handlerNameMismatch() + ")");
    if (!algo.handler())
        throw PcmSpecError("PCM handler file declares PCM_ALGORITHM(" + name +
                           ") but has no PCM_HANDLER(" + name + ")");
    return algo;
}

std::vector<std::string> pcmRegisteredAlgorithms() {
    std::vector<std::string> names;
    for (const auto& kv : registry())
        names.push_back(kv.first);
    return names;  // std::map keeps them sorted
}
