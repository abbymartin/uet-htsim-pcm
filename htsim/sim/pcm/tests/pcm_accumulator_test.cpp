// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_accumulator.h"

#include <gtest/gtest.h>

// Each accumulator starts at the value that has no effect on its operation.
TEST(PcmAccumulatorTest, StartingValues) {
    for (PcmAccOp op : {PcmAccOp::SUM, PcmAccOp::MAX, PcmAccOp::LAST}) {
        PcmAccumulator a(op);
        EXPECT_EQ(a.get(), 0) << pcmAccOpName(op);
    }
    PcmAccumulator mn(PcmAccOp::MIN);
    EXPECT_EQ(mn.get(), kPcmMinStartValue);

    PcmAccumulator e(PcmAccOp::EWMA, 0.5);
    EXPECT_EQ(e.get(), 0);
    EXPECT_DOUBLE_EQ(e.getFloat(), 0.0);
}

TEST(PcmAccumulatorTest, Sum) {
    PcmAccumulator a(PcmAccOp::SUM);
    a.update(5);
    a.update(0);
    a.update(-2);
    a.update(10);
    EXPECT_EQ(a.get(), 13);
}

TEST(PcmAccumulatorTest, MaxAndMin) {
    PcmAccumulator mx(PcmAccOp::MAX);
    mx.update(2);
    EXPECT_EQ(mx.get(), 2);
    mx.update(3);
    mx.update(1);
    EXPECT_EQ(mx.get(), 3);

    PcmAccumulator mn(PcmAccOp::MIN);
    mn.update(40);
    EXPECT_EQ(mn.get(), 40);
    mn.update(25);
    mn.update(30);
    EXPECT_EQ(mn.get(), 25);
}

// Setting a MIN back to its starting value restarts the minimum.
TEST(PcmAccumulatorTest, RestartMin) {
    PcmAccumulator mn(PcmAccOp::MIN);
    mn.update(10);
    mn.set(kPcmMinStartValue);
    mn.update(50);
    EXPECT_EQ(mn.get(), 50);
}

TEST(PcmAccumulatorTest, Last) {
    PcmAccumulator a(PcmAccOp::LAST);
    a.update(9);
    a.update(4);
    EXPECT_EQ(a.get(), 4);
}

TEST(PcmAccumulatorTest, Ewma) {
    PcmAccumulator a(PcmAccOp::EWMA, 0.25);
    a.update(100);  // first sample is stored as it is
    EXPECT_DOUBLE_EQ(a.getFloat(), 100.0);
    a.update(200);  // 200 * 0.25 + 100 * 0.75
    EXPECT_DOUBLE_EQ(a.getFloat(), 125.0);
    a.update(0);    // 0 * 0.25 + 125 * 0.75
    EXPECT_DOUBLE_EQ(a.getFloat(), 93.75);
    EXPECT_EQ(a.get(), 94);  // rounded
}

TEST(PcmAccumulatorTest, SetReplacesValue) {
    PcmAccumulator sum(PcmAccOp::SUM);
    sum.update(50);
    sum.set(0);  // e.g. a handler resetting a running total
    sum.update(7);
    EXPECT_EQ(sum.get(), 7);

    // After set(), MIN continues from the set value.
    PcmAccumulator mn(PcmAccOp::MIN);
    mn.set(1000);
    mn.update(1500);
    EXPECT_EQ(mn.get(), 1000);
    mn.update(800);
    EXPECT_EQ(mn.get(), 800);

    PcmAccumulator e(PcmAccOp::EWMA, 0.5);
    e.set(10);
    e.update(20);
    EXPECT_DOUBLE_EQ(e.getFloat(), 15.0);
}
