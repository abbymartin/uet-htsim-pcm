// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PCM_REGISTRY_H
#define PCM_REGISTRY_H

// The compiled PCM handlers, and the names (macros) each handler uses.
//
// A handler file declares its algorithm with PCM_ALGORITHM(name), then the
// YAML macros it uses with PCM_SIGNAL, PCM_VARIABLE and PCM_PARAMETER_*, and
// finally the handler itself with PCM_HANDLER(name); see uet_pcm.h. These
// register the handler under its name at program start. When a YAML file is
// loaded, its cc_algorithm.name selects the handler, and each macro the
// handler uses is matched to the entry with that macro in the YAML file.
//
// Handler authors only need uet_pcm.h; the classes here support its macros.

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "pcm_spec.h"

using PcmHandlerFn = void (*)();

enum class PcmBindingKind { SIGNAL, VARIABLE, PARAMETER };
const char* pcmBindingKindName(PcmBindingKind k);

// One algorithm's handler and the macros it uses.
class PcmAlgorithmInfo {
public:
    struct Binding {
        PcmBindingKind kind;
        std::string macro;
        PcmDtype dtype;  // only used for parameters
    };

    // Registers this algorithm under `name`. Created by PCM_ALGORITHM.
    explicit PcmAlgorithmInfo(const char* name);

    // Records a macro the handler uses; returns its id within this algorithm.
    unsigned addBinding(PcmBindingKind kind, const char* macro,
                        PcmDtype dtype = PcmDtype::UINT);

    // Called by PCM_HANDLER.
    void setHandler(const char* name, PcmHandlerFn fn);

    const std::string& name() const { return _name; }
    PcmHandlerFn handler() const { return _handler; }
    const std::vector<Binding>& bindings() const { return _bindings; }

    // Set if PCM_HANDLER used a different name from PCM_ALGORITHM.
    const std::string& handlerNameMismatch() const { return _handler_name_mismatch; }

private:
    std::string _name;
    PcmHandlerFn _handler = nullptr;
    std::vector<Binding> _bindings;
    std::string _handler_name_mismatch;
};

// The registered algorithm with this name. Throws PcmSpecError if there is none,
// if two handler files register the same name, or if the handler is incomplete.
const PcmAlgorithmInfo& pcmFindAlgorithm(const std::string& name);

// Names of all registered algorithms, sorted.
std::vector<std::string> pcmRegisteredAlgorithms();

// Implemented by the runtime (pcm_runtime.cpp). They resolve a macro for the
// CCC whose handler is running. Calling them outside a handler is a
// programming error and aborts.
unsigned pcmResolveBinding(const PcmAlgorithmInfo& algo, unsigned id);
const PcmValue& pcmResolveParameter(const PcmAlgorithmInfo& algo, unsigned id);

// ---------------------------------------------------------------------------
// Macro objects created by PCM_SIGNAL, PCM_VARIABLE and PCM_PARAMETER_*.

using PcmMaskBits = uint64_t;

// A signal macro. The spec uses a signal macro both as an index, e.g.
// uet_signal_get(S_RTT), and as a bit in
// the fired-trigger mask, e.g. `if (fired_triggers & t_time)` (§3.6.18.5.5). So
// this converts to the signal's index, and combines with a mask as its bit.
class PcmSignalMacro {
public:
    PcmSignalMacro(PcmAlgorithmInfo& algo, const char* macro)
        : _algo(algo), _id(algo.addBinding(PcmBindingKind::SIGNAL, macro)) {}
    operator unsigned int() const { return pcmResolveBinding(_algo, _id); }
    PcmMaskBits bit() const { return PcmMaskBits(1) << unsigned(*this); }

private:
    const PcmAlgorithmInfo& _algo;
    unsigned _id;
};

inline PcmMaskBits operator&(PcmMaskBits m, const PcmSignalMacro& s) { return m & s.bit(); }
inline PcmMaskBits operator&(const PcmSignalMacro& s, PcmMaskBits m) { return m & s.bit(); }
inline PcmMaskBits operator|(const PcmSignalMacro& a, const PcmSignalMacro& b) {
    return a.bit() | b.bit();
}
inline PcmMaskBits operator|(PcmMaskBits m, const PcmSignalMacro& s) { return m | s.bit(); }
inline PcmMaskBits operator|(const PcmSignalMacro& s, PcmMaskBits m) { return m | s.bit(); }

// A variable macro: converts to the variable's index, as in
// uet_get_variable_uint(V_X, &x).
class PcmVariableMacro {
public:
    PcmVariableMacro(PcmAlgorithmInfo& algo, const char* macro)
        : _algo(algo), _id(algo.addBinding(PcmBindingKind::VARIABLE, macro)) {}
    operator unsigned int() const { return pcmResolveBinding(_algo, _id); }

private:
    const PcmAlgorithmInfo& _algo;
    unsigned _id;
};

// A parameter macro: reads as the parameter's value, as in `cwnd += P_AI`. The
// handler declares the type; loading fails if the YAML file declares another.
template <typename T>
class PcmParameterMacro {
public:
    PcmParameterMacro(PcmAlgorithmInfo& algo, const char* macro)
        : _algo(algo), _id(algo.addBinding(PcmBindingKind::PARAMETER, macro, dtype())) {}
    operator T() const { return std::get<T>(pcmResolveParameter(_algo, _id)); }

    static constexpr PcmDtype dtype() {
        if constexpr (std::is_same_v<T, double>)
            return PcmDtype::FLOAT;
        else if constexpr (std::is_same_v<T, int64_t>)
            return PcmDtype::INT;
        else
            return PcmDtype::UINT;
    }

private:
    const PcmAlgorithmInfo& _algo;
    unsigned _id;
};

// Registers the handler function with its algorithm. Created by PCM_HANDLER.
class PcmHandlerRegistrar {
public:
    PcmHandlerRegistrar(PcmAlgorithmInfo& algo, const char* name, PcmHandlerFn fn) {
        algo.setHandler(name, fn);
    }
};

#endif  // PCM_REGISTRY_H
