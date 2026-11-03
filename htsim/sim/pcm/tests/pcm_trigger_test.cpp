// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_trigger.h"

#include <limits>

#include <gtest/gtest.h>

namespace {

constexpr PcmTime kUnit = kPcmTimeUnitPs;  // one YAML time unit, 128 ns

PcmTriggerSpec trig(PcmTriggerOp op, int64_t op_val, uint64_t count, uint64_t time,
                    PcmArmMode arm = PcmArmMode::CONTINUOUS,
                    PcmTriggerType type = PcmTriggerType::RAW) {
    PcmTriggerSpec t;
    t.op = op;
    t.op_val = op_val;
    t.ref_count = count;
    t.ref_time = time;
    t.arm_mode = arm;
    t.type = type;
    return t;
}

}  // namespace

TEST(PcmTriggerTest, TimeUnits) {
    EXPECT_EQ(pcmTimeUnitsToPs(0), 0u);
    EXPECT_EQ(pcmTimeUnitsToPs(15625), 2000000000u);  // the spec's "every 2 ms" example
    EXPECT_EQ(pcmTimeUnitsToPs(std::numeric_limits<uint64_t>::max()),
              std::numeric_limits<PcmTime>::max());
}

// mode_refval 0 0: "no evaluation is performed; trigger fires immediately if
// armed", even if the comparison would be false.
TEST(PcmTriggerTest, NoEvaluationModeFiresEveryTime) {
    PcmAccumulator acc(PcmAccOp::LAST);
    PcmTrigger t(trig(PcmTriggerOp::GT, 100, 0, 0), acc, 0);
    acc.update(1);
    EXPECT_TRUE(t.evaluate(acc, 10));
    EXPECT_TRUE(t.evaluate(acc, 20));
}

TEST(PcmTriggerTest, CountOnlyWildcard) {
    PcmAccumulator acc(PcmAccOp::SUM);
    PcmTrigger t(trig(PcmTriggerOp::WC, 0, 3, 0), acc, 0);
    std::vector<bool> fired;
    for (int i = 0; i < 7; i++) {
        acc.update(1);
        fired.push_back(t.evaluate(acc, i));
    }
    EXPECT_EQ(fired, (std::vector<bool>{false, false, true, false, false, true, false}));
    EXPECT_EQ(t.trueCount(), 1u);
}

TEST(PcmTriggerTest, CountOnlyCountsTrueComparisons) {
    // Fire on the second sample above 10.
    PcmAccumulator acc(PcmAccOp::LAST);
    PcmTrigger t(trig(PcmTriggerOp::GT, 10, 2, 0), acc, 0);
    std::vector<int64_t> samples = {5, 11, 3, 10, 12, 20};
    std::vector<bool> fired;
    for (int64_t s : samples) {
        acc.update(s);
        fired.push_back(t.evaluate(acc, 0));
    }
    EXPECT_EQ(fired, (std::vector<bool>{false, false, false, false, true, false}));
}

TEST(PcmTriggerTest, EveryComparisonOperator) {
    struct Case {
        PcmTriggerOp op;
        bool at_9, at_10, at_11;
    };
    for (const Case& c : {Case{PcmTriggerOp::EQ, false, true, false},
                          Case{PcmTriggerOp::NEQ, true, false, true},
                          Case{PcmTriggerOp::GT, false, false, true},
                          Case{PcmTriggerOp::GTE, false, true, true},
                          Case{PcmTriggerOp::LT, true, false, false},
                          Case{PcmTriggerOp::LTE, true, true, false},
                          Case{PcmTriggerOp::WC, true, true, true}}) {
        std::vector<bool> got;
        for (int64_t v : {9, 10, 11}) {
            PcmAccumulator acc(PcmAccOp::LAST);
            PcmTrigger t(trig(c.op, 10, 1, 0), acc, 0);
            acc.update(v);
            got.push_back(t.evaluate(acc, 0));
        }
        EXPECT_EQ(got, (std::vector<bool>{c.at_9, c.at_10, c.at_11})) << pcmTriggerOpName(c.op);
    }
}

TEST(PcmTriggerTest, TimeOnly) {
    PcmAccumulator acc(PcmAccOp::LAST);
    PcmTrigger t(trig(PcmTriggerOp::WC, 0, 0, 2), acc, 1000);  // every 2 units, from t=1000
    EXPECT_EQ(t.timeDue().value(), 1000 + 2 * kUnit);
    EXPECT_FALSE(t.evaluate(acc, 1000 + 2 * kUnit - 1));
    EXPECT_TRUE(t.evaluate(acc, 1000 + 2 * kUnit));
    // Time is measured from when it last fired.
    EXPECT_EQ(t.timeDue().value(), 1000 + 4 * kUnit);
    EXPECT_FALSE(t.evaluate(acc, 1000 + 3 * kUnit));
    EXPECT_TRUE(t.evaluate(acc, 1000 + 5 * kUnit));  // late evaluation still fires
    EXPECT_EQ(t.timeDue().value(), 1000 + 7 * kUnit);
}

// The time condition doesn't depend on the comparison (§3.6.18.5.1).
TEST(PcmTriggerTest, TimeConditionIgnoresComparison) {
    PcmAccumulator acc(PcmAccOp::LAST);
    PcmTrigger t(trig(PcmTriggerOp::GT, 100, 0, 1), acc, 0);
    acc.update(1);
    EXPECT_TRUE(t.evaluate(acc, kUnit));
}

TEST(PcmTriggerTest, CombinedCountOrTime) {
    PcmAccumulator acc(PcmAccOp::SUM);
    PcmTrigger t(trig(PcmTriggerOp::WC, 0, 3, 10), acc, 0);
    // Count reaches 3 first.
    EXPECT_FALSE(t.evaluate(acc, 1));
    EXPECT_FALSE(t.evaluate(acc, 2));
    EXPECT_TRUE(t.evaluate(acc, 3));
    // Then time runs out first: one evaluation, then 10 units pass.
    EXPECT_FALSE(t.evaluate(acc, 4));
    EXPECT_TRUE(t.evaluate(acc, 3 + 10 * kUnit));
    // Firing on time also reset the count.
    EXPECT_EQ(t.trueCount(), 0u);
}

TEST(PcmTriggerTest, OnceFiresOnlyOnce) {
    PcmAccumulator acc(PcmAccOp::SUM);
    PcmTrigger t(trig(PcmTriggerOp::WC, 0, 1, 2, PcmArmMode::ONCE), acc, 0);
    EXPECT_TRUE(t.armed());
    EXPECT_TRUE(t.evaluate(acc, 1));
    EXPECT_FALSE(t.armed());
    EXPECT_FALSE(t.evaluate(acc, 2));
    EXPECT_FALSE(t.evaluate(acc, 10 * kUnit));  // the time condition is off too
    EXPECT_FALSE(t.timeDue().has_value());
}

// DELTA (from the PCM paper): compare the change since the last true
// comparison. ">= 1" on a running ACK-bytes total fires only on ACKs that
// acknowledge new bytes.
TEST(PcmTriggerTest, DeltaFiresOnIncrease) {
    PcmAccumulator acked(PcmAccOp::SUM);
    PcmTrigger t(trig(PcmTriggerOp::GTE, 1, 1, 0, PcmArmMode::CONTINUOUS, PcmTriggerType::DELTA),
                 acked, 0);
    std::vector<int64_t> acks = {4096, 0, 4096, 0, 0, 8192};
    std::vector<bool> fired;
    for (int64_t a : acks) {
        acked.update(a);
        fired.push_back(t.evaluate(acked, 0));
    }
    EXPECT_EQ(fired, (std::vector<bool>{true, false, true, false, false, true}));
}

TEST(PcmTriggerTest, DeltaThresholdAccumulatesUntilTrue) {
    // Fire each time another 10000 bytes have been acknowledged.
    PcmAccumulator acked(PcmAccOp::SUM);
    PcmTrigger t(trig(PcmTriggerOp::GTE, 10000, 1, 0, PcmArmMode::CONTINUOUS,
                      PcmTriggerType::DELTA),
                 acked, 0);
    std::vector<bool> fired;
    for (int i = 0; i < 6; i++) {
        acked.update(4000);  // totals: 4000, 8000, 12000, 16000, 20000, 24000
        fired.push_back(t.evaluate(acked, 0));
    }
    // Baseline moves to 12000 after the first fire, then 24000 is the next >= +10000.
    EXPECT_EQ(fired, (std::vector<bool>{false, false, true, false, false, true}));
}

TEST(PcmTriggerTest, DeltaStartsFromValueAtCreation) {
    PcmAccumulator acked(PcmAccOp::SUM);
    acked.update(50000);
    PcmTrigger t(trig(PcmTriggerOp::GTE, 1000, 1, 0, PcmArmMode::CONTINUOUS,
                      PcmTriggerType::DELTA),
                 acked, 0);
    acked.update(500);
    EXPECT_FALSE(t.evaluate(acked, 0));
    acked.update(500);
    EXPECT_TRUE(t.evaluate(acked, 0));
}

TEST(PcmTriggerTest, MagnitudeFiresOnChangeInEitherDirection) {
    PcmAccumulator rtt(PcmAccOp::LAST);
    rtt.update(100);
    PcmTrigger t(trig(PcmTriggerOp::GTE, 20, 1, 0, PcmArmMode::CONTINUOUS,
                      PcmTriggerType::MAGNITUDE),
                 rtt, 0);
    std::vector<int64_t> samples = {110, 125, 104, 90, 92};
    std::vector<bool> fired;
    for (int64_t s : samples) {
        rtt.update(s);
        fired.push_back(t.evaluate(rtt, 0));
    }
    // 110 (+10), 125 (+25 from 100: fire, baseline 125), 104 (-21: fire, baseline 104),
    // 90 (-14), 92 (-12).
    EXPECT_EQ(fired, (std::vector<bool>{false, true, true, false, false}));
}

TEST(PcmTriggerTest, RawComparisonOnEwmaUsesFloat) {
    PcmAccumulator avg(PcmAccOp::EWMA, 0.5);
    PcmTrigger t(trig(PcmTriggerOp::GT, 10, 1, 0), avg, 0);
    avg.update(10);
    avg.update(11);  // 10.5 > 10
    EXPECT_TRUE(t.evaluate(avg, 0));

    PcmAccumulator avg2(PcmAccOp::EWMA, 0.5);
    PcmTrigger t2(trig(PcmTriggerOp::GTE, 11, 1, 0), avg2, 0);
    avg2.update(10);
    avg2.update(11);  // 10.5 is not >= 11, although it rounds to 11
    EXPECT_FALSE(t2.evaluate(avg2, 0));
}
