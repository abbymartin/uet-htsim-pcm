
 // -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
 // Tests for the PCM DCQCN approximation.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "pcm_runtime.h"

namespace {

constexpr int64_t kMinCwnd = 4150;
constexpr int64_t kMaxCwnd = 262926;
constexpr uint64_t kUpdatePeriod = 430; // 55 us in 128-ns ticks
constexpr uint64_t kCnpInterval = 391;

class FakeDcqcnSender {
public:
    explicit FakeDcqcnSender(int64_t initial_cwnd = kMaxCwnd)
        : _ccc(std::make_shared<const PcmAlgorithm>(
                pcmLoadSpecFile(
                    std::string(PCM_ALGORITHMS_DIR) + "/dcqcn.yaml")),
            0),
        _cwnd(initial_cwnd) {}

    void advanceTicks(uint64_t ticks) {
        _now += ticks * 128000;
    }

    void ack(bool ecn = false) {
        deliver({
            {pcmFindSignal("uet_rx_ack"), 1},
            {pcmFindSignal("uet_rx_ack_bytes"), 16},
            {pcmFindSignal("uet_rx_ack_ecn"), ecn ? 1 : 0}
        });
    }

    void nack(bool ecn = false) {
        deliver({
            {pcmFindSignal("uet_rx_nack"), 1},
            {pcmFindSignal("uet_rx_nack_ecn"), ecn ? 1 : 0}
        });
    }

    // Supplies current time without ACK or NACK feedback.
    // This also allows the PCM timer trigger to run.
    void tick() {
        deliver({});
    }

    int64_t cwnd() const {
        return _cwnd;
    }

    const PcmCcc& ccc() const {
        return _ccc;
    }

    double alpha() const {
        return std::get<double>(_ccc.variable(0));
    }

    double rate() const {
        return std::get<double>(_ccc.variable(1));
    }

    double targetRate() const {
        return std::get<double>(_ccc.variable(2));
    }

    uint64_t recoverySteps() const {
        return std::get<uint64_t>(_ccc.variable(6));
    }

private:
    void deliver(std::vector<PcmSignalUpdate> updates) {
        _now += 128000;

        updates.push_back({
            pcmFindSignal("uet_time_current"),
            static_cast<int64_t>(_now / 128000)
        });

        updates.push_back({
            pcmFindSignal("uet_tx_cwnd"),
            _cwnd
        });

        const uint64_t before = _ccc.counters().cwnd_updates;

        _ccc.onEvent(updates, _now);

        if (_ccc.counters().cwnd_updates != before)
            _cwnd = static_cast<int64_t>(*_ccc.cwnd());
    }

    PcmCcc _ccc;
    int64_t _cwnd;
    PcmTime _now = 0;
};

} // namespace

TEST(PcmDcqcnTest, Loads) {
    FakeDcqcnSender s;

    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, EcnAckReducesCwnd) {
    FakeDcqcnSender s;
    const int64_t initial = s.cwnd();

    s.ack(true);

    EXPECT_LT(s.cwnd(), initial);
    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, EcnNackReducesCwnd) {
    FakeDcqcnSender s;
    const int64_t initial = s.cwnd();

    s.nack(true);

    EXPECT_LT(s.cwnd(), initial);
    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, UnmarkedAckDoesNotReduceCwnd) {
    FakeDcqcnSender s;
    const int64_t initial = s.cwnd();

    s.ack(false);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, UnmarkedNackDoesNotReduceCwnd) {
    FakeDcqcnSender s;
    const int64_t initial = s.cwnd();

    s.nack(false);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, TimerRecoveryIncreasesCwnd) {
    FakeDcqcnSender s;

    // Congestion causes the initial rate reduction.
    s.ack(true);
    const int64_t reduced = s.cwnd();

    // Move beyond the periodic recovery interval.
    s.advanceTicks(kUpdatePeriod + 1);
    s.tick();

    EXPECT_GT(s.cwnd(), reduced);
    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, NeverBelowMinimumCwnd) {
    FakeDcqcnSender s;

    // Allow repeated ECN responses by spacing them
    // beyond the minimum CNP interval.
    for (int i = 0; i < 30; ++i) {
        s.advanceTicks(kCnpInterval + 1);
        s.ack(true);
    }

    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, NeverAboveReferenceCwnd) {
    FakeDcqcnSender s;

    // Exercise periodic recovery without congestion.
    for (int i = 0; i < 30; ++i) {
        s.advanceTicks(kUpdatePeriod + 1);
        s.tick();
    }

    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, SeparateConnectionsHaveIndependentWindows) {
    FakeDcqcnSender a;
    FakeDcqcnSender b;

    a.ack(true);

    EXPECT_LT(a.cwnd(), kMaxCwnd);
    EXPECT_EQ(b.cwnd(), kMaxCwnd);

    b.nack(true);

    EXPECT_LT(b.cwnd(), kMaxCwnd);
    EXPECT_FALSE(a.ccc().inError());
    EXPECT_FALSE(b.ccc().inError());
}

TEST(PcmDcqcnTest, StartsWithFullNormalizedRate) {
    FakeDcqcnSender s;

    // The first event initializes the DCQCN state.
    s.ack(false);

    EXPECT_DOUBLE_EQ(s.rate(), 1.0);
    EXPECT_DOUBLE_EQ(s.targetRate(), 1.0);
    EXPECT_EQ(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, EcnFeedbackUpdatesRateAndAlpha) {
    FakeDcqcnSender s;

    s.ack(true);

    // Congestion feedback must reduce the sending rate.
    EXPECT_LT(s.rate(), 1.0);
    EXPECT_GE(s.rate(), 0.02);

    // Alpha remains a normalized congestion estimate.
    EXPECT_GE(s.alpha(), 0.0);
    EXPECT_LE(s.alpha(), 1.0);

    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, RepeatedEcnWithinCnpIntervalIsLimited) {
    FakeDcqcnSender s;

    s.ack(true);
    const double rate_after_first = s.rate();

    // Only one tick passes, less than P_CNP_INTERVAL.
    s.ack(true);

    EXPECT_DOUBLE_EQ(s.rate(), rate_after_first);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, RecoveryStepsAdvanceWithTimer) {
    FakeDcqcnSender s;

    s.ack(true);
    EXPECT_EQ(s.recoverySteps(), 0u);

    s.advanceTicks(kUpdatePeriod + 1);
    s.tick();

    EXPECT_EQ(s.recoverySteps(), 1u);
    EXPECT_FALSE(s.ccc().inError());
}
TEST(PcmDcqcnTest, PreservesSimulatorInitialCwnd) {
    FakeDcqcnSender s(49800);

    s.ack(false);

    EXPECT_EQ(s.cwnd(), 49800);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, SeparateConnectionsPreserveOwnInitialCwnd) {
    FakeDcqcnSender a(49800);
    FakeDcqcnSender b(99600);

    a.ack(false);
    b.ack(false);

    EXPECT_EQ(a.cwnd(), 49800);
    EXPECT_EQ(b.cwnd(), 99600);

    a.ack(true);

    EXPECT_LT(a.cwnd(), 49800);
    EXPECT_EQ(b.cwnd(), 99600);

    EXPECT_FALSE(a.ccc().inError());
    EXPECT_FALSE(b.ccc().inError());
}

TEST(PcmDcqcnTest, FastRecoveryUsesMidpoint) {
    FakeDcqcnSender s;

    s.ack(true);

    const double reduced_rate = s.rate();
    const double target_rate = s.targetRate();

    s.advanceTicks(kUpdatePeriod + 1);
    s.tick();

    const double expected =
        (target_rate + reduced_rate) / 2.0;

    EXPECT_NEAR(s.rate(), expected, 1e-6);
    EXPECT_EQ(s.recoverySteps(), 1u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, ActiveIncreaseUsesRai) {
    FakeDcqcnSender s;

    s.ack(true);

    // Complete five fast-recovery steps.
    for (int i = 0; i < 5; ++i) {
        s.advanceTicks(kUpdatePeriod + 1);
        s.tick();
    }

    EXPECT_EQ(s.recoverySteps(), 5u);

    const double previous_target = s.targetRate();

    // Step six enters active increase.
    s.advanceTicks(kUpdatePeriod + 1);
    s.tick();

    EXPECT_EQ(s.recoverySteps(), 6u);

    const double expected_target =
        std::min(1.0, previous_target + 0.05);

    EXPECT_NEAR(s.targetRate(), expected_target, 1e-6);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, HyperactiveIncreaseUsesRhai) {
    FakeDcqcnSender s;

    // First congestion notification.
    s.ack(true);

    // Second notification after the CNP interval,
    // but before the first recovery timer.
    s.advanceTicks(kCnpInterval + 1);
    s.ack(true);

    // The second notification saves a lower target rate.
    EXPECT_LT(s.targetRate(), 1.0);

    // Complete 5 fast-recovery and 5 active-increase steps.
    for (int i = 0; i < 10; ++i) {
        s.advanceTicks(kUpdatePeriod + 1);
        s.tick();
    }

    ASSERT_EQ(s.recoverySteps(), 10u);

    const double previous_target = s.targetRate();

    // Ensure the target has not saturated before RHAI.
    ASSERT_LT(previous_target, 0.90);

    // Step 11 must use P_RHAI = 0.10, not P_RAI = 0.05.
    s.advanceTicks(kUpdatePeriod + 1);
    s.tick();

    EXPECT_EQ(s.recoverySteps(), 11u);
    EXPECT_NEAR(
        s.targetRate(),
        previous_target + 0.10,
        1e-6);

    EXPECT_FALSE(s.ccc().inError());
}


TEST(PcmDcqcnTest, CongestionResetsRecoveryStages) {
    FakeDcqcnSender s;

    s.ack(true);

    // Advance into active increase.
    for (int i = 0; i < 7; ++i) {
        s.advanceTicks(kUpdatePeriod + 1);
        s.tick();
    }

    EXPECT_EQ(s.recoverySteps(), 7u);

    // New ECN feedback should reset the recovery stage.
    s.ack(true);

    EXPECT_EQ(s.recoverySteps(), 0u);

    s.advanceTicks(kUpdatePeriod + 1);
    s.tick();

    EXPECT_EQ(s.recoverySteps(), 1u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, EcnWithinCnpIntervalDoesNotReduceRateTwice) {
    FakeDcqcnSender s;

    s.ack(true);
    const double first_rate = s.rate();

    // A second congestion notification arrives too soon.
    s.ack(true);

    EXPECT_DOUBLE_EQ(s.rate(), first_rate);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, EcnAfterCnpIntervalReducesRateAgain) {
    FakeDcqcnSender s;

    s.ack(true);
    const double first_rate = s.rate();

    s.advanceTicks(kCnpInterval + 1);
    s.ack(true);

    EXPECT_LT(s.rate(), first_rate);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmDcqcnTest, SimultaneousRecoveryAndEcnResetsSteps) {
    FakeDcqcnSender s;

    s.ack(true);

    // Cross both the recovery and CNP intervals.
    s.advanceTicks(kUpdatePeriod + 1);

    // Recovery timer and ECN feedback are due together.
    s.ack(true);

    // The accepted congestion notification must reset
    // the recovery sequence even if the timer also fires.
    EXPECT_EQ(s.recoverySteps(), 0u);
    EXPECT_LT(s.rate(), 1.0);
    EXPECT_FALSE(s.ccc().inError());
}
