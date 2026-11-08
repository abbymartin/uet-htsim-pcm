
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "pcm_runtime.h"

namespace {

constexpr int64_t kMtu = 4150;
constexpr int64_t kMaxCwnd = 262926;

class FakeSwiftSender {
public:
    explicit FakeSwiftSender(int64_t cwnd = 12 * kMtu)
        : _ccc(std::make_shared<const PcmAlgorithm>(
                   pcmLoadSpecFile(
                       std::string(PCM_ALGORITHMS_DIR) + "/swift.yaml")),
               0),
          _cwnd(cwnd) {}

    void advanceTicks(uint64_t ticks) {
        _now += ticks * 128000;
    }

    void ack(int64_t rtt, int64_t ack_units = 16) {
        deliver({
            {pcmFindSignal("uet_rx_ack"), 1},
            {pcmFindSignal("uet_rx_ack_bytes"), ack_units},
            {pcmFindSignal("uet_rx_ack_rtt"), rtt}
        });
    }

    void nack(int64_t rtt = 110) {
        deliver({
            {pcmFindSignal("uet_rx_nack"), 1},
            {pcmFindSignal("uet_rx_nack_rtt"), rtt}
        });
    }

    int64_t cwnd() const { return _cwnd; }
    const PcmCcc& ccc() const { return _ccc; }

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

TEST(PcmSwiftTest, Loads) {
    FakeSwiftSender s;
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, LowRttIncreasesCwnd) {
    FakeSwiftSender s;
    const int64_t initial = s.cwnd();

    for (int i = 0; i < 20; ++i)
        s.ack(110);

    EXPECT_GT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, HighRttNackDecreasesCwnd) {
    FakeSwiftSender s;
    const int64_t initial = s.cwnd();

    // RTT above Swift's target indicates congestion.
    s.nack(1000);

    EXPECT_LT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, DecreaseIsGatedByRtt) {
    FakeSwiftSender s;

    s.ack(1000);
    const int64_t after_first = s.cwnd();

    s.ack(1000);

    EXPECT_EQ(s.cwnd(), after_first);

    s.advanceTicks(1001);
    s.ack(1000);

    EXPECT_LT(s.cwnd(), after_first);
    EXPECT_FALSE(s.ccc().inError());
}



TEST(PcmSwiftTest, NeverBelowMinimumCwnd) {
    FakeSwiftSender s;

    for (int i = 0; i < 30; ++i) {
        s.advanceTicks(2000);
        s.ack(10000);
    }

    EXPECT_GE(s.cwnd(), kMtu);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, NeverAboveMaximumCwnd) {
    FakeSwiftSender s(kMaxCwnd);

    for (int i = 0; i < 1000; ++i)
        s.ack(50);

    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, SeparateConnectionsHaveIndependentWindows) {
    FakeSwiftSender a;
    FakeSwiftSender b;

    a.ack(1000);

    EXPECT_LT(a.cwnd(), b.cwnd());
    EXPECT_EQ(b.cwnd(), 12 * kMtu);
    EXPECT_FALSE(a.ccc().inError());
    EXPECT_FALSE(b.ccc().inError());
}

TEST(PcmSwiftTest, LowRttNackDoesNotDecreaseCwnd) {
    FakeSwiftSender s;
    const int64_t initial = s.cwnd();

    s.nack(110);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, InvalidNackRttDoesNotDecreaseCwnd) {
    FakeSwiftSender s;
    const int64_t initial = s.cwnd();

    s.nack(0);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, InvalidAckRttDoesNotChangeCwnd) {
    FakeSwiftSender s;
    const int64_t initial = s.cwnd();

    s.ack(0);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, NackDecreaseIsGatedByRtt) {
    FakeSwiftSender s;

    s.nack(1000);
    const int64_t after_first = s.cwnd();

    // Too soon for another decrease.
    s.nack(1000);

    EXPECT_EQ(s.cwnd(), after_first);

    // Allow the RTT gate to expire.
    s.advanceTicks(1001);
    s.nack(1000);

    EXPECT_LT(s.cwnd(), after_first);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, AckAndNackShareDecreaseGate) {
    FakeSwiftSender s;

    s.ack(1000);
    const int64_t after_ack = s.cwnd();

    // A NACK immediately afterward must not cause
    // another decrease in the same RTT interval.
    s.nack(1000);

    EXPECT_EQ(s.cwnd(), after_ack);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, InitialCwndIsPreserved) {
    FakeSwiftSender s(99600);

    // Invalid RTT invokes the handler without
    // triggering additive increase or decrease.
    s.ack(0);

    EXPECT_EQ(s.cwnd(), 99600);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, FlowScalingChangesCongestionResponse) {
    // Smaller CWND produces a larger flow-scaling delay.
    // RTT 350 is below the small-window target
    // but above the large-window target.
    FakeSwiftSender small(2 * kMtu);
    FakeSwiftSender large(40 * kMtu);

    const int64_t small_initial = small.cwnd();
    const int64_t large_initial = large.cwnd();

    small.ack(350);
    large.ack(350);

    EXPECT_GT(small.cwnd(), small_initial);
    EXPECT_LT(large.cwnd(), large_initial);

    EXPECT_FALSE(small.ccc().inError());
    EXPECT_FALSE(large.ccc().inError());
}

TEST(PcmSwiftTest, RttBelowTargetIncreasesCwnd) {
    FakeSwiftSender s;
    const int64_t initial = s.cwnd();

    for (int i = 0; i < 10; ++i)
        s.ack(100);

    EXPECT_GT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, LargerDelayProducesLargerDecrease) {
    FakeSwiftSender moderate;
    FakeSwiftSender severe;

    moderate.ack(600);
    severe.ack(2000);

    EXPECT_LT(severe.cwnd(), moderate.cwnd());
    EXPECT_FALSE(moderate.ccc().inError());
    EXPECT_FALSE(severe.ccc().inError());
}

TEST(PcmSwiftTest, ChangingRttUpdatesSmoothedRtt) {
    FakeSwiftSender s;

    s.ack(100);
    EXPECT_EQ(std::get<uint64_t>(s.ccc().variable(0)), 100u);

    s.ack(900);

    // EWMA = 7/8 * 100 + 1/8 * 900 = 200.
    EXPECT_EQ(std::get<uint64_t>(s.ccc().variable(0)), 200u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, ShorterRttDoesNotShortenDecreaseGate) {
    FakeSwiftSender s;

    s.ack(2000);
    const int64_t after_first = s.cwnd();

    // The current RTT is smaller than the first RTT,
    // but the previous decrease interval must remain active.
    s.advanceTicks(600);
    s.ack(600);

    EXPECT_EQ(s.cwnd(), after_first);

    // Expire the original 2000-tick gate.
    s.advanceTicks(1500);
    s.ack(600);

    EXPECT_LT(s.cwnd(), after_first);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmSwiftTest, ZeroAckBytesDoNotIncreaseCwnd) {
    FakeSwiftSender s;
    const int64_t initial = s.cwnd();

    s.ack(100, 0);

    EXPECT_EQ(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}
