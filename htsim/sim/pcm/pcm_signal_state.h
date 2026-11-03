// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PCM_SIGNAL_STATE_H
#define PCM_SIGNAL_STATE_H

// The signal values and triggers of one CCC, built from an algorithm's loaded
// YAML file. There is one entry per signal entry in the file, in file order, so
// a signal listed twice (two accumulators, §3.6.18.4.1) has two entries.
//
// Entry i is identified in the trigger mask by bit (1 << i). The handler code
// compares uet_trigger_mask() against these bits, using the signal macros from
// the YAML file (§3.6.18.5.5).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "pcm_accumulator.h"
#include "pcm_spec.h"
#include "pcm_trigger.h"

using PcmTriggerMask = uint64_t;

// The value of one signal from one event, e.g. uet_rx_ack_bytes = 4096 from an
// ACK. The value is already in the units the handler sees.
struct PcmSignalUpdate {
    const PcmSignalInfo* signal;
    int64_t value;
};

// What happened to the triggers during one event.
struct PcmFired {
    // Bits of all triggers that fired, both INVOKE and RECORD.
    PcmTriggerMask mask = 0;
    // An INVOKE trigger with invoke_deadline 0 fired: call the handler now.
    bool invoke_now = false;
    // An INVOKE trigger with a non-zero invoke_deadline fired: the handler must
    // be called by this time (the earliest such deadline, §3.6.18.5.3).
    std::optional<PcmTime> invoke_by;
};

class PcmSignalState {
public:
    // At most this many signal entries, so each fits one bit of the trigger
    // mask.
    static constexpr size_t kMaxEntries = 64;

    // Throws PcmSpecError if the file has more than kMaxEntries signal entries.
    // Keeps pointers into `spec`, which must outlive this object.
    PcmSignalState(const PcmSpec& spec, PcmTime now);

    // Apply all signal updates from one event (one received or sent packet, or
    // one timer event), then check the triggers. All updates are applied before
    // any trigger is checked, so that updates from one packet take effect
    // together (§3.6.18.4.1). Updates to signals the file doesn't use are
    // ignored.
    //
    // Every event goes through this function, so a model of skipped events
    // (§3.6.18.9) can be added here later.
    PcmFired applyEvent(const std::vector<PcmSignalUpdate>& updates, PcmTime now);

    // Apply updates without checking any triggers. Used to give the time
    // signals their current value before the handler is called.
    void refresh(const std::vector<PcmSignalUpdate>& updates);

    size_t size() const { return _entries.size(); }
    static PcmTriggerMask bit(size_t i) { return PcmTriggerMask(1) << i; }

    const PcmSignalSpec& entrySpec(size_t i) const { return *_entries[i].spec; }
    const PcmAccumulator& accumulator(size_t i) const { return _entries[i].acc; }
    // nullptr if entry i has no trigger.
    PcmTrigger* trigger(size_t i) { return _entries[i].trigger.get(); }
    const PcmTrigger* trigger(size_t i) const { return _entries[i].trigger.get(); }

    // uet_signal_get / uet_signal_set / uet_signal_update for entry i.
    int64_t get(size_t i) const { return _entries[i].acc.get(); }
    void set(size_t i, int64_t value) { _entries[i].acc.set(value); }
    void update(size_t i, int64_t value) { _entries[i].acc.update(value); }

    // Earliest time at which an armed trigger on a uet_time_* signal meets its
    // time condition. Only these are checked on timer events (§3.6.18.5.3), so
    // the runtime schedules a timer event for this time. A trigger on another
    // signal checks its time condition when that signal is next updated.
    std::optional<PcmTime> nextTimeDue() const;

    struct Counters {
        uint64_t events = 0;           // applyEvent() calls
        uint64_t signal_updates = 0;   // accumulator updates applied
        uint64_t evaluations = 0;      // trigger evaluations
        uint64_t fires = 0;            // triggers fired
    };
    const Counters& counters() const { return _counters; }

private:
    struct Entry {
        const PcmSignalSpec* spec;
        PcmAccumulator acc;
        std::unique_ptr<PcmTrigger> trigger;
    };
    std::vector<Entry> _entries;
    // Signal -> its entries.
    std::map<const PcmSignalInfo*, std::vector<size_t>> _by_signal;
    Counters _counters;
};

#endif  // PCM_SIGNAL_STATE_H
