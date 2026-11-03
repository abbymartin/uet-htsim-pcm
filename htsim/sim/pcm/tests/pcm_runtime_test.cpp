// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_runtime.h"

#include <functional>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "uet_pcm.h"

using ::testing::HasSubstr;

// ---------------------------------------------------------------------------
// Test handlers. Each handler file declares one algorithm; here each is in its
// own namespace instead.

// Runs whatever the test puts in `body`, so tests can call any API function.
namespace scripted {
PCM_ALGORITHM(scripted);
PCM_SIGNAL(T_ACK);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_BYTES);
PCM_SIGNAL(S_ECN);
PCM_VARIABLE(V_COUNT);
PCM_VARIABLE(V_RATIO);
PCM_PARAMETER_UINT(P_STEP);

std::function<void()> body;

PCM_HANDLER(scripted) {
    if (body)
        body();
}
}  // namespace scripted

// The paper's Listing 1 (an MP-RDMA-style sender), with its App. B
// specification: a running ACKed-bytes total with a DELTA >= 1 trigger, and a
// private baseline to get the newly ACKed bytes.
namespace listing1 {
PCM_ALGORITHM(listing1);
PCM_SIGNAL(ACKED_BYTES);
PCM_SIGNAL(NACKED_BYTES);
PCM_VARIABLE(PREV_ACKED_BYTES);
PCM_VARIABLE(CWND);
PCM_PARAMETER_UINT(MTU);

PCM_HANDLER(listing1) {
    uet_uint_t cwnd;
    uet_get_variable_uint(CWND, &cwnd);
    uet_trigmask_t mask;
    uet_trigger_mask(&mask);
    if (mask & NACKED_BYTES) {
        cwnd = cwnd / 2 > MTU ? cwnd / 2 : MTU;  // decrease logic
    } else if (mask & ACKED_BYTES) {
        uet_uint_t cur_acked = uet_signal_get(ACKED_BYTES);
        uet_uint_t prev;
        uet_get_variable_uint(PREV_ACKED_BYTES, &prev);
        uet_uint_t diff = cur_acked - prev;
        cwnd += diff * MTU / cwnd;  // additive increase
        cwnd = cwnd > MTU ? cwnd : MTU;
        uet_set_variable_uint(PREV_ACKED_BYTES, cur_acked);
    }
    uet_set_variable_uint(CWND, cwnd);
    uet_set_cwnd(unsigned(cwnd));
}
}  // namespace listing1

// Handlers that are wrong in different ways, for the loading tests.
namespace uses_missing_macro {
PCM_ALGORITHM(uses_missing_macro);
PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_NOT_IN_FILE);
PCM_HANDLER(uses_missing_macro) {}
}  // namespace uses_missing_macro

namespace uses_wrong_kind {
PCM_ALGORITHM(uses_wrong_kind);
PCM_SIGNAL(T_ACK);
PCM_SIGNAL(V_COUNT);  // a variable in the file
PCM_HANDLER(uses_wrong_kind) {}
}  // namespace uses_wrong_kind

namespace uses_wrong_type {
PCM_ALGORITHM(uses_wrong_type);
PCM_SIGNAL(T_ACK);
PCM_PARAMETER_FLOAT(P_STEP);  // uint in the file
PCM_HANDLER(uses_wrong_type) {}
}  // namespace uses_wrong_type

namespace wrong_handler_name {
PCM_ALGORITHM(wrong_handler_name);
PCM_HANDLER(some_other_name) {}
}  // namespace wrong_handler_name

namespace no_handler {
PCM_ALGORITHM(no_handler);
}  // namespace no_handler

namespace duplicate_a {
PCM_ALGORITHM(duplicate);
PCM_HANDLER(duplicate) {}
}  // namespace duplicate_a
namespace duplicate_b {
PCM_ALGORITHM(duplicate);
PCM_HANDLER(duplicate) {}
}  // namespace duplicate_b

// ---------------------------------------------------------------------------

// The tests' lambdas run as the body of the `scripted` handler and use its
// macros.
using namespace scripted;

namespace {

constexpr PcmTime kUnit = kPcmTimeUnitPs;

std::string header(const std::string& name) {
    return "\ncc_algorithm:\n  name: " + name +
           "\n  description: test\n  max_ack_coalescing: 8\n";
}

// Signal entries, in order: T_ACK (0), T_NACK (1), S_BYTES (2), S_ECN (3).
const std::string kScriptedSignals = R"(
signal:
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: invoke, invoke_deadline: 0}
  - name: uet_rx_nack
    macro: T_NACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: invoke, invoke_deadline: 4}
  - name: uet_rx_ack_bytes
    macro: S_BYTES
    accumulator: uet_op_sum
  - name: uet_rx_ack_ecn
    macro: S_ECN
    accumulator: uet_op_sum
    trigger: {op: gt, op_val: 0, trigger_type: delta, mode: count_or_time, mode_refval: 1 0,
              arm_mode: continuous, action: record, invoke_deadline: 0}
)";

const std::string kScriptedRest = R"(
control:
  - name: uet_tx_cwnd
    macro: C_CWND
    initial_value: 10000
variable:
  - {macro: V_COUNT, dtype: uint, initial_value: 0}
  - {macro: V_RATIO, dtype: float, initial_value: 0.5}
parameter:
  - {macro: P_STEP, dtype: uint, value: 100, description: step}
)";

std::shared_ptr<const PcmAlgorithm> load(const std::string& yaml) {
    return std::make_shared<const PcmAlgorithm>(pcmParseSpec(yaml, "test.yaml"));
}

std::string loadError(const std::string& yaml) {
    try {
        load(yaml);
    } catch (const PcmSpecError& e) {
        return e.what();
    }
    return "";
}

const PcmSignalInfo* sig(const char* name) { return pcmFindSignal(name); }

std::vector<PcmSignalUpdate> ack(int64_t bytes, bool ecn = false) {
    return {{sig("uet_rx_ack"), 1},
            {sig("uet_rx_ack_bytes"), bytes},
            {sig("uet_rx_ack_ecn"), ecn ? 1 : 0}};
}

std::vector<PcmSignalUpdate> nack(int64_t bytes = 0) {
    return {{sig("uet_rx_nack"), 1}, {sig("uet_rx_nack_vend_htsim_bytes"), bytes}};
}

class PcmRuntimeTest : public ::testing::Test {
protected:
    PcmRuntimeTest() {
        PcmCcc::print_errors = false;
        scripted::body = nullptr;
    }
    ~PcmRuntimeTest() override { scripted::body = nullptr; }

    std::shared_ptr<const PcmAlgorithm> scriptedAlgo() {
        return load(header("scripted") + kScriptedSignals + kScriptedRest);
    }
};

}  // namespace

// --- Loading -----------------------------------------------------------------

TEST_F(PcmRuntimeTest, MatchesMacrosToTheFile) {
    auto algo = scriptedAlgo();
    EXPECT_EQ(algo->info().name(), "scripted");
    EXPECT_TRUE(algo->warnings().empty());
    ASSERT_TRUE(algo->cwndControl().has_value());
}

TEST_F(PcmRuntimeTest, WarnsAboutMacrosTheHandlerDoesNotUse) {
    auto algo = load(header("scripted") + kScriptedSignals +
                     "  - {name: uet_rx_ack_rtt, macro: S_EXTRA, accumulator: uet_op_min}\n" +
                     kScriptedRest);
    ASSERT_EQ(algo->warnings().size(), 1u);
    EXPECT_THAT(algo->warnings()[0], HasSubstr("S_EXTRA is not used"));
}

TEST_F(PcmRuntimeTest, LoadErrors) {
    EXPECT_THAT(loadError(header("not_compiled_in") + kScriptedSignals + kScriptedRest),
                HasSubstr("no PCM handler named 'not_compiled_in'"));
    EXPECT_THAT(loadError(header("uses_missing_macro") + kScriptedSignals + kScriptedRest),
                HasSubstr("S_NOT_IN_FILE, which the file doesn't define"));
    EXPECT_THAT(loadError(header("uses_wrong_kind") + kScriptedSignals + kScriptedRest),
                HasSubstr("uses V_COUNT as a signal, but the file defines it as a variable"));
    EXPECT_THAT(loadError(header("uses_wrong_type") + kScriptedSignals + kScriptedRest),
                HasSubstr("parameter P_STEP as float, but the file declares it as uint"));
    EXPECT_THAT(loadError(header("wrong_handler_name") + kScriptedSignals + kScriptedRest),
                HasSubstr("PCM_HANDLER(some_other_name)"));
    EXPECT_THAT(loadError(header("no_handler") + kScriptedSignals + kScriptedRest),
                HasSubstr("has no PCM_HANDLER(no_handler)"));
    EXPECT_THAT(loadError(header("duplicate") + kScriptedSignals + kScriptedRest),
                HasSubstr("more than one PCM handler file"));
}

TEST_F(PcmRuntimeTest, InitialCwndFromControl) {
    PcmCcc ccc(scriptedAlgo(), 0);
    EXPECT_EQ(ccc.cwnd().value(), 10000u);

    // Without a uet_tx_cwnd control there is no window, and setting one is an
    // invalid operation.
    std::string no_control = header("scripted") + kScriptedSignals + R"(
control: []
variable:
  - {macro: V_COUNT, dtype: uint, initial_value: 0}
  - {macro: V_RATIO, dtype: float, initial_value: 0.5}
parameter:
  - {macro: P_STEP, dtype: uint, value: 100, description: step}
)";
    PcmCcc c2(load(no_control), 0);
    EXPECT_FALSE(c2.cwnd().has_value());
    scripted::body = [] { uet_set_cwnd(5000); };
    c2.onEvent(ack(1), 1);
    ASSERT_TRUE(c2.inError());
    EXPECT_EQ(c2.fatalError()->value, PCM_ERR_NO_CWND_CONTROL);
}

// --- Calling the handler -------------------------------------------------------

TEST_F(PcmRuntimeTest, CalledOnAckWithSignalsAndMask) {
    PcmCcc ccc(scriptedAlgo(), 0);
    uet_trigmask_t seen_mask = 0;
    bool ack_fired = false, nack_fired = true;
    int64_t seen_bytes = -1;
    scripted::body = [&] {
        uet_trigger_mask(&seen_mask);
        ack_fired = seen_mask & T_ACK;
        nack_fired = seen_mask & T_NACK;
        seen_bytes = uet_signal_get(S_BYTES);
        uet_set_cwnd(unsigned(seen_bytes) + 1);
    };
    ccc.onEvent(ack(4096), 1);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_EQ(seen_mask, PcmSignalState::bit(0));
    EXPECT_TRUE(ack_fired);
    EXPECT_FALSE(nack_fired);
    EXPECT_EQ(seen_bytes, 4096);
    EXPECT_EQ(ccc.cwnd().value(), 4097u);
    EXPECT_EQ(ccc.counters().cwnd_updates, 1u);
    EXPECT_EQ(ccc.pendingMask(), 0u);
}

TEST_F(PcmRuntimeTest, TriggerMaskAndDropMask) {
    PcmCcc ccc(scriptedAlgo(), 0);
    uet_trigmask_t mask = 0;
    uint32_t dropped = 99;
    int rc = -1, rc_drop = -1;
    scripted::body = [&] {
        rc = uet_trigger_mask(&mask);
        rc_drop = uet_trigger_drop_mask(&dropped);
    };
    ccc.onEvent(ack(1), 1);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(mask, PcmSignalState::bit(0));
    EXPECT_EQ(rc_drop, 0);
    EXPECT_EQ(dropped, 0u);
}

// A RECORD trigger doesn't call the handler, but its bit is delivered with the
// next call.
TEST_F(PcmRuntimeTest, RecordedTriggersDeliveredWithNextCall) {
    PcmCcc ccc(scriptedAlgo(), 0);
    uet_trigmask_t seen = 0, expected = 0;
    scripted::body = [&] {
        uet_trigger_mask(&seen);
        expected = T_ACK | S_ECN;
    };

    ccc.onEvent({{sig("uet_rx_ack_ecn"), 1}}, 1);
    EXPECT_EQ(ccc.counters().invocations, 0u);
    EXPECT_EQ(ccc.pendingMask(), PcmSignalState::bit(3));

    ccc.onEvent(ack(1), 2);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_EQ(seen, expected);
    EXPECT_EQ(seen, PcmSignalState::bit(0) | PcmSignalState::bit(3));
    EXPECT_EQ(ccc.pendingMask(), 0u);
}

TEST_F(PcmRuntimeTest, InvokeDeadlineDefersTheCall) {
    PcmCcc ccc(scriptedAlgo(), 0);
    uet_trigmask_t seen = 0;
    scripted::body = [&] { uet_trigger_mask(&seen); };

    ccc.onEvent(nack(), 1000);
    EXPECT_EQ(ccc.counters().invocations, 0u);
    EXPECT_EQ(ccc.invokeDeadline().value(), 1000 + 4 * kUnit);

    ccc.invoke(1000 + 4 * kUnit);
    EXPECT_EQ(seen, PcmSignalState::bit(1));  // T_NACK
    EXPECT_FALSE(ccc.invokeDeadline().has_value());
}

// An immediate trigger delivers deferred ones that fired earlier, too.
TEST_F(PcmRuntimeTest, ImmediateCallDeliversDeferredTriggers) {
    PcmCcc ccc(scriptedAlgo(), 0);
    uet_trigmask_t seen = 0;
    scripted::body = [&] { uet_trigger_mask(&seen); };
    ccc.onEvent(nack(), 1);
    ccc.onEvent(ack(1), 2);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_EQ(seen, PcmSignalState::bit(0) | PcmSignalState::bit(1));  // T_ACK | T_NACK
    EXPECT_FALSE(ccc.invokeDeadline().has_value());
}

// The handler reads a snapshot. Its own signal writes show in the snapshot at
// once, and in the CCC's signals when it returns.
TEST_F(PcmRuntimeTest, SignalWrites) {
    PcmCcc ccc(scriptedAlgo(), 0);
    std::vector<int64_t> reads;
    scripted::body = [&] {
        reads.push_back(uet_signal_get(S_BYTES));
        uet_signal_set(S_BYTES, 0);
        reads.push_back(uet_signal_get(S_BYTES));
        uet_signal_update(S_BYTES, 10);
        reads.push_back(uet_signal_get(S_BYTES));
    };
    ccc.onEvent(ack(4096), 1);
    EXPECT_EQ(reads, (std::vector<int64_t>{4096, 0, 10}));
    EXPECT_EQ(ccc.signals().get(2), 10);

    scripted::body = nullptr;
    ccc.onEvent(ack(100), 2);
    EXPECT_EQ(ccc.signals().get(2), 110);
}

TEST_F(PcmRuntimeTest, VariablesAndParameters) {
    PcmCcc ccc(scriptedAlgo(), 0);
    uet_uint_t step = 0;
    scripted::body = [&] {
        uet_uint_t n;
        uet_get_variable_uint(V_COUNT, &n);
        uet_set_variable_uint(V_COUNT, n + 1);
        uet_float_t r;
        uet_get_variable_float(V_RATIO, &r);
        uet_set_variable_float(V_RATIO, r * 2);
        step = P_STEP;
    };
    ccc.onEvent(ack(1), 1);
    ccc.onEvent(ack(1), 2);
    ccc.onEvent(ack(1), 3);
    EXPECT_EQ(std::get<uint64_t>(ccc.variable(0)), 3u);
    EXPECT_DOUBLE_EQ(std::get<double>(ccc.variable(1)), 4.0);
    EXPECT_EQ(step, 100u);
    EXPECT_FALSE(ccc.inError());
}

// Each CCC has its own variables, signals and window.
TEST_F(PcmRuntimeTest, CccsSharingAnAlgorithmAreIndependent) {
    auto algo = scriptedAlgo();
    PcmCcc a(algo, 0, "a"), b(algo, 0, "b");
    scripted::body = [] {
        uet_uint_t n;
        uet_get_variable_uint(V_COUNT, &n);
        uet_set_variable_uint(V_COUNT, n + 1);
        uet_set_cwnd(unsigned(uet_signal_get(S_BYTES)));
    };
    a.onEvent(ack(1000), 1);
    a.onEvent(ack(1000), 2);
    b.onEvent(ack(500), 3);
    EXPECT_EQ(std::get<uint64_t>(a.variable(0)), 2u);
    EXPECT_EQ(std::get<uint64_t>(b.variable(0)), 1u);
    EXPECT_EQ(a.cwnd().value(), 2000u);
    EXPECT_EQ(b.cwnd().value(), 500u);
}

// The same handler loaded from a second file with its entries in a different
// order and a different parameter value: each file's macros resolve to that
// file's entries.
TEST_F(PcmRuntimeTest, MacrosResolvePerFile) {
    const std::string reordered = header("scripted") + R"(
signal:
  - name: uet_rx_ack_ecn
    macro: S_ECN
    accumulator: uet_op_sum
  - name: uet_rx_ack_bytes
    macro: S_BYTES
    accumulator: uet_op_sum
  - name: uet_rx_nack
    macro: T_NACK
    accumulator: uet_op_sum
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode: count_or_time, mode_refval: 1 0, arm_mode: continuous,
              action: invoke, invoke_deadline: 0}
control:
  - {name: uet_tx_cwnd, macro: C_CWND, initial_value: 1}
variable:
  - {macro: V_RATIO, dtype: float, initial_value: 0}
  - {macro: V_COUNT, dtype: uint, initial_value: 0}
parameter:
  - {macro: P_STEP, dtype: uint, value: 7, description: step}
)";
    PcmCcc first(scriptedAlgo(), 0), second(load(reordered), 0);
    struct Seen {
        int64_t bytes;
        uet_uint_t step;
        bool ack_bit;
        unsigned ack_index;
    } seen{};
    scripted::body = [&] {
        seen.bytes = uet_signal_get(S_BYTES);
        seen.step = P_STEP;
        uet_trigmask_t fired;
        uet_trigger_mask(&fired);
        seen.ack_bit = (fired & T_ACK) != 0;
        seen.ack_index = T_ACK;
        uet_set_variable_uint(V_COUNT, 5);
    };
    first.onEvent(ack(300), 1);
    EXPECT_EQ(seen.bytes, 300);
    EXPECT_EQ(seen.step, 100u);
    EXPECT_TRUE(seen.ack_bit);
    EXPECT_EQ(seen.ack_index, 0u);

    second.onEvent(ack(700), 1);
    EXPECT_EQ(seen.bytes, 700);
    EXPECT_EQ(seen.step, 7u);
    EXPECT_TRUE(seen.ack_bit);
    EXPECT_EQ(seen.ack_index, 3u);
    EXPECT_EQ(std::get<uint64_t>(second.variable(1)), 5u);
    EXPECT_FALSE(second.inError());
}

// --- ERROR state ---------------------------------------------------------------

TEST_F(PcmRuntimeTest, FatalDiscardsTheCallAndStopsTheCcc) {
    PcmCcc ccc(scriptedAlgo(), 0);
    int rc_after = 0;
    scripted::body = [&] {
        uet_set_cwnd(5000);
        uet_set_variable_uint(V_COUNT, 9);  // local variables change at once
        uet_ccc_fatal(42);
        rc_after = uet_set_cwnd(6000);
    };
    ccc.onEvent(ack(1), 1);
    ASSERT_TRUE(ccc.inError());
    EXPECT_EQ(ccc.fatalError()->source, PcmErrorSource::PCM_ALGORITHM);
    EXPECT_EQ(ccc.fatalError()->value, 42);
    EXPECT_EQ(rc_after, -ENOTRECOVERABLE);
    EXPECT_EQ(ccc.cwnd().value(), 10000u);  // the 5000 was not applied
    EXPECT_EQ(ccc.counters().cwnd_updates, 0u);

    // No more calls, and later events are ignored.
    ccc.onEvent(ack(1), 2);
    ccc.invoke(3);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_EQ(ccc.signals().counters().events, 1u);
}

TEST_F(PcmRuntimeTest, OnlyTheFirstFatalErrorIsRecorded) {
    PcmCcc ccc(scriptedAlgo(), 0);
    scripted::body = [] {
        uet_ccc_fatal(1);
        uet_ccc_fatal(2);
        uet_signal_get(99);
    };
    ccc.onEvent(ack(1), 1);
    EXPECT_EQ(ccc.fatalError()->value, 1);
}

TEST_F(PcmRuntimeTest, InvalidApiUseIsFatal) {
    struct Case {
        const char* what;
        std::function<void()> body;
        int expected;
    };
    std::vector<Case> cases = {
        {"signal index", [] { uet_signal_get(99); }, PCM_ERR_SIGNAL_INDEX},
        {"signal set index", [] { uet_signal_set(99, 1); }, PCM_ERR_SIGNAL_INDEX},
        {"variable index", [] { uet_set_variable_uint(99, 1); }, PCM_ERR_VARIABLE_INDEX},
        {"variable type", [] { uet_int_t v; uet_get_variable_int(scripted::V_COUNT, &v); },
         PCM_ERR_VARIABLE_TYPE},
        {"set variable type", [] { uet_set_variable_float(scripted::V_COUNT, 1.0); },
         PCM_ERR_VARIABLE_TYPE},
        {"null pointer", [] { uet_get_variable_uint(scripted::V_COUNT, nullptr); },
         PCM_ERR_NULL_POINTER},
        {"zero cwnd", [] { uet_set_cwnd(0); }, PCM_ERR_ZERO_CWND},
        {"null trigger mask", [] { uet_trigger_mask(nullptr); }, PCM_ERR_NULL_POINTER},
        {"null drop mask", [] { uet_trigger_drop_mask(nullptr); }, PCM_ERR_NULL_POINTER},
    };
    for (const Case& c : cases) {
        PcmCcc ccc(scriptedAlgo(), 0);
        scripted::body = c.body;
        ccc.onEvent(ack(1), 1);
        ASSERT_TRUE(ccc.inError()) << c.what;
        EXPECT_EQ(ccc.fatalError()->source, PcmErrorSource::IMPLEMENTATION) << c.what;
        EXPECT_EQ(ccc.fatalError()->value, c.expected) << c.what;
    }
}

TEST_F(PcmRuntimeTest, MacroOutsideHandlerAborts) {
    EXPECT_DEATH({ unsigned i = scripted::T_ACK; (void)i; }, "outside a running handler");
    EXPECT_DEATH(uet_set_cwnd(1), "outside a running handler");
}

// --- A complete handler ----------------------------------------------------------

TEST_F(PcmRuntimeTest, PaperListing1) {
    auto algo = load(header("listing1") + R"(
signal:
  - name: uet_rx_ack_bytes
    macro: ACKED_BYTES
    accumulator: uet_op_sum
    trigger: {op: gte, op_val: 1, trigger_type: delta, mode: count_or_time, mode_refval: 1 0,
              arm_mode: continuous, action: invoke, invoke_deadline: 0}
  - name: uet_rx_nack_vend_htsim_bytes
    macro: NACKED_BYTES
    accumulator: uet_op_sum
    trigger: {op: gte, op_val: 1, trigger_type: delta, mode: count_or_time, mode_refval: 1 0,
              arm_mode: continuous, action: invoke, invoke_deadline: 0}
control:
  - {name: uet_tx_cwnd, macro: C_CWND, initial_value: 8192}
variable:
  - {macro: PREV_ACKED_BYTES, dtype: uint, initial_value: 0}
  - {macro: CWND, dtype: uint, initial_value: 8192}
parameter:
  - {macro: MTU, dtype: uint, value: 4096, description: MTU}
)");
    EXPECT_EQ(algo->spec().extensions_used.size(), 3u);  // 2 delta triggers + vendor signal
    PcmCcc ccc(algo, 0);

    ccc.onEvent(ack(4096), 1);  // cwnd += 4096 * 4096 / 8192 = 2048
    EXPECT_EQ(ccc.cwnd().value(), 10240u);
    ccc.onEvent(ack(0), 2);     // a duplicate ACK: DELTA >= 1 doesn't fire
    EXPECT_EQ(ccc.counters().invocations, 1u);
    ccc.onEvent(ack(8192), 3);  // cwnd += 8192 * 4096 / 10240 = 3276
    EXPECT_EQ(ccc.cwnd().value(), 13516u);
    ccc.onEvent(nack(4096), 4);  // halve
    EXPECT_EQ(ccc.cwnd().value(), 6758u);
    EXPECT_EQ(std::get<uint64_t>(ccc.variable(0)), 12288u);
    EXPECT_FALSE(ccc.inError());
}
