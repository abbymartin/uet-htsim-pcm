// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_trigger.h"

#include <cmath>
#include <limits>

PcmTime pcmTimeUnitsToPs(uint64_t units) {
    if (units > std::numeric_limits<PcmTime>::max() / kPcmTimeUnitPs)
        return std::numeric_limits<PcmTime>::max();
    return units * kPcmTimeUnitPs;
}

namespace {

template <typename T>
bool applyOp(PcmTriggerOp op, T a, T b) {
    switch (op) {
    case PcmTriggerOp::EQ: return a == b;
    case PcmTriggerOp::NEQ: return a != b;
    case PcmTriggerOp::GT: return a > b;
    case PcmTriggerOp::GTE: return a >= b;
    case PcmTriggerOp::LT: return a < b;
    case PcmTriggerOp::LTE: return a <= b;
    case PcmTriggerOp::WC: return true;
    }
    return false;
}

}  // namespace

PcmTrigger::PcmTrigger(const PcmTriggerSpec& spec, const PcmAccumulator& acc, PcmTime now)
    : _spec(spec), _last_fire(now), _baseline(acc.getFloat()) {}

// The comparison `accumulator (op) op_val` (docs/pcm-plan.md §7: the spec is
// inconsistent about what a trigger compares against; we always use op_val).
// For DELTA and MAGNITUDE triggers, which come from the PCM paper (§5.1), the
// left-hand side is the change since the last true comparison, and a true
// comparison moves the baseline to the current value.
bool PcmTrigger::compare(const PcmAccumulator& acc) {
    if (_spec.op == PcmTriggerOp::WC)
        return true;

    bool result;
    if (_spec.type == PcmTriggerType::RAW) {
        // Compare integers exactly, except for EWMA, which is a float.
        if (acc.op() == PcmAccOp::EWMA)
            result = applyOp(_spec.op, acc.getFloat(), double(_spec.op_val));
        else
            result = applyOp(_spec.op, acc.get(), _spec.op_val);
    } else {
        double change = acc.getFloat() - _baseline;
        if (_spec.type == PcmTriggerType::MAGNITUDE)
            change = std::fabs(change);
        result = applyOp(_spec.op, change, double(_spec.op_val));
        if (result)
            _baseline = acc.getFloat();
    }
    return result;
}

bool PcmTrigger::evaluate(const PcmAccumulator& acc, PcmTime now) {
    if (!_armed)
        return false;

    const PcmTime ref_time = pcmTimeUnitsToPs(_spec.ref_time);
    bool fire = false;
    if (_spec.ref_count == 0 && ref_time == 0) {
        // "No evaluation is performed; trigger fires immediately if armed"
        // (§3.6.18.5.1).
        fire = true;
    } else {
        if (_spec.ref_count > 0 && compare(acc)) {
            ++_true_count;
            if (_true_count >= _spec.ref_count)
                fire = true;
        }
        // The time condition is "elapsed time since the trigger last fired"
        // (§3.6.18.5.1); it doesn't depend on the comparison.
        if (ref_time > 0 && now - _last_fire >= ref_time)
            fire = true;
    }

    if (fire) {
        // ONCE: "when it fires it automatically transitions to DISARMED".
        // CONTINUOUS: "after firing, the count of true evaluations and elapsed
        // time since last fire are reset and evaluation continues".
        if (_spec.arm_mode == PcmArmMode::ONCE)
            _armed = false;
        _true_count = 0;
        _last_fire = now;
    }
    return fire;
}

std::optional<PcmTime> PcmTrigger::timeDue() const {
    if (!_armed || _spec.ref_time == 0)
        return std::nullopt;
    const PcmTime ref_time = pcmTimeUnitsToPs(_spec.ref_time);
    if (_last_fire > std::numeric_limits<PcmTime>::max() - ref_time)
        return std::numeric_limits<PcmTime>::max();
    return _last_fire + ref_time;
}
