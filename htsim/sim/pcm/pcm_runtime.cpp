// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>

#include "uet_pcm.h"

static_assert(UET_SIGNAL_MIN_EMPTY == kPcmMinStartValue,
              "UET_SIGNAL_MIN_EMPTY must match the MIN accumulator's starting value");

// ---------------------------------------------------------------------------
// PcmAlgorithm

PcmAlgorithm::PcmAlgorithm(PcmSpec spec) : _spec(std::move(spec)) {
    _info = &pcmFindAlgorithm(_spec.algorithm.name);

    // Every macro in the file, with its kind and index.
    struct Target {
        PcmBindingKind kind;
        unsigned index;
    };
    std::map<std::string, Target> targets;
    for (size_t i = 0; i < _spec.signals.size(); i++)
        targets[_spec.signals[i].macro] = {PcmBindingKind::SIGNAL, unsigned(i)};
    for (size_t i = 0; i < _spec.variables.size(); i++)
        targets[_spec.variables[i].macro] = {PcmBindingKind::VARIABLE, unsigned(i)};
    for (size_t i = 0; i < _spec.parameters.size(); i++)
        targets[_spec.parameters[i].macro] = {PcmBindingKind::PARAMETER, unsigned(i)};

    auto dtypeName = [](PcmDtype d) {
        return d == PcmDtype::UINT ? "uint" : d == PcmDtype::INT ? "int" : "float";
    };

    std::map<std::string, bool> used;
    for (const auto& b : _info->bindings()) {
        const std::string where = _spec.source + ": handler '" + _info->name() + "' uses ";
        auto it = targets.find(b.macro);
        if (it == targets.end())
            throw PcmSpecError(where + pcmBindingKindName(b.kind) + " macro " + b.macro +
                               ", which the file doesn't define");
        if (it->second.kind != b.kind)
            throw PcmSpecError(where + b.macro + " as a " + pcmBindingKindName(b.kind) +
                               ", but the file defines it as a " +
                               pcmBindingKindName(it->second.kind));
        if (b.kind == PcmBindingKind::PARAMETER) {
            PcmDtype file_dtype = _spec.parameters[it->second.index].dtype;
            if (file_dtype != b.dtype)
                throw PcmSpecError(where + "parameter " + b.macro + " as " + dtypeName(b.dtype) +
                                   ", but the file declares it as " + dtypeName(file_dtype));
        }
        _targets.push_back(it->second.index);
        used[b.macro] = true;
    }
    for (const auto& kv : targets)
        if (!used.count(kv.first))
            _warnings.push_back(_spec.source + ": " + pcmBindingKindName(kv.second.kind) +
                                " macro " + kv.first + " is not used by handler '" +
                                _info->name() + "'");

    for (size_t i = 0; i < _spec.controls.size(); i++)
        if (_spec.controls[i].name == "uet_tx_cwnd")
            _cwnd_control = i;
}

// ---------------------------------------------------------------------------
// PcmCcc

namespace {

PcmCcc* g_current = nullptr;

// Sets the current CCC for the duration of a handler call.
class CurrentCcc {
public:
    explicit CurrentCcc(PcmCcc* c) : _prev(g_current) { g_current = c; }
    ~CurrentCcc() { g_current = _prev; }

private:
    PcmCcc* _prev;
};

PcmValue initialValue(const PcmVariableSpec& v) { return v.initial_value; }

}  // namespace

bool PcmCcc::print_errors = true;

PcmCcc* PcmCcc::current() { return g_current; }

PcmCcc::PcmCcc(std::shared_ptr<const PcmAlgorithm> algo, PcmTime now, std::string name)
    : _algo(std::move(algo)), _name(std::move(name)), _signals(_algo->spec(), now) {
    for (const auto& v : _algo->spec().variables)
        _variables.push_back(initialValue(v));
    if (auto c = _algo->cwndControl())
        _cwnd = _algo->spec().controls[*c].initial_value;
}

void PcmCcc::onEvent(const std::vector<PcmSignalUpdate>& updates, PcmTime now) {
    if (_error)
        return;
    PcmFired f = _signals.applyEvent(updates, now);
    _pending_mask |= f.mask;
    if (f.invoke_by)
        _deadline = _deadline ? std::min(*_deadline, *f.invoke_by) : *f.invoke_by;
    if (f.invoke_now)
        invoke(now);
}

void PcmCcc::invoke(PcmTime now) {
    if (_error)
        return;
    _counters.invocations++;

    // All fired triggers are delivered in this one call (§3.6.18.5.3), including
    // those whose invoke_deadline would have allowed a later call.
    _delivered_mask = _pending_mask;
    _pending_mask = 0;
    _deadline.reset();

    _snapshot.clear();
    for (size_t i = 0; i < _signals.size(); i++)
        _snapshot.push_back(_signals.accumulator(i));
    _ops.clear();

    {
        CurrentCcc guard(this);
        _algo->info().handler()();
    }

    // "Regardless of completion or abortion, it MUST NOT produce any externally
    // visible side effects of any kind" (§3.6.18.7.2.2).
    if (_error) {
        _ops.clear();
        return;
    }

    for (const Op& op : _ops) {
        switch (op.kind) {
        case Op::SIGNAL_SET:
            _signals.set(op.index, op.value);
            break;
        case Op::SIGNAL_UPDATE:
            _signals.update(op.index, op.value);
            break;
        case Op::CWND:
            _cwnd = uint64_t(op.value);
            _counters.cwnd_updates++;
            break;
        }
    }
    _ops.clear();
    _snapshot.clear();
}

void PcmCcc::fail(PcmErrorSource source, int value, const std::string& message) {
    if (_error)
        return;
    _error = PcmFatalError{source, value, message};
    _ops.clear();
    if (print_errors)
        std::cerr << "PCM: CCC " << (_name.empty() ? "(unnamed)" : _name) << " running '"
                  << _algo->info().name() << "' entered the ERROR state ("
                  << (source == PcmErrorSource::PCM_ALGORITHM ? "algorithm" : "implementation")
                  << " error " << value << "): " << message << std::endl;
}

bool PcmCcc::checkSignal(unsigned idx) {
    if (idx < _signals.size())
        return true;
    fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_SIGNAL_INDEX,
         "signal index " + std::to_string(idx) + " is out of range (the file has " +
             std::to_string(_signals.size()) + " signal entries)");
    return false;
}

int64_t PcmCcc::apiSignalGet(unsigned idx) {
    if (_error || !checkSignal(idx))
        return 0;
    return _snapshot[idx].get();
}

int PcmCcc::apiSignalSet(unsigned idx, int64_t value, bool update) {
    if (_error || !checkSignal(idx))
        return -ENOTRECOVERABLE;
    if (update)
        _snapshot[idx].update(value);
    else
        _snapshot[idx].set(value);
    Op op;
    op.kind = update ? Op::SIGNAL_UPDATE : Op::SIGNAL_SET;
    op.index = idx;
    op.value = value;
    _ops.push_back(op);
    return 0;
}

int PcmCcc::apiSetCwnd(unsigned value) {
    if (_error)
        return -ENOTRECOVERABLE;
    if (!_algo->cwndControl()) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_NO_CWND_CONTROL,
             "uet_set_cwnd: the file declares no uet_tx_cwnd control");
        return -ENOTRECOVERABLE;
    }
    // A window of 0 would stop the flow for good, so it's treated as an invalid
    // operation (§3.6.18.7.1). See docs/pcm-plan.md §7.
    if (value == 0) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_ZERO_CWND, "uet_set_cwnd(0)");
        return -ENOTRECOVERABLE;
    }
    Op op;
    op.kind = Op::CWND;
    op.value = value;
    _ops.push_back(op);
    return 0;
}

int PcmCcc::apiGetVariable(unsigned idx, PcmDtype dtype, PcmValue* out) {
    if (_error)
        return -ENOTRECOVERABLE;
    if (idx >= _variables.size()) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_VARIABLE_INDEX,
             "variable index " + std::to_string(idx) + " is out of range");
        return -ENOTRECOVERABLE;
    }
    if (!out) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_NULL_POINTER,
             "uet_get_variable_*: value pointer is null");
        return -ENOTRECOVERABLE;
    }
    if (_algo->spec().variables[idx].dtype != dtype) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_VARIABLE_TYPE,
             "variable " + _algo->spec().variables[idx].macro +
                 " read with a function for a different dtype");
        return -ENOTRECOVERABLE;
    }
    *out = _variables[idx];
    return 0;
}

int PcmCcc::apiSetVariable(unsigned idx, PcmValue value) {
    if (_error)
        return -ENOTRECOVERABLE;
    if (idx >= _variables.size()) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_VARIABLE_INDEX,
             "variable index " + std::to_string(idx) + " is out of range");
        return -ENOTRECOVERABLE;
    }
    if (_variables[idx].index() != value.index()) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_VARIABLE_TYPE,
             "variable " + _algo->spec().variables[idx].macro +
                 " set with a function for a different dtype");
        return -ENOTRECOVERABLE;
    }
    // Local variables are private to this CCC, so they change at once.
    _variables[idx] = value;
    return 0;
}


int PcmCcc::apiTriggerMask(PcmTriggerMask* out) {
    if (_error)
        return -ENOTRECOVERABLE;
    if (!out) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_NULL_POINTER,
             "uet_trigger_mask: mask pointer is null");
        return -ENOTRECOVERABLE;
    }
    *out = _delivered_mask;
    return 0;
}

int PcmCcc::apiTriggerDropMask(uint32_t* out) {
    if (_error)
        return -ENOTRECOVERABLE;
    if (!out) {
        fail(PcmErrorSource::IMPLEMENTATION, PCM_ERR_NULL_POINTER,
             "uet_trigger_drop_mask: mask pointer is null");
        return -ENOTRECOVERABLE;
    }
    *out = 0;  // no events are ever dropped (docs/pcm-plan.md §4.4)
    return 0;
}

void PcmCcc::apiFatal(int reason) {
    fail(PcmErrorSource::PCM_ALGORITHM, reason,
         "uet_ccc_fatal(" + std::to_string(reason) + ")");
}

// ---------------------------------------------------------------------------
// Macro resolution (pcm_registry.h)

namespace {

PcmCcc& currentOrAbort(const char* what) {
    PcmCcc* c = PcmCcc::current();
    if (!c) {
        std::cerr << "PCM: " << what << " used outside a running handler" << std::endl;
        std::abort();
    }
    return *c;
}

}  // namespace

unsigned pcmResolveBinding(const PcmAlgorithmInfo& algo, unsigned id) {
    PcmCcc& c = currentOrAbort("a PCM macro");
    if (&c.algorithm().info() != &algo) {
        std::cerr << "PCM: a macro of handler '" << algo.name() << "' was used while handler '"
                  << c.algorithm().info().name() << "' was running" << std::endl;
        std::abort();
    }
    return c.algorithm().bindingTarget(id);
}

const PcmValue& pcmResolveParameter(const PcmAlgorithmInfo& algo, unsigned id) {
    unsigned i = pcmResolveBinding(algo, id);
    return currentOrAbort("a PCM parameter").algorithm().parameterValue(i);
}

// ---------------------------------------------------------------------------
// The uet_* functions (uet_pcm.h)

uet_int_t uet_signal_get(unsigned int signal_index) {
    return currentOrAbort("uet_signal_get").apiSignalGet(signal_index);
}

int uet_signal_set(unsigned int signal_index, uet_int_t value) {
    return currentOrAbort("uet_signal_set").apiSignalSet(signal_index, value, false);
}

int uet_signal_update(unsigned int signal_index, uet_int_t update) {
    return currentOrAbort("uet_signal_update").apiSignalSet(signal_index, update, true);
}

int uet_trigger_mask(uet_trigmask_t* mask) {
    return currentOrAbort("uet_trigger_mask").apiTriggerMask(mask);
}

int uet_trigger_drop_mask(uint32_t* trigger_mask) {
    return currentOrAbort("uet_trigger_drop_mask").apiTriggerDropMask(trigger_mask);
}

int uet_set_cwnd(unsigned int value) { return currentOrAbort("uet_set_cwnd").apiSetCwnd(value); }

namespace {

template <typename T>
int getVariable(const char* what, unsigned idx, PcmDtype dtype, T* value) {
    PcmValue v;
    int r = currentOrAbort(what).apiGetVariable(idx, dtype, value ? &v : nullptr);
    if (r == 0)
        *value = std::get<T>(v);
    return r;
}

}  // namespace

int uet_get_variable_int(unsigned int variable_index, uet_int_t* value) {
    return getVariable("uet_get_variable_int", variable_index, PcmDtype::INT, value);
}
int uet_get_variable_uint(unsigned int variable_index, uet_uint_t* value) {
    return getVariable("uet_get_variable_uint", variable_index, PcmDtype::UINT, value);
}
int uet_get_variable_float(unsigned int variable_index, uet_float_t* value) {
    return getVariable("uet_get_variable_float", variable_index, PcmDtype::FLOAT, value);
}
int uet_set_variable_int(unsigned int variable_index, uet_int_t value) {
    return currentOrAbort("uet_set_variable_int").apiSetVariable(variable_index, value);
}
int uet_set_variable_uint(unsigned int variable_index, uet_uint_t value) {
    return currentOrAbort("uet_set_variable_uint").apiSetVariable(variable_index, value);
}
int uet_set_variable_float(unsigned int variable_index, uet_float_t value) {
    return currentOrAbort("uet_set_variable_float").apiSetVariable(variable_index, value);
}

void uet_ccc_fatal(int reason) { currentOrAbort("uet_ccc_fatal").apiFatal(reason); }
