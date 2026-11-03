// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PCM_ACCUMULATOR_H
#define PCM_ACCUMULATOR_H

// One accumulated signal value (UE spec v1.1 §3.6.18.4.1). Each signal entry in
// an algorithm's YAML file has one accumulator, which combines the signal's
// updates using its operator: running sum, maximum, minimum, last value, or
// exponentially weighted moving average.

#include <cstdint>
#include <limits>

#include "pcm_spec.h"

// Value of a MIN accumulator before its first update. A handler reading this
// knows no sample has arrived yet; setting a MIN to it restarts the minimum.
constexpr int64_t kPcmMinStartValue = std::numeric_limits<int64_t>::max();

class PcmAccumulator {
public:
    // smoothing is the EWMA factor s; it is only used for PcmAccOp::EWMA.
    explicit PcmAccumulator(PcmAccOp op, double smoothing = 0);

    // Combine a new signal update with the current value.
    void update(int64_t x);

    // uet_signal_set(): replace the value. Handlers use this to reset a
    // running total, e.g. to get "since the last call" values.
    void set(int64_t x);

    // The value as an integer, as returned by uet_signal_get(). An EWMA value
    // is rounded to the nearest integer.
    int64_t get() const;

    // The value as a float. Only differs from get() for EWMA.
    double getFloat() const;

    PcmAccOp op() const { return _op; }

    // Accumulators never reset by themselves (docs/pcm-plan.md §7).
    //
    // The spec doesn't define a signal's value before its first update; its
    // YAML format has no initial value for signals. Each accumulator starts at
    // the value that has no effect on its operation, so the first update needs
    // no special case: SUM, LAST and MAX start at 0 (spec signals are unsigned,
    // so 0 is below every sample), and MIN starts at kPcmMinStartValue. An EWMA
    // reads 0 until its first update, which is stored as it is, since the
    // average has nothing to start from. See docs/pcm-plan.md §7.

private:
    PcmAccOp _op;
    double _s;
    int64_t _value;              // used by every operator except EWMA
    double _fvalue = 0;          // used by EWMA
    bool _ewma_started = false;  // EWMA has had its first update
};

#endif  // PCM_ACCUMULATOR_H
