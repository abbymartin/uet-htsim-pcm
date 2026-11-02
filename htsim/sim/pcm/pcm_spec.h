// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PCM_SPEC_H
#define PCM_SPEC_H

// Loader for PCM algorithm YAML files (UE spec v1.1 §3.6.18.8.5).
//
// The loader reads a YAML file into the plain structs below and checks it. It
// rejects anything the simulator doesn't support, with an error message that
// gives the file and line. It doesn't run anything; the runtime builds on the
// result.
//
// Everything that differs from the spec is explained in docs/pcm-plan.md §7
// and in comments at the code below.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "pcm_signals.h"

class PcmSpecError : public std::runtime_error {
public:
    explicit PcmSpecError(const std::string& msg) : std::runtime_error(msg) {}
};

// Accumulation operators, Tables 3-106 and 3-107.
enum class PcmAccOp { SUM, MAX, MIN, LAST, EWMA };

// Trigger comparison operators, §3.6.18.5.1 (YAML names from §3.6.18.8.5).
enum class PcmTriggerOp { EQ, NEQ, GT, GTE, LT, LTE, WC };

// RAW is the spec's trigger: compare the accumulator's value. DELTA (compare
// the change since the trigger's last satisfied check) and MAGNITUDE (compare
// the absolute value of that change) are not in the spec; they are the PCM
// paper's other trigger modes (Khalilov et al. §5.1, used in its App. B YAML).
// See docs/pcm-plan.md §7.
enum class PcmTriggerType { RAW, DELTA, MAGNITUDE };

enum class PcmArmMode { DISARMED, ONCE, CONTINUOUS };
enum class PcmTriggerAction { INVOKE, RECORD };
enum class PcmDtype { UINT, INT, FLOAT };

// A value of a variable or parameter. Which alternative is held matches the
// declared dtype: uint64_t for uint, int64_t for int, double for float.
using PcmValue = std::variant<uint64_t, int64_t, double>;

struct PcmTriggerSpec {
    PcmTriggerOp op = PcmTriggerOp::WC;
    int64_t op_val = 0;  // ignored for op wc
    PcmTriggerType type = PcmTriggerType::RAW;
    uint64_t ref_count = 0;  // mode_refval, first value
    uint64_t ref_time = 0;   // mode_refval, second value, in 128 ns units
    PcmArmMode arm_mode = PcmArmMode::CONTINUOUS;
    PcmTriggerAction action = PcmTriggerAction::INVOKE;
    uint64_t invoke_deadline = 0;  // in 128 ns units
};

struct PcmSignalSpec {
    std::string name;  // spec signal name, e.g. "uet_rx_ack_bytes"
    const PcmSignalInfo* info = nullptr;
    std::string macro;  // name used in the handler code
    PcmAccOp accumulator = PcmAccOp::LAST;
    double accumulator_const = 0;  // smoothing factor s, only for EWMA
    std::optional<PcmTriggerSpec> trigger;
    int line = 0;  // 1-based line in the YAML file
};

struct PcmControlSpec {
    std::string name;  // e.g. "uet_tx_cwnd"
    std::string macro;
    uint64_t initial_value = 0;
    int line = 0;
};

struct PcmVariableSpec {
    std::string macro;
    PcmDtype dtype = PcmDtype::UINT;
    PcmValue initial_value = uint64_t(0);
    int line = 0;
};

struct PcmParameterSpec {
    std::string macro;
    PcmDtype dtype = PcmDtype::UINT;
    PcmValue value = uint64_t(0);
    std::string description;
    int line = 0;
};

struct PcmAlgorithmSpec {
    std::string name;  // name of the handler function
    std::string description;
    uint64_t max_ack_coalescing = 0;
    std::optional<uint64_t> uint_min_bitwidth;
    std::optional<uint64_t> int_min_bitwidth;
    std::optional<uint64_t> float_min_bitwidth;
};

struct PcmSpec {
    std::string source;  // file name, used in messages
    PcmAlgorithmSpec algorithm;
    std::vector<PcmSignalSpec> signals;
    std::vector<PcmControlSpec> controls;
    std::vector<PcmVariableSpec> variables;
    std::vector<PcmParameterSpec> parameters;

    // Features used by this file that are additions to the spec, one line
    // each, so a run's log shows whether it used only spec features.
    std::vector<std::string> extensions_used;

    // Things that are valid but probably a mistake, e.g. no trigger that can
    // ever call the handler.
    std::vector<std::string> warnings;
};

// Parse YAML text. source_name is used in error messages. Throws PcmSpecError.
PcmSpec pcmParseSpec(const std::string& yaml_text, const std::string& source_name);

// Read and parse a YAML file. Throws PcmSpecError.
PcmSpec pcmLoadSpecFile(const std::string& path);

const char* pcmAccOpName(PcmAccOp op);
const char* pcmTriggerOpName(PcmTriggerOp op);

#endif  // PCM_SPEC_H
