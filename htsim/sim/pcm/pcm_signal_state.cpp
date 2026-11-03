// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_signal_state.h"

#include <algorithm>
#include <string>

PcmSignalState::PcmSignalState(const PcmSpec& spec, PcmTime now) {
    if (spec.signals.size() > kMaxEntries)
        throw PcmSpecError(spec.source + ": " + std::to_string(spec.signals.size()) +
                           " signal entries; at most " + std::to_string(kMaxEntries) +
                           " are supported, so that each fits one bit of the trigger mask");

    _entries.reserve(spec.signals.size());
    for (size_t i = 0; i < spec.signals.size(); i++) {
        const PcmSignalSpec& s = spec.signals[i];
        _entries.push_back(Entry{&s, PcmAccumulator(s.accumulator, s.accumulator_const), nullptr});
        if (s.trigger)
            _entries.back().trigger =
                std::make_unique<PcmTrigger>(*s.trigger, _entries.back().acc, now);
        _by_signal[s.info].push_back(i);
    }
}

PcmFired PcmSignalState::applyEvent(const std::vector<PcmSignalUpdate>& updates,
                                    PcmTime now) {
    _counters.events++;

    // Apply every update first, remembering which entries changed.
    PcmTriggerMask updated = 0;
    for (const auto& u : updates) {
        auto it = _by_signal.find(u.signal);
        if (it == _by_signal.end())
            continue;
        for (size_t i : it->second) {
            _entries[i].acc.update(u.value);
            updated |= bit(i);
            _counters.signal_updates++;
        }
    }

    // Then check only the triggers whose own signal was updated. The spec
    // (§3.6.18.5.3) says every received packet checks all armed triggers on
    // uet_rx_* signals, every sent packet all triggers on uet_tx_* signals, and
    // every timer event all triggers on uet_time_* signals. Then an "every 8
    // ACKs" trigger would also count NACKs, and the handler's fired-trigger mask
    // would be misleading. We check a trigger only when its signal was updated
    // by the event, as the paper's htsim version does (paper Fig. 4). An update
    // with value 0 still counts. See docs/pcm-plan.md §7.
    PcmFired fired;
    for (size_t i = 0; i < _entries.size(); i++) {
        Entry& e = _entries[i];
        if (!(updated & bit(i)) || !e.trigger)
            continue;
        _counters.evaluations++;
        if (!e.trigger->evaluate(e.acc, now))
            continue;
        _counters.fires++;
        fired.mask |= bit(i);
        const PcmTriggerSpec& t = e.trigger->spec();
        if (t.action != PcmTriggerAction::INVOKE)
            continue;
        // invoke_deadline is an "upper bound on how long an invoke action may be
        // deferred after trigger fires" (§3.6.18.5.1). §3.6.18.5.3 says the
        // CCC's deadline is updated "for every evaluated trigger", which we read
        // as every trigger that fired, since a trigger that didn't fire has no
        // invoke action to defer. See docs/pcm-plan.md §7.
        if (t.invoke_deadline == 0) {
            fired.invoke_now = true;
        } else {
            PcmTime by = now + pcmTimeUnitsToPs(t.invoke_deadline);
            fired.invoke_by = fired.invoke_by ? std::min(*fired.invoke_by, by) : by;
        }
    }
    return fired;
}

void PcmSignalState::refresh(const std::vector<PcmSignalUpdate>& updates) {
    for (const auto& u : updates) {
        auto it = _by_signal.find(u.signal);
        if (it == _by_signal.end())
            continue;
        for (size_t i : it->second) {
            _entries[i].acc.update(u.value);
            _counters.signal_updates++;
        }
    }
}

std::optional<PcmTime> PcmSignalState::nextTimeDue() const {
    std::optional<PcmTime> due;
    for (const auto& e : _entries) {
        if (!e.trigger || e.spec->info->dir != PcmSignalDir::TIME)
            continue;
        auto t = e.trigger->timeDue();
        if (t && (!due || *t < *due))
            due = t;
    }
    return due;
}
