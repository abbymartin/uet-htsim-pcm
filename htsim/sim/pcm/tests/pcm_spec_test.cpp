// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_spec.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::HasSubstr;

namespace {

// A small valid file: a per-ACK trigger, one control, one variable, one
// parameter. Tests below change parts of it.
const std::string kHeader = R"(
cc_algorithm:
  name: test_algo
  description: "test algorithm"
  max_ack_coalescing: 8
)";

const std::string kAckSignal = R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger:
      op: wc
      mode: count_or_time
      mode_refval: 1 0
      arm_mode: continuous
      action: invoke
      invoke_deadline: 0
)";

const std::string kControl = R"(
control:
  - name: uet_tx_cwnd
    macro: C_CWND
    initial_value: 16600
)";

const std::string kRest = R"(
variable:
  - macro: V_ALPHA
    dtype: float
    initial_value: 1.0
parameter:
  - macro: P_G
    dtype: float
    value: 0.0625
    description: "DCTCP gain"
)";

std::string validFile() { return kHeader + kAckSignal + kControl + kRest; }

// Parses `text` and returns the error message, or "" if it parsed.
std::string errorOf(const std::string& text) {
    try {
        pcmParseSpec(text, "test.yaml");
    } catch (const PcmSpecError& e) {
        return e.what();
    }
    return "";
}

// A file with the given signal section and the standard rest.
std::string withSignals(const std::string& signals) {
    return kHeader + signals + kControl;
}

}  // namespace

TEST(PcmSpecTest, ParsesValidFile) {
    PcmSpec s = pcmParseSpec(validFile(), "test.yaml");

    EXPECT_EQ(s.source, "test.yaml");
    EXPECT_EQ(s.algorithm.name, "test_algo");

    ASSERT_EQ(s.signals.size(), 1u);
    const PcmSignalSpec& sig = s.signals[0];
    EXPECT_EQ(sig.name, "uet_rx_ack");
    EXPECT_EQ(sig.macro, "T_ACK");
    ASSERT_NE(sig.info, nullptr);
    EXPECT_EQ(sig.info->dir, PcmSignalDir::RX);
    EXPECT_EQ(sig.accumulator, PcmAccOp::SUM);
    ASSERT_TRUE(sig.trigger.has_value());
    EXPECT_EQ(sig.trigger->op, PcmTriggerOp::WC);
    EXPECT_EQ(sig.trigger->type, PcmTriggerType::RAW);
    EXPECT_EQ(sig.trigger->ref_count, 1u);
    EXPECT_EQ(sig.trigger->ref_time, 0u);
    EXPECT_EQ(sig.trigger->arm_mode, PcmArmMode::CONTINUOUS);
    EXPECT_EQ(sig.trigger->action, PcmTriggerAction::INVOKE);
    EXPECT_EQ(sig.trigger->invoke_deadline, 0u);

    ASSERT_EQ(s.controls.size(), 1u);
    EXPECT_EQ(s.controls[0].name, "uet_tx_cwnd");
    EXPECT_EQ(s.controls[0].initial_value, 16600u);

    ASSERT_EQ(s.variables.size(), 1u);
    EXPECT_EQ(s.variables[0].macro, "V_ALPHA");
    EXPECT_EQ(s.variables[0].dtype, PcmDtype::FLOAT);
    EXPECT_DOUBLE_EQ(std::get<double>(s.variables[0].initial_value), 1.0);

    ASSERT_EQ(s.parameters.size(), 1u);
    EXPECT_EQ(s.parameters[0].macro, "P_G");
    EXPECT_DOUBLE_EQ(std::get<double>(s.parameters[0].value), 0.0625);

    EXPECT_TRUE(s.extensions_used.empty());
    EXPECT_TRUE(s.warnings.empty());
}

// Fields the spec's YAML format requires but the simulator doesn't use may be
// left out: cc_algorithm description and max_ack_coalescing, a control's macro,
// a parameter's description, and a trigger's mode.
TEST(PcmSpecTest, UnusedSpecFieldsAreOptional) {
    PcmSpec s = pcmParseSpec(R"(
cc_algorithm:
  name: minimal
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode_refval: 1 0, arm_mode: continuous, action: invoke,
              invoke_deadline: 0}
control:
  - name: uet_tx_cwnd
    initial_value: 4096
parameter:
  - {macro: P_X, dtype: uint, value: 3}
)", "minimal.yaml");
    EXPECT_EQ(s.algorithm.name, "minimal");
    EXPECT_EQ(s.controls[0].initial_value, 4096u);
    EXPECT_EQ(std::get<uint64_t>(s.parameters[0].value), 3u);
    EXPECT_TRUE(s.warnings.empty());

    // When present they are still checked.
    EXPECT_THAT(errorOf(R"(
cc_algorithm:
  name: minimal
  max_ack_coalescing: many
)" + kAckSignal + kControl), HasSubstr("max_ack_coalescing must be an unsigned integer"));
    EXPECT_THAT(errorOf(kHeader + kAckSignal + R"(
control:
  - {name: uet_tx_cwnd, macro: 9bad, initial_value: 1}
)"), HasSubstr("must be a C identifier"));
}

// The spec's 2 ms timer example (§3.6.18.8.5): short accumulator name
// "uet_last", lowercase enum values, a single mapping instead of a list, and no
// `scope`.
TEST(PcmSpecTest, ParsesSpecTimerExample) {
    PcmSpec s = pcmParseSpec(kHeader + R"(
signal:
  name: uet_time_current
  macro: t_time
  accumulator: uet_last
  trigger:
    op: wc
    mode: count_or_time
    mode_refval: 0 15625
    arm_mode: continuous
    action: invoke
    invoke_deadline: 0
)" + kControl, "timer.yaml");
    ASSERT_EQ(s.signals.size(), 1u);
    EXPECT_EQ(s.signals[0].info->dir, PcmSignalDir::TIME);
    EXPECT_EQ(s.signals[0].accumulator, PcmAccOp::LAST);
    EXPECT_EQ(s.signals[0].trigger->ref_count, 0u);
    EXPECT_EQ(s.signals[0].trigger->ref_time, 15625u);
}

// `scope` may be left out (both of the spec's signal examples do); CCC is the
// only value accepted. Upper-case enum values, as in the spec's YAML format.
TEST(PcmSpecTest, ScopeIsOptional) {
    PcmSpec s = pcmParseSpec(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    scope: CCC
    accumulator: uet_op_sum
    trigger:
      op: wc
      mode: COUNT_OR_TIME
      mode_refval: 1 0
      arm_mode: CONTINUOUS
      action: INVOKE
      invoke_deadline: 7
)"), "test.yaml");
    EXPECT_EQ(s.signals[0].trigger->invoke_deadline, 7u);
}

// `invoke_deadline` is required, as in the spec's YAML format.
TEST(PcmSpecTest, InvokeDeadlineRequired) {
    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: invoke}
)")), HasSubstr("missing required key 'invoke_deadline'"));
}

TEST(PcmSpecTest, SignalWithoutTrigger) {
    PcmSpec s = pcmParseSpec(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: invoke, invoke_deadline: 0}
  - name: uet_rx_ack_rtt
    macro: S_RTT
    accumulator: uet_op_min
)"), "test.yaml");
    ASSERT_EQ(s.signals.size(), 2u);
    EXPECT_FALSE(s.signals[1].trigger.has_value());
    EXPECT_EQ(s.signals[1].accumulator, PcmAccOp::MIN);
}

// §3.6.18.4.1: several accumulators may be defined for the same signal.
TEST(PcmSpecTest, SeveralAccumulatorsForOneSignal) {
    PcmSpec s = pcmParseSpec(withSignals(R"(
signal:
  - name: uet_rx_ack_bytes
    macro: S_ACKED_TOTAL
    accumulator: uet_op_sum
    trigger: {op: wc, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: invoke, invoke_deadline: 0}
  - name: uet_rx_ack_bytes
    macro: S_ACKED_LAST
    accumulator: uet_op_last
    trigger: {op: gt, op_val: 0, mode: count_or_time, mode_refval: 1 0,
              arm_mode: continuous, action: record, invoke_deadline: 0}
)"), "test.yaml");
    ASSERT_EQ(s.signals.size(), 2u);
    EXPECT_EQ(s.signals[1].trigger->op, PcmTriggerOp::GT);
    EXPECT_EQ(s.signals[1].trigger->op_val, 0);
    EXPECT_EQ(s.signals[1].trigger->action, PcmTriggerAction::RECORD);
}

TEST(PcmSpecTest, EwmaNeedsSmoothingFactor) {
    PcmSpec s = pcmParseSpec(withSignals(kAckSignal.substr(0, kAckSignal.size()) + R"(
  - name: uet_rx_ack_rtt
    macro: S_RTT_AVG
    accumulator: uet_op_ewma
    accumulator_const: 1.25e-1
)"), "test.yaml");
    EXPECT_DOUBLE_EQ(s.signals[1].accumulator_const, 0.125);

    EXPECT_THAT(errorOf(withSignals(kAckSignal + R"(
  - name: uet_rx_ack_rtt
    macro: S_RTT_AVG
    accumulator: uet_op_ewma
)")), HasSubstr("missing required key 'accumulator_const'"));

    EXPECT_THAT(errorOf(withSignals(kAckSignal + R"(
  - name: uet_rx_ack_rtt
    macro: S_RTT_AVG
    accumulator: uet_op_ewma
    accumulator_const: 1.5
)")), HasSubstr("at most 1"));

    EXPECT_THAT(errorOf(withSignals(kAckSignal + R"(
  - name: uet_rx_ack_rtt
    macro: S_RTT
    accumulator: uet_op_last
    accumulator_const: 0.5
)")), HasSubstr("only used with uet_op_ewma"));
}

TEST(PcmSpecTest, NonSpecFeaturesAreReported) {
    PcmSpec s = pcmParseSpec(withSignals(kAckSignal + R"(
  - name: uet_rx_ack_bytes
    macro: S_ACKED
    accumulator: uet_op_sum
    trigger: {op: gte, op_val: 65536, trigger_type: delta, mode: count_or_time,
              mode_refval: 1 0, arm_mode: continuous, action: invoke, invoke_deadline: 0}
  - name: uet_rx_nack_vend_htsim_bytes
    macro: S_NACKED
    accumulator: uet_op_sum
)"), "test.yaml");
    EXPECT_EQ(s.signals[1].trigger->type, PcmTriggerType::DELTA);
    ASSERT_EQ(s.extensions_used.size(), 2u);
    EXPECT_THAT(s.extensions_used[0], HasSubstr("trigger_type delta"));
    EXPECT_THAT(s.extensions_used[1], HasSubstr("uet_rx_nack_vend_htsim_bytes"));
}

TEST(PcmSpecTest, HexAndNegativeNumbers) {
    PcmSpec s = pcmParseSpec(withSignals(R"(
signal:
  - name: uet_rx_ack_rtt
    macro: S_RTT
    accumulator: uet_op_last
    trigger: {op: lt, op_val: -0x10, mode: count_or_time, mode_refval: 0x2 0,
              arm_mode: once, action: invoke, invoke_deadline: 0}
)"), "test.yaml");
    EXPECT_EQ(s.signals[0].trigger->op_val, -16);
    EXPECT_EQ(s.signals[0].trigger->ref_count, 2u);
    EXPECT_EQ(s.signals[0].trigger->arm_mode, PcmArmMode::ONCE);
}

// Our reading of the spec: op_val is required for every operator except wc.
TEST(PcmSpecTest, ComparisonNeedsOpVal) {
    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_ack_rtt
    macro: S_RTT
    accumulator: uet_op_last
    trigger: {op: gt, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: invoke, invoke_deadline: 0}
)")), HasSubstr("missing required key 'op_val'"));
}

TEST(PcmSpecTest, OpValWithWildcardWarns) {
    PcmSpec s = pcmParseSpec(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, op_val: 3, mode: count_or_time, mode_refval: 1 0,
              arm_mode: continuous, action: invoke, invoke_deadline: 0}
)"), "test.yaml");
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_THAT(s.warnings[0], HasSubstr("op_val is ignored"));
}

TEST(PcmSpecTest, WarnsWhenHandlerCanNeverRun) {
    PcmSpec s = pcmParseSpec(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: record, invoke_deadline: 0}
)"), "test.yaml");
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_THAT(s.warnings[0], HasSubstr("never be called"));
}

// Triggers can't be changed at runtime, so a disarmed one could never fire.
TEST(PcmSpecTest, RejectsDisarmedTrigger) {
    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode_refval: 1 0, arm_mode: DISARMED, action: invoke,
              invoke_deadline: 0}
)")), HasSubstr("arm_mode disarmed is not supported"));
}

TEST(PcmSpecTest, RejectsBadSignals) {
    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_akc
    macro: T_ACK
    accumulator: uet_op_sum
)")), HasSubstr("unknown signal 'uet_rx_akc'"));

    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_credit
    macro: S_CREDIT
    accumulator: uet_op_sum
)")), HasSubstr("defined by the spec but not supported"));

    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    scope: EV
    accumulator: uet_op_sum
)")), HasSubstr("scope EV"));

    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
)")), HasSubstr("missing required key 'accumulator'"));

    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_median
)")), HasSubstr("invalid signal 'uet_rx_ack' (macro T_ACK).accumulator"));
}

TEST(PcmSpecTest, RejectsBadTriggers) {
    auto trig = [](const std::string& t) {
        return withSignals("\nsignal:\n  - name: uet_rx_ack\n    macro: T_ACK\n"
                           "    accumulator: uet_op_sum\n    trigger: " + t + "\n");
    };
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count_or_time, mode_refval: 1, "
                             "arm_mode: continuous, action: invoke, invoke_deadline: 0}")),
                HasSubstr("must be two numbers"));
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count_or_time, mode_refval: 1 2 3, "
                             "arm_mode: continuous, action: invoke, invoke_deadline: 0}")),
                HasSubstr("must be two numbers"));
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count_or_time, mode_refval: -1 0, "
                             "arm_mode: continuous, action: invoke, invoke_deadline: 0}")),
                HasSubstr("unsigned integer"));
    EXPECT_THAT(errorOf(trig("{op: ge, mode: count_or_time, mode_refval: 1 0, "
                             "arm_mode: continuous, action: invoke, invoke_deadline: 0}")),
                HasSubstr("invalid signal 'uet_rx_ack' (macro T_ACK) trigger.op 'ge'"));
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count, mode_refval: 1 0, "
                             "arm_mode: continuous, action: invoke, invoke_deadline: 0}")),
                HasSubstr("mode 'count'"));
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count_or_time, mode_refval: 1 0, "
                             "arm_mode: always, action: invoke, invoke_deadline: 0}")),
                HasSubstr("arm_mode 'always'"));
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count_or_time, mode_refval: 1 0, "
                             "arm_mode: once, action: call, invoke_deadline: 0}")),
                HasSubstr("action 'call'"));
    // The spec's own example misspells this key; we reject unknown keys.
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count_or_time, mode_refval: 1 0, "
                             "arm_mode: once, action: invoke, invoke_dealine: 0}")),
                HasSubstr("unknown key 'invoke_dealine'"));
    EXPECT_THAT(errorOf(trig("{op: wc, mode: count_or_time, mode_refval: 1 0, "
                             "action: invoke, invoke_deadline: 0}")),
                HasSubstr("missing required key 'arm_mode'"));
}

TEST(PcmSpecTest, RejectsBadControls) {
    EXPECT_THAT(errorOf(kHeader + kAckSignal + R"(
control:
  - name: uet_tx_send_rate
    macro: C_RATE
    initial_value: 0
)"), HasSubstr("not supported by this simulator yet"));

    EXPECT_THAT(errorOf(kHeader + kAckSignal + R"(
control:
  - name: cwnd
    macro: C_CWND
    initial_value: 0
)"), HasSubstr("unknown control 'cwnd'"));

    EXPECT_THAT(errorOf(kHeader + kAckSignal + R"(
control:
  - name: uet_tx_cwnd
    macro: C_CWND
    initial_value: 1
  - name: uet_tx_cwnd
    macro: C_CWND2
    initial_value: 2
)"), HasSubstr("listed twice"));
}

TEST(PcmSpecTest, RejectsUnsupportedSections) {
    EXPECT_THAT(errorOf(validFile() + R"(
global_variable:
  - macro: G_X
    dtype: uint
    initial_value: 0
    accumulator: uet_op_sum
)"), HasSubstr("global_variable is not supported"));
    EXPECT_THAT(errorOf(validFile() + "tgm:\n  macro: M\n"), HasSubstr("telegrams"));
    EXPECT_THAT(errorOf(validFile() + "lb_algorithm:\n  op_model: per-packet\n"),
                HasSubstr("load-balancing"));
    EXPECT_THAT(errorOf(validFile() + "signals:\n  - name: x\n"),
                HasSubstr("unknown top-level key 'signals'"));
}

TEST(PcmSpecTest, RejectsMissingSections) {
    EXPECT_THAT(errorOf(kAckSignal + kControl), HasSubstr("'cc_algorithm'"));
    EXPECT_THAT(errorOf(kHeader + kControl), HasSubstr("'signal'"));
    EXPECT_THAT(errorOf(kHeader + kAckSignal), HasSubstr("'control'"));
    EXPECT_THAT(errorOf(""), HasSubstr("file is empty"));
    EXPECT_THAT(errorOf("- just a list\n"), HasSubstr("top level must be a mapping"));
}

TEST(PcmSpecTest, RejectsBadAlgorithmSection) {
    EXPECT_THAT(errorOf(R"(
cc_algorithm:
  description: x
)" + kAckSignal + kControl), HasSubstr("missing required key 'name'"));

    EXPECT_THAT(errorOf(R"(
cc_algorithm:
  name: 2fast
  description: x
  max_ack_coalescing: 1
)" + kAckSignal + kControl), HasSubstr("must be a C identifier"));

    EXPECT_THAT(errorOf(R"(
cc_algorithm:
  name: test_algo
  description: x
  max_ack_coalescing: 1
  uint_min_bitwidth: 128
)" + kAckSignal + kControl), HasSubstr("between 1 and 64"));

    // Bit widths up to 64 are met, so they load (and are not stored).
    EXPECT_EQ(errorOf(R"(
cc_algorithm:
  name: test_algo
  uint_min_bitwidth: 32
  float_min_bitwidth: 64
)" + kAckSignal + kControl), "");
}

TEST(PcmSpecTest, RejectsBadVariablesAndParameters) {
    const std::string base = kHeader + kAckSignal + kControl;
    EXPECT_THAT(errorOf(base + "variable:\n  - {macro: V, dtype: uint, initial_value: -1}\n"),
                HasSubstr("unsigned integer"));
    EXPECT_THAT(errorOf(base + "variable:\n  - {macro: V, dtype: int, initial_value: 1.5}\n"),
                HasSubstr("must be an integer"));
    EXPECT_THAT(errorOf(base + "variable:\n  - {macro: V, dtype: float, initial_value: abc}\n"),
                HasSubstr("must be a number"));
    EXPECT_THAT(errorOf(base + "variable:\n  - {macro: V, dtype: double, initial_value: 1}\n"),
                HasSubstr("dtype 'double'"));
    EXPECT_THAT(errorOf(base + "parameter:\n  - {macro: P, dtype: uint}\n"),
                HasSubstr("missing required key 'value'"));

    PcmSpec s = pcmParseSpec(
        base + "variable:\n  - {macro: V_I, dtype: int, initial_value: -42}\n"
               "  - {macro: V_U, dtype: uint, initial_value: 18446744073709551615}\n",
        "test.yaml");
    EXPECT_EQ(std::get<int64_t>(s.variables[0].initial_value), -42);
    EXPECT_EQ(std::get<uint64_t>(s.variables[1].initial_value), UINT64_MAX);
}

TEST(PcmSpecTest, RejectsDuplicateMacrosAndKeys) {
    EXPECT_THAT(errorOf(kHeader + kAckSignal + kControl + R"(
variable:
  - {macro: T_ACK, dtype: uint, initial_value: 0}
)"), HasSubstr("macro 'T_ACK' is used by both"));

    EXPECT_THAT(errorOf(validFile() + "variable:\n  - {macro: V2, dtype: uint, initial_value: 0}\n"),
                HasSubstr("top-level key 'variable' appears twice"));

    EXPECT_THAT(errorOf(withSignals(R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    macro: T_ACK2
    accumulator: uet_op_sum
)")), HasSubstr("key 'macro' appears twice"));
}

TEST(PcmSpecTest, ErrorsGiveLineNumbers) {
    // Line 1 is empty; the bad signal name is on line 7.
    std::string err = errorOf(R"(
cc_algorithm:
  name: test_algo
  description: x
  max_ack_coalescing: 1
signal:
  - name: uet_rx_nope
    macro: T
    accumulator: uet_op_sum
)" + kControl);
    EXPECT_THAT(err, HasSubstr("test.yaml:7:"));

    EXPECT_THAT(errorOf("cc_algorithm: [unclosed\n"), HasSubstr("YAML syntax error"));
}

TEST(PcmSpecTest, LoadsFile) {
    const std::string path = ::testing::TempDir() + "pcm_spec_test.yaml";
    {
        std::ofstream out(path);
        out << validFile();
    }
    PcmSpec s = pcmLoadSpecFile(path);
    EXPECT_EQ(s.source, path);
    EXPECT_EQ(s.algorithm.name, "test_algo");
    std::remove(path.c_str());

    EXPECT_THROW(pcmLoadSpecFile(path), PcmSpecError);
}

TEST(PcmSignalsTest, Table) {
    const PcmSignalInfo* s = pcmFindSignal("uet_rx_ack_rtt");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->unit, PcmUnit::TIME_128NS);
    EXPECT_TRUE(s->supported);

    s = pcmFindSignal("uet_rx_ack_bytes");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->unit, PcmUnit::BYTES_256);

    s = pcmFindSignal("uet_rx_csig_s");
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->supported);

    EXPECT_EQ(pcmFindSignal("not_a_signal"), nullptr);

    // Every name is unique, and direction matches the name prefix.
    std::set<std::string> names;
    for (const auto& info : pcmAllSignals()) {
        EXPECT_TRUE(names.insert(info.name).second) << info.name;
        std::string n = info.name;
        PcmSignalDir expect = n.rfind("uet_rx_", 0) == 0   ? PcmSignalDir::RX
                              : n.rfind("uet_tx_", 0) == 0 ? PcmSignalDir::TX
                                                           : PcmSignalDir::TIME;
        EXPECT_EQ(info.dir, expect) << n;
    }
}
