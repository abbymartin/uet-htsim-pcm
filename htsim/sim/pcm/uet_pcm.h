// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef UET_PCM_H
#define UET_PCM_H

// The PCM API for algorithm handlers (UE spec v1.1 §3.6.18). A handler file
// includes only this header.
//
// Example:
//
//   #include "uet_pcm.h"
//
//   PCM_ALGORITHM(my_cc);        // same name as cc_algorithm.name in the YAML
//   PCM_SIGNAL(T_ACK);           // macros from the YAML file
//   PCM_SIGNAL(S_ACKED);
//   PCM_VARIABLE(V_PREV_ACKED);
//   PCM_PARAMETER_UINT(P_MTU);
//
//   PCM_HANDLER(my_cc) {
//       uet_trigmask_t fired;
//       uet_trigger_mask(&fired);
//       if (fired & T_ACK) {
//           uet_uint_t prev;
//           uet_get_variable_uint(V_PREV_ACKED, &prev);
//           ...
//           uet_set_cwnd(cwnd);
//       }
//   }
//
// The function names, types and argument lists are the spec's. Where the spec
// leaves something undefined (the trigger attribute structure, error codes),
// the choice made here is explained below and in docs/pcm-plan.md §7.
//
// Each function acts on the CCC whose handler is running; calling one outside a
// handler aborts the program. Invalid use, such as an out-of-range index or the
// wrong variable type, puts the CCC into the ERROR state (§3.6.18.7.1). After
// that, every function fails with -ENOTRECOVERABLE and has no effect.

#include <cerrno>
#include <climits>
#include <cstdint>

#include "pcm_registry.h"

typedef int64_t uet_int_t;
typedef uint64_t uet_uint_t;
typedef double uet_float_t;
typedef uint64_t uet_trigmask_t;

// Value of a signal with a MIN accumulator before its first update (docs/
// pcm-plan.md §7). Setting a MIN signal to this restarts the minimum.
#define UET_SIGNAL_MIN_EMPTY INT64_MAX

// --- Signals (§3.6.18.4.2) --------------------------------------------------
//
// Values are read from a snapshot taken when the handler was called
// (§3.6.18.2). uet_signal_set and uet_signal_update change the snapshot at once,
// so the handler sees its own changes, and change the CCC's signal when the
// handler returns.

uet_int_t uet_signal_get(unsigned int signal_index);
int uet_signal_set(unsigned int signal_index, uet_int_t value);
int uet_signal_update(unsigned int signal_index, uet_int_t update);

// --- Triggers (§3.6.18.5.5, §3.6.18.5.6) -------------------------------------

// The fired triggers delivered with this invocation, as bits that can be
// tested with the signal macros: `if (fired & T_ACK)`.
//
// The spec gives no signature for this function, and is inconsistent about its
// form (§3.6.18.5.5). The text says uet_trigger_mask() "returns an unsigned
// integer", but the spec's example calls `uet_trigger_mask(&fired_triggers)`
// and reads the mask from the pointer. We use the pointer form, as in the
// example, because it matches the spec's other getters such as
// uet_get_variable_int(), which fill in a pointer and return a status code.
// See docs/pcm-plan.md §7.
int uet_trigger_mask(uet_trigmask_t* mask);

// Triggers whose events were dropped. This simulator never drops events, so the
// mask is always 0.
int uet_trigger_drop_mask(uint32_t* trigger_mask);

// The spec's uet_modify_trigger() and uet_query_trigger() (Table 3-114) are not
// provided: none of the algorithms we plan to implement change their triggers
// while running, and the spec leaves their argument structure undefined. A
// trigger's configuration comes only from the YAML file. See docs/pcm-plan.md §7.

// --- Controls (§3.6.18.6.1) --------------------------------------------------

// Set the congestion window in bytes. Takes effect when the handler returns.
// The YAML file must declare the uet_tx_cwnd control. A window of 0 is an
// invalid operation (docs/pcm-plan.md §7).
int uet_set_cwnd(unsigned int value);

// --- Variables (§3.6.18.6.2) -------------------------------------------------
//
// Local variables keep their values between invocations. The function must
// match the variable's dtype in the YAML file.
//
// The spec's uet_update_variable_* functions are not provided: they only
// update global variables, which this simulator doesn't support yet
// (docs/pcm-plan.md §7).

int uet_get_variable_int(unsigned int variable_index, uet_int_t* value);
int uet_get_variable_uint(unsigned int variable_index, uet_uint_t* value);
int uet_get_variable_float(unsigned int variable_index, uet_float_t* value);
int uet_set_variable_int(unsigned int variable_index, uet_int_t value);
int uet_set_variable_uint(unsigned int variable_index, uet_uint_t value);
int uet_set_variable_float(unsigned int variable_index, uet_float_t value);

// --- Errors (§3.6.18.7.1.1) --------------------------------------------------

// Put the CCC into the ERROR state. Nothing the handler did in this invocation
// takes effect.
void uet_ccc_fatal(int reason);

// --- Declaring a handler -----------------------------------------------------
//
// The spec leaves how handlers are loaded to the implementation (§3.6.18.8.1).
// In this simulator, handlers are compiled in and declared with these macros.
// Each handler file declares one algorithm. The macro names must match the
// `macro:` names in the YAML file; loading fails if the handler uses a macro
// the file doesn't define, or of a different kind or type.

#define PCM_ALGORITHM(name) static PcmAlgorithmInfo pcm_this_algorithm(#name)

#define PCM_SIGNAL(macro) static const PcmSignalMacro macro(pcm_this_algorithm, #macro)
#define PCM_VARIABLE(macro) static const PcmVariableMacro macro(pcm_this_algorithm, #macro)
#define PCM_PARAMETER_UINT(macro) \
    static const PcmParameterMacro<uet_uint_t> macro(pcm_this_algorithm, #macro)
#define PCM_PARAMETER_INT(macro) \
    static const PcmParameterMacro<uet_int_t> macro(pcm_this_algorithm, #macro)
#define PCM_PARAMETER_FLOAT(macro) \
    static const PcmParameterMacro<uet_float_t> macro(pcm_this_algorithm, #macro)

// The handler function, named after the algorithm (the entry point named by
// cc_algorithm.name, §3.6.18.8.5).
#define PCM_HANDLER(name)                                                               \
    static void name();                                                                 \
    static const PcmHandlerRegistrar pcm_handler_registrar_##name(pcm_this_algorithm,   \
                                                                  #name, &name);        \
    static void name()

#endif  // UET_PCM_H
