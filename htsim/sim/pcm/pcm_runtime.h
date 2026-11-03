// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PCM_RUNTIME_H
#define PCM_RUNTIME_H

// Calling PCM handlers (UE spec v1.1 §3.6.18.2, §3.6.18.6, §3.6.18.7).
//
// PcmAlgorithm is a loaded YAML file together with its compiled handler, with
// the handler's macros matched to the file's entries. Several CCCs can share
// one PcmAlgorithm.
//
// PcmCcc is the state of one CCC: its signals and triggers, local variables,
// controls (the congestion window), and ERROR state. The datapath passes it
// events; when a trigger asks for it, PcmCcc calls the handler:
//   1. the fired-trigger mask since the last call is delivered and cleared;
//   2. the signal values are frozen in a snapshot (§3.6.18.2);
//   3. the handler runs, using the uet_* functions in uet_pcm.h;
//   4. the handler's signal writes and control settings are applied, in the
//      order it made them ("control operations are executed in issued order",
//      §3.6.18.9).
// In asynchronous mode (a handler time T > 0, set with setHandlerTime), step 4
// happens T after the call starts, at a timer event. Until then the CCC is
// busy: events still update signals and fire triggers, and a call asked for
// meanwhile starts when the busy one finishes. This models a NIC that takes T
// to run the handler (PCM paper §5.3, §7.3).
// If the handler calls uet_ccc_fatal() or uses the API wrongly, the CCC enters
// the ERROR state: nothing from that call is applied, and the CCC never calls
// its handler again (§3.6.18.7).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pcm_registry.h"
#include "pcm_signal_state.h"
#include "pcm_spec.h"

class PcmAlgorithm {
public:
    // Finds the handler named by the file's cc_algorithm.name and matches each
    // macro the handler uses to the file's entry with that macro. Throws
    // PcmSpecError if there is no such handler, or if the handler uses a macro
    // the file doesn't define, or defines with a different kind or type.
    explicit PcmAlgorithm(PcmSpec spec);
    PcmAlgorithm(const PcmAlgorithm&) = delete;
    PcmAlgorithm& operator=(const PcmAlgorithm&) = delete;

    const PcmSpec& spec() const { return _spec; }
    const PcmAlgorithmInfo& info() const { return *_info; }

    // For handler macro `id`: the index of its signal entry, variable or
    // parameter in the file.
    unsigned bindingTarget(unsigned id) const { return _targets[id]; }

    const PcmValue& parameterValue(size_t i) const { return _spec.parameters[i].value; }

    // Index in spec().controls of the uet_tx_cwnd control, if the file has one.
    std::optional<size_t> cwndControl() const { return _cwnd_control; }

    // Macros the file defines but the handler doesn't use.
    const std::vector<std::string>& warnings() const { return _warnings; }

private:
    PcmSpec _spec;
    const PcmAlgorithmInfo* _info;
    std::vector<unsigned> _targets;
    std::optional<size_t> _cwnd_control;
    std::vector<std::string> _warnings;
};

// Who caused a CCC's ERROR state (§3.6.18.7.3).
enum class PcmErrorSource { IMPLEMENTATION, PCM_ALGORITHM };

// Error values for errors found by the simulator. The spec leaves these
// "implementation-defined" (§3.6.18.7.3).
enum PcmImplementationError {
    PCM_ERR_SIGNAL_INDEX = 1,   // signal index out of range
    PCM_ERR_VARIABLE_INDEX,     // variable index out of range
    PCM_ERR_VARIABLE_TYPE,      // get/set function doesn't match the variable's dtype
    PCM_ERR_NO_CWND_CONTROL,    // uet_set_cwnd without a uet_tx_cwnd control
    PCM_ERR_ZERO_CWND,          // uet_set_cwnd(0)
    PCM_ERR_NULL_POINTER,       // null pointer argument
};

// The CCC's single recorded fatal error (§3.6.18.7.3).
struct PcmFatalError {
    PcmErrorSource source;
    int value;  // the reason passed to uet_ccc_fatal, or a PcmImplementationError
    std::string message;
};

class PcmCcc {
public:
    // `units` are the units handlers see signal values in. Signal updates
    // passed to onEvent() must already be in those units.
    PcmCcc(std::shared_ptr<const PcmAlgorithm> algo, PcmTime now, std::string name = "",
           PcmUnits units = PcmUnits::SPEC);
    PcmCcc(const PcmCcc&) = delete;
    PcmCcc& operator=(const PcmCcc&) = delete;

    // Apply one event's signal updates. Calls the handler if an INVOKE trigger
    // with no invoke_deadline fired. Does nothing in the ERROR state.
    void onEvent(const std::vector<PcmSignalUpdate>& updates, PcmTime now);

    // Call the handler now, delivering every trigger fired since the last call.
    // Does nothing in the ERROR state.
    void invoke(PcmTime now);

    // --- Timers ---
    //
    // A CCC needs to act at given times: when a trigger on a uet_time_* signal
    // is due, and when an invoke_deadline is reached. It asks the datapath for a
    // call to onTimer() at that time through the scheduler function. The
    // datapath may call onTimer() at other times too; a call when nothing is
    // due does nothing.

    // Called with the time at which onTimer() should next be called. A new
    // request may be earlier than one made before; a later or repeated call to
    // onTimer() is harmless.
    using Scheduler = std::function<void(PcmTime when)>;
    void setScheduler(Scheduler scheduler, PcmTime now);

    // A timer event: update the uet_time_* signals and check their triggers,
    // then call the handler if an invoke_deadline has been reached.
    void onTimer(PcmTime now);

    // When onTimer() next needs to be called, if at all.
    std::optional<PcmTime> nextWakeup() const;

    // How long each handler call takes. 0 (the default) means synchronous:
    // the handler's changes take effect at once. Anything else is asynchronous
    // mode (see the top of this file); it needs a scheduler.
    void setHandlerTime(PcmTime t);
    PcmTime handlerTime() const { return _handler_time; }

    // A handler call is in progress (asynchronous mode only).
    bool busy() const { return _busy_until.has_value(); }

    PcmUnits units() const { return _units; }

    // The congestion window set by the handler, or the uet_tx_cwnd control's
    // initial_value. Empty if the file has no uet_tx_cwnd control.
    std::optional<uint64_t> cwnd() const { return _cwnd; }

    bool inError() const { return _error.has_value(); }
    const std::optional<PcmFatalError>& fatalError() const { return _error; }

    // A fired trigger asked for the handler to be called by this time.
    std::optional<PcmTime> invokeDeadline() const { return _deadline; }

    // Triggers fired since the handler was last called.
    PcmTriggerMask pendingMask() const { return _pending_mask; }

    const PcmSignalState& signals() const { return _signals; }
    const PcmAlgorithm& algorithm() const { return *_algo; }
    const PcmValue& variable(size_t i) const { return _variables[i]; }
    const std::string& name() const { return _name; }

    struct Counters {
        uint64_t invocations = 0;           // handler calls
        uint64_t deadline_invocations = 0;  // of which made because a deadline was reached
        uint64_t timer_events = 0;          // onTimer() calls
        uint64_t cwnd_updates = 0;          // uet_set_cwnd calls applied
        uint64_t requests_while_busy = 0;   // calls asked for while one was in
                                            // progress (asynchronous mode)
    };
    const Counters& counters() const { return _counters; }

    // Print a message when a CCC enters the ERROR state (§3.6.18.7.3: "SHOULD
    // emit a diagnostic message"). On by default.
    static bool print_errors;

    // --- Used by the uet_* functions while this CCC's handler runs. ---

    // The CCC whose handler is running, or nullptr.
    static PcmCcc* current();

    int64_t apiSignalGet(unsigned idx);
    int apiSignalSet(unsigned idx, int64_t value, bool update);
    int apiTriggerMask(PcmTriggerMask* out);
    int apiTriggerDropMask(uint32_t* out);
    int apiSetCwnd(unsigned value);
    int apiGetVariable(unsigned idx, PcmDtype dtype, PcmValue* out);
    int apiSetVariable(unsigned idx, PcmValue value);
    void apiFatal(int reason);
    bool apiInError() const { return inError(); }

private:
    // Enter the ERROR state (once; later errors are ignored, §3.6.18.7.3).
    void fail(PcmErrorSource source, int value, const std::string& message);
    bool checkSignal(unsigned idx);
    // The uet_time_* signal values at `now`, in handler units.
    std::vector<PcmSignalUpdate> timeSignals(PcmTime now) const;
    // Ask the scheduler for an onTimer() call if one is needed earlier than
    // already requested.
    void requestWakeup(PcmTime now);
    // Apply the handler's changes from the last call.
    void commit(PcmTime now);

    // A change made by the handler, applied when it returns.
    struct Op {
        enum Kind { SIGNAL_SET, SIGNAL_UPDATE, CWND } kind;
        unsigned index = 0;
        int64_t value = 0;
    };

    std::shared_ptr<const PcmAlgorithm> _algo;
    std::string _name;
    PcmUnits _units;
    PcmSignalState _signals;
    std::vector<PcmValue> _variables;
    std::optional<uint64_t> _cwnd;
    std::optional<PcmFatalError> _error;

    PcmTriggerMask _pending_mask = 0;  // fired since the last call
    std::optional<PcmTime> _deadline;

    Scheduler _scheduler;
    std::optional<PcmTime> _scheduled;  // earliest onTimer() call requested
    PcmTime _last_update = 0;           // last event that updated a signal

    PcmTime _handler_time = 0;          // 0: synchronous
    std::optional<PcmTime> _busy_until; // a call in progress finishes then
    bool _invoke_pending = false;       // a call was asked for while busy

    // Only valid during an invocation (_ops until its changes are applied).
    PcmTriggerMask _delivered_mask = 0;
    std::vector<PcmAccumulator> _snapshot;
    std::vector<Op> _ops;

    Counters _counters;
};

#endif  // PCM_RUNTIME_H
