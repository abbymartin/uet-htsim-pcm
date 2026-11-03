// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PCM_TRIGGER_H
#define PCM_TRIGGER_H

// A PCM trigger (UE spec v1.1 §3.6.18.5): a state machine that watches one
// accumulator and decides when the handler should be called.
//
// Each time the trigger's signal is updated, evaluate() is called. Depending
// on the trigger's (count, time) reference values (the YAML mode_refval), it
// fires:
//   (0, 0)  on every evaluation ("no evaluation" mode)
//   (0, T)  when at least T has passed since it last fired ("time-only")
//   (C, 0)  after C evaluations where the comparison was true ("count-only")
//   (C, T)  whichever of the two happens first ("combined")
// After firing, a CONTINUOUS trigger resets its count and time and keeps
// going. A ONCE trigger disarms itself and never fires again, since triggers
// can't be changed (re-armed) at runtime here.

#include <cstdint>
#include <optional>

#include "pcm_accumulator.h"
#include "pcm_spec.h"

// Simulation time in picoseconds, the same unit as htsim's simtime_picosec.
using PcmTime = uint64_t;

// Times in a PCM YAML file (mode_refval time, invoke_deadline) are in units of
// 128 ns (Table 3-113).
constexpr PcmTime kPcmTimeUnitPs = 128000;
PcmTime pcmTimeUnitsToPs(uint64_t units);

class PcmTrigger {
public:
    // `acc` is the accumulator this trigger watches; its current value is the
    // starting point for DELTA and MAGNITUDE triggers. `now` is the starting
    // point for the time condition.
    PcmTrigger(const PcmTriggerSpec& spec, const PcmAccumulator& acc, PcmTime now);

    // Call after the watched accumulator was updated. Returns true if the
    // trigger fired.
    bool evaluate(const PcmAccumulator& acc, PcmTime now);

    const PcmTriggerSpec& spec() const { return _spec; }
    bool armed() const { return _armed; }

    // Number of true comparisons counted towards the count condition since it
    // last fired.
    uint64_t trueCount() const { return _true_count; }

    // When the time condition will be met, if the trigger has one and is
    // armed. A time trigger is only checked when it is evaluated, so the
    // runtime uses this to schedule an evaluation.
    std::optional<PcmTime> timeDue() const;

private:
    bool compare(const PcmAccumulator& acc);

    PcmTriggerSpec _spec;
    bool _armed = true;
    uint64_t _true_count = 0;
    PcmTime _last_fire = 0;  // or the time the trigger was created
    double _baseline = 0;    // DELTA / MAGNITUDE: value at the last true comparison
};

#endif  // PCM_TRIGGER_H
