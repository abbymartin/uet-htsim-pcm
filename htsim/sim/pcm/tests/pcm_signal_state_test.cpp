// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_signal_state.h"

#include <string>

#include <gtest/gtest.h>

namespace {

constexpr PcmTime kUnit = kPcmTimeUnitPs;

const std::string kHeader = R"(
cc_algorithm:
  name: test_algo
  description: test
  max_ack_coalescing: 8
control:
  - name: uet_tx_cwnd
    macro: C_CWND
    initial_value: 0
)";

// Entries, in order:
//   0 T_ACK      uet_rx_ack sum, invoke on every ACK
//   1 T_NACK     uet_rx_nack sum, invoke on every NACK, may be deferred 4 units
//   2 S_ECN      uet_rx_ack_ecn sum, record on every marked ACK
//   3 S_BYTES    uet_rx_ack_bytes sum, no trigger
//   4 S_BYTES_LAST uet_rx_ack_bytes last, no trigger
//   5 T_TIME     uet_time_current last, invoke every 10 units
const std::string kSignals = R"(
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
  - name: uet_rx_ack_ecn
    macro: S_ECN
    accumulator: uet_op_sum
    trigger: {op: gt, op_val: 0, trigger_type: delta, mode: count_or_time, mode_refval: 1 0,
              arm_mode: continuous, action: record, invoke_deadline: 0}
  - name: uet_rx_ack_bytes
    macro: S_BYTES
    accumulator: uet_op_sum
  - name: uet_rx_ack_bytes
    macro: S_BYTES_LAST
    accumulator: uet_op_last
  - name: uet_time_current
    macro: T_TIME
    accumulator: uet_op_last
    trigger: {op: wc, mode: count_or_time, mode_refval: 0 10, arm_mode: continuous,
              action: invoke, invoke_deadline: 0}
)";

const PcmSignalInfo* sig(const char* name) { return pcmFindSignal(name); }

std::vector<PcmSignalUpdate> ack(int64_t bytes, bool ecn) {
    return {{sig("uet_rx_ack"), 1},
            {sig("uet_rx_ack_bytes"), bytes},
            {sig("uet_rx_ack_ecn"), ecn ? 1 : 0}};
}

std::vector<PcmSignalUpdate> nack() { return {{sig("uet_rx_nack"), 1}}; }

class PcmSignalStateTest : public ::testing::Test {
protected:
    PcmSignalStateTest()
        : spec_(pcmParseSpec(kHeader + kSignals, "test.yaml")), state_(spec_, 0) {}
    PcmSpec spec_;
    PcmSignalState state_;
};

}  // namespace

TEST_F(PcmSignalStateTest, OneEntryPerSignalEntry) {
    ASSERT_EQ(state_.size(), 6u);
    EXPECT_EQ(state_.entrySpec(3).macro, "S_BYTES");
    EXPECT_EQ(state_.entrySpec(4).macro, "S_BYTES_LAST");
    EXPECT_NE(state_.trigger(0), nullptr);
    EXPECT_EQ(state_.trigger(3), nullptr);
}

TEST_F(PcmSignalStateTest, AckUpdatesEveryAccumulatorOfItsSignals) {
    state_.applyEvent(ack(4096, false), 1);
    state_.applyEvent(ack(1024, false), 2);
    EXPECT_EQ(state_.get(0), 2);      // ACK count
    EXPECT_EQ(state_.get(3), 5120);   // byte total
    EXPECT_EQ(state_.get(4), 1024);   // last ACK's bytes
    EXPECT_EQ(state_.get(1), 0);      // NACKs untouched
}

TEST_F(PcmSignalStateTest, InvokeAndRecordBits) {
    PcmFired f = state_.applyEvent(ack(4096, false), 1);
    EXPECT_EQ(f.mask, PcmSignalState::bit(0));
    EXPECT_TRUE(f.invoke_now);
    EXPECT_FALSE(f.invoke_by.has_value());

    // An ECN-marked ACK also fires the RECORD trigger: its bit is in the mask,
    // but it doesn't by itself ask for an invocation.
    f = state_.applyEvent(ack(4096, true), 2);
    EXPECT_EQ(f.mask, PcmSignalState::bit(0) | PcmSignalState::bit(2));
    EXPECT_TRUE(f.invoke_now);
}

TEST_F(PcmSignalStateTest, RecordOnlyDoesNotInvoke) {
    // An event that only updates the ECN signal.
    PcmFired f = state_.applyEvent({{sig("uet_rx_ack_ecn"), 1}}, 1);
    EXPECT_EQ(f.mask, PcmSignalState::bit(2));
    EXPECT_FALSE(f.invoke_now);
    EXPECT_FALSE(f.invoke_by.has_value());
}

TEST_F(PcmSignalStateTest, InvokeDeadline) {
    PcmFired f = state_.applyEvent(nack(), 1000);
    EXPECT_EQ(f.mask, PcmSignalState::bit(1));
    EXPECT_FALSE(f.invoke_now);
    ASSERT_TRUE(f.invoke_by.has_value());
    EXPECT_EQ(*f.invoke_by, 1000 + 4 * kUnit);
}

// A trigger is only checked when the event updated its own signal.
TEST_F(PcmSignalStateTest, OnlyTriggersOfUpdatedSignalsAreChecked) {
    // Make the ACK trigger fire every 2 ACKs, then send ACK, NACK, ACK.
    PcmTriggerSpec every2 = state_.trigger(0)->spec();
    every2.ref_count = 2;
    state_.trigger(0)->reconfigure(every2, state_.accumulator(0), 0);

    EXPECT_EQ(state_.applyEvent(ack(1, false), 1).mask, 0u);
    // The NACK doesn't count towards the ACK trigger...
    EXPECT_EQ(state_.applyEvent(nack(), 2).mask, PcmSignalState::bit(1));
    EXPECT_EQ(state_.trigger(0)->trueCount(), 1u);
    // ...so the second ACK is what fires it.
    EXPECT_EQ(state_.applyEvent(ack(1, false), 3).mask, PcmSignalState::bit(0));
}

// All updates of one event are applied before triggers are checked, and a
// trigger is checked once per event even if its signal is updated twice.
TEST_F(PcmSignalStateTest, OneEvaluationPerEvent) {
    PcmTriggerSpec every2 = state_.trigger(0)->spec();
    every2.ref_count = 2;
    state_.trigger(0)->reconfigure(every2, state_.accumulator(0), 0);

    PcmFired f = state_.applyEvent({{sig("uet_rx_ack"), 1}, {sig("uet_rx_ack"), 1}}, 1);
    EXPECT_EQ(state_.get(0), 2);
    EXPECT_EQ(f.mask, 0u);
    EXPECT_EQ(state_.trigger(0)->trueCount(), 1u);
}

TEST_F(PcmSignalStateTest, UnusedSignalsAreIgnored) {
    PcmFired f = state_.applyEvent({{sig("uet_tx_rto"), 1}}, 1);
    EXPECT_EQ(f.mask, 0u);
    EXPECT_EQ(state_.counters().signal_updates, 0u);
}

TEST_F(PcmSignalStateTest, TimeTrigger) {
    EXPECT_EQ(state_.nextTimeDue().value(), 10 * kUnit);
    auto tick = [](PcmTime now) {
        return std::vector<PcmSignalUpdate>{{sig("uet_time_current"), int64_t(now / kUnit)}};
    };
    EXPECT_EQ(state_.applyEvent(tick(5 * kUnit), 5 * kUnit).mask, 0u);
    PcmFired f = state_.applyEvent(tick(10 * kUnit), 10 * kUnit);
    EXPECT_EQ(f.mask, PcmSignalState::bit(5));
    EXPECT_TRUE(f.invoke_now);
    EXPECT_EQ(state_.get(5), 10);
    EXPECT_EQ(state_.nextTimeDue().value(), 20 * kUnit);
}

TEST_F(PcmSignalStateTest, HandlerAccess) {
    state_.applyEvent(ack(4096, false), 1);
    state_.set(3, 0);       // uet_signal_set: reset the byte total
    state_.update(3, 100);  // uet_signal_update: through the accumulator
    state_.applyEvent(ack(50, false), 2);
    EXPECT_EQ(state_.get(3), 150);
}

TEST_F(PcmSignalStateTest, Counters) {
    state_.applyEvent(ack(1, false), 1);  // 4 updates (bytes has 2 entries), 2 triggers checked
    state_.applyEvent(nack(), 2);         // 1 update, 1 trigger checked
    const auto& c = state_.counters();
    EXPECT_EQ(c.events, 2u);
    EXPECT_EQ(c.signal_updates, 5u);
    EXPECT_EQ(c.evaluations, 3u);
    EXPECT_EQ(c.fires, 2u);  // the ECN trigger's delta was 0
}

TEST(PcmSignalStateLimits, AtMost64Entries) {
    std::string signals = "\nsignal:\n";
    for (int i = 0; i < 65; i++)
        signals += "  - {name: uet_rx_ack, macro: S" + std::to_string(i) +
                   ", accumulator: uet_op_sum}\n";
    PcmSpec spec = pcmParseSpec(kHeader + signals, "big.yaml");
    EXPECT_THROW(PcmSignalState(spec, 0), PcmSpecError);
}
