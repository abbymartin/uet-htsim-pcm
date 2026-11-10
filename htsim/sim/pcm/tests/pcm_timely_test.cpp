
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include "pcm_runtime.h"

namespace {

constexpr int64_t kMinCwnd = 4150;
constexpr int64_t kMaxCwnd = 262926;
constexpr int64_t kInitialCwnd = 49800;

class FakeTimelySender {
public:
    explicit FakeTimelySender(int64_t cwnd = kInitialCwnd)
        : _ccc(std::make_shared<const PcmAlgorithm>(
            pcmLoadSpecFile(
                std::string(PCM_ALGORITHMS_DIR) + "/timely.yaml")), 0),
          _cwnd(cwnd) {}

    void advanceTicks(uint64_t ticks) {
        _now += ticks * 128000;
    }

    void ack(int64_t rtt) {
        deliver({
            {pcmFindSignal("uet_rx_ack"), 1},
            {pcmFindSignal("uet_rx_ack_bytes"), 16},
            {pcmFindSignal("uet_rx_ack_rtt"), rtt}
        });
    }

    void nack(int64_t rtt = 250) {
        deliver({
            {pcmFindSignal("uet_rx_nack"), 1},
            {pcmFindSignal("uet_rx_nack_rtt"), rtt}
        });
    }

    int64_t cwnd() const { return _cwnd; }
    const PcmCcc& ccc() const { return _ccc; }

    uint64_t previousRtt() const {
        return std::get<uint64_t>(_ccc.variable(0));
    }

    double gradient() const {
        return std::get<double>(_ccc.variable(1));
    }

private:
    void deliver(std::vector<PcmSignalUpdate> updates) {
        _now += 128000;

        updates.push_back({
            pcmFindSignal("uet_time_current"),
            static_cast<int64_t>(_now / 128000)
        });
        updates.push_back({
            pcmFindSignal("uet_tx_cwnd"), _cwnd
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

TEST(PcmTimelyTest, Loads) {
    FakeTimelySender s;
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, LowRttIncreasesCwnd) {
    FakeTimelySender s;
    s.ack(200);  // Initialize RTT history
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.ack(200);

    EXPECT_GT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, HighRttDecreasesCwnd) {
    FakeTimelySender s;
    s.ack(5000);
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.ack(5000);

    EXPECT_LT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, HighRttNackDecreasesCwnd) {
    FakeTimelySender s;
    s.ack(200);
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.nack(5000);

    EXPECT_LT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, NackDoesNotIncreaseCwnd) {
    FakeTimelySender s;
    s.ack(200);
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.nack(100);

    EXPECT_LE(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, UpdateIntervalIsEnforced) {
    FakeTimelySender s;
    s.ack(5000);

    s.advanceTicks(200);
    s.ack(5000);
    const int64_t after_decrease = s.cwnd();

    s.ack(5000);

    EXPECT_EQ(s.cwnd(), after_decrease);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, NeverBelowMinimumCwnd) {
    FakeTimelySender s;

    s.ack(5000);

    for (int i = 0; i < 30; ++i) {
        s.advanceTicks(200);
        s.nack();
    }

    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, NeverAboveMaximumCwnd) {
    FakeTimelySender s(kMaxCwnd);

    s.ack(100);

    for (int i = 0; i < 100; ++i) {
        s.advanceTicks(200);
        s.ack(100);
    }

    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, ConnectionsHaveIndependentWindows) {
    FakeTimelySender a;
    FakeTimelySender b;

    a.ack(5000);
    a.advanceTicks(200);
    a.ack(5000);

    EXPECT_LT(a.cwnd(), b.cwnd());
    EXPECT_EQ(b.cwnd(), kInitialCwnd);
    EXPECT_FALSE(a.ccc().inError());
    EXPECT_FALSE(b.ccc().inError());
}
TEST(PcmTimelyTest, LowRttNackDoesNotDecreaseCwnd) {
    FakeTimelySender s;
    s.ack(200);

    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.nack(250);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, InvalidAckRttDoesNotChangeCwnd) {
    FakeTimelySender s;
    const int64_t initial = s.cwnd();

    s.ack(0);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, InvalidNackRttDoesNotChangeCwnd) {
    FakeTimelySender s;
    const int64_t initial = s.cwnd();

    s.nack(0);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, FirstValidRttInitializesWithoutAdjustment) {
    FakeTimelySender s;
    const int64_t initial = s.cwnd();

    s.ack(5000);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_EQ(
        std::get<uint64_t>(s.ccc().variable(0)),
        5000u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, NackReductionRespectsUpdateInterval) {
    FakeTimelySender s;
    s.ack(200);

    s.advanceTicks(200);
    s.nack(5000);

    const int64_t after_first = s.cwnd();

    s.nack(5000);

    EXPECT_EQ(s.cwnd(), after_first);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, GradientUpdatesOnlyAtControlInterval) {
    FakeTimelySender s;

    s.ack(1000);

    // RTT changes, but not enough time has elapsed.
    s.ack(1500);

    EXPECT_EQ(s.previousRtt(), 1000u);
    EXPECT_NEAR(s.gradient(), 0.0, 1e-9);

    // A control update is now due.
    s.advanceTicks(200);
    s.ack(1500);

    EXPECT_EQ(s.previousRtt(), 1500u);

    // Raw relative gradient = (1500 - 1000) / 1000 = 0.5.
    // Smoothed gradient = 0.8 * 0 + 0.2 * 0.5 = 0.1.
    EXPECT_NEAR(s.gradient(), 0.1, 1e-6);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, RisingRttDecreasesCwnd) {
    FakeTimelySender s;

    s.ack(1000);
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.ack(2000);

    EXPECT_LT(s.cwnd(), initial);
    EXPECT_GT(s.gradient(), 0.0);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, FallingRttIncreasesCwnd) {
    FakeTimelySender s;

    s.ack(2000);
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.ack(1000);

    EXPECT_GT(s.cwnd(), initial);
    EXPECT_LT(s.gradient(), 0.0);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, LowRttThresholdOverridesRisingGradient) {
    FakeTimelySender s;

    s.ack(100);
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.ack(300);

    // RTT increased, but remains below T_LOW = 391.
    EXPECT_GT(s.gradient(), 0.0);
    EXPECT_GT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, HighRttThresholdOverridesFallingGradient) {
    FakeTimelySender s;

    s.ack(6000);
    const int64_t initial = s.cwnd();

    s.advanceTicks(200);
    s.ack(5000);

    // RTT decreased, but remains above T_HIGH = 3910.
    EXPECT_LT(s.gradient(), 0.0);
    EXPECT_LT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmTimelyTest, InvalidRttPreservesGradientState) {
    FakeTimelySender s;

    s.ack(1000);

    s.advanceTicks(200);
    s.ack(1500);

    const uint64_t previous = s.previousRtt();
    const double gradient_before = s.gradient();
    const int64_t cwnd_before = s.cwnd();

    s.ack(0);
    s.nack(0);

    EXPECT_EQ(s.previousRtt(), previous);
    EXPECT_NEAR(s.gradient(), gradient_before, 1e-9);
    EXPECT_EQ(s.cwnd(), cwnd_before);
    EXPECT_FALSE(s.ccc().inError());
}
