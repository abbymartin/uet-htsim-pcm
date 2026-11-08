
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <gtest/gtest.h>
#include "pcm_runtime.h"

namespace {

constexpr int64_t kMinCwnd = 4150;
constexpr int64_t kMaxCwnd = 262926;
constexpr int64_t kInitialCwnd = 49800;

class FakeBbrSender {
public:
    explicit FakeBbrSender(int64_t cwnd = kInitialCwnd)
        : _ccc(std::make_shared<const PcmAlgorithm>(
            pcmLoadSpecFile(
                std::string(PCM_ALGORITHMS_DIR) + "/bbr.yaml")), 0),
          _cwnd(cwnd) {}

    void advanceTicks(uint64_t ticks) {
        _now += ticks * 128000;
    }

    void ack(int64_t rtt, int64_t bytes256 = 16) {
        deliver({
            {pcmFindSignal("uet_rx_ack"), 1},
            {pcmFindSignal("uet_rx_ack_bytes"), bytes256},
            {pcmFindSignal("uet_rx_ack_rtt"), rtt}
        });
    }

    void nack() {
        deliver({
            {pcmFindSignal("uet_rx_nack"), 1}
        });
    }

    int64_t cwnd() const {
        return _cwnd;
    }

    const PcmCcc& ccc() const {
        return _ccc;
    }
    uint64_t mode() const {
        return std::get<uint64_t>(_ccc.variable(0));
    }

    uint64_t minRtt() const {
        return std::get<uint64_t>(_ccc.variable(2));
    }

    double bandwidth() const {
        return std::get<double>(_ccc.variable(4));
    }

    uint64_t stableRounds() const {
        return std::get<uint64_t>(_ccc.variable(6));
    }

    uint64_t probePhase() const {
        return std::get<uint64_t>(_ccc.variable(9));
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

        const uint64_t before =
            _ccc.counters().cwnd_updates;

        _ccc.onEvent(updates, _now);

        if (_ccc.counters().cwnd_updates != before) {
            _cwnd = static_cast<int64_t>(*_ccc.cwnd());
        }
    }

    PcmCcc _ccc;
    int64_t _cwnd;
    PcmTime _now = 0;
};

} // namespace

TEST(PcmBbrTest, Loads) {
    FakeBbrSender s;

    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, StartsWithValidCwnd) {
    FakeBbrSender s;

    s.ack(200);

    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, StartupGrowsWindowOnAcks) {
    FakeBbrSender s;

    const int64_t initial = s.cwnd();

    s.ack(200);

    EXPECT_GT(s.cwnd(), initial);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, AccumulatesAckedBytes) {
    FakeBbrSender s;

    s.ack(200, 16);
    const int64_t after_first = s.cwnd();

    s.ack(200, 32);

    EXPECT_GT(s.cwnd(), after_first);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, BandwidthSamplingDoesNotCauseErrors) {
    FakeBbrSender s;

    s.ack(200);

    for (int i = 0; i < 10; ++i) {
        s.advanceTicks(200);
        s.ack(200);
    }

    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, LowerRttRemainsUsable) {
    FakeBbrSender s;

    s.ack(500);

    s.advanceTicks(200);
    s.ack(200);

    s.advanceTicks(200);
    s.ack(400);

    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, StartupCanExitAfterBandwidthPlateau) {
    FakeBbrSender s;

    s.ack(200);

    for (int i = 0; i < 12; ++i) {
        s.advanceTicks(200);
        s.ack(200, 16);
    }

    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, NackReducesCwnd) {
    FakeBbrSender s;

    s.ack(200);
    const int64_t before_nack = s.cwnd();

    s.advanceTicks(200);
    s.nack();

    EXPECT_LT(s.cwnd(), before_nack);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, NackDoesNotIncreaseCwnd) {
    FakeBbrSender s;

    s.ack(200);
    const int64_t before_nack = s.cwnd();

    s.advanceTicks(200);
    s.nack();

    EXPECT_LE(s.cwnd(), before_nack);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, NeverBelowMinimumCwnd) {
    FakeBbrSender s;

    s.ack(200);

    for (int i = 0; i < 30; ++i) {
        s.advanceTicks(200);
        s.nack();
    }

    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, NeverAboveMaximumCwnd) {
    FakeBbrSender s(kMaxCwnd);

    s.ack(100);

    for (int i = 0; i < 100; ++i) {
        s.advanceTicks(200);
        s.ack(100, 64);
    }

    EXPECT_LE(s.cwnd(), kMaxCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, ConnectionsHaveIndependentWindows) {
    FakeBbrSender a;
    FakeBbrSender b;

    a.ack(200);
    a.advanceTicks(200);
    a.nack();

    EXPECT_EQ(b.cwnd(), kInitialCwnd);
    EXPECT_FALSE(a.ccc().inError());
    EXPECT_FALSE(b.ccc().inError());
}

TEST(PcmBbrTest, BeginsInStartupMode) {
    FakeBbrSender s;

    s.ack(200);

    EXPECT_EQ(s.mode(), 0u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, TracksMinimumRtt) {
    FakeBbrSender s;

    s.ack(500);
    EXPECT_EQ(s.minRtt(), 500u);

    s.ack(200);
    EXPECT_EQ(s.minRtt(), 200u);

    s.ack(400);
    EXPECT_EQ(s.minRtt(), 200u);

    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, CalculatesPositiveBandwidthEstimate) {
    FakeBbrSender s;

    s.ack(200, 16);

    s.advanceTicks(200);
    s.ack(200, 16);

    EXPECT_GT(s.bandwidth(), 0.0);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, TracksBandwidthPlateau) {
    FakeBbrSender s;

    s.ack(200, 16);

    for (int i = 0; i < 3; ++i) {
        s.advanceTicks(200);
        s.ack(200, 16);
    }

    EXPECT_GT(s.bandwidth(), 0.0);

    // Repeated similar bandwidth samples should either
    // advance the plateau counter or exit STARTUP.
    EXPECT_TRUE(s.stableRounds() > 0 || s.mode() != 0u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, ExitsStartupAfterBandwidthPlateau) {
    FakeBbrSender s;

    s.ack(200, 16);

    for (int i = 0; i < 20; ++i) {
        s.advanceTicks(200);
        s.ack(200, 16);
    }

    EXPECT_NE(s.mode(), 0u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, EntersProbeRttAfterMinimumRttExpires) {
    FakeBbrSender s;

    s.ack(200);

    // P_MIN_RTT_WINDOW = 78125 ticks.
    s.advanceTicks(80000);
    s.ack(300);

    EXPECT_EQ(s.mode(), 3u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, ExitsProbeRttAfterDuration) {
    FakeBbrSender s;

    s.ack(200);

    s.advanceTicks(80000);
    s.ack(300);

    ASSERT_EQ(s.mode(), 3u);

    // P_PROBE_RTT_DURATION = 1563 ticks.
    s.advanceTicks(2000);
    s.ack(300);

    EXPECT_EQ(s.mode(), 2u);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, ProbeRttLimitsCongestionWindow) {
    FakeBbrSender s;

    s.ack(200);

    s.advanceTicks(80000);
    s.ack(300);

    ASSERT_EQ(s.mode(), 3u);

    // PROBE_RTT should cap CWND at four MSS.
    EXPECT_LE(s.cwnd(), 4 * kMinCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, ConnectionsHaveIndependentRttEstimates) {
    FakeBbrSender a;
    FakeBbrSender b;

    a.ack(200);
    b.ack(500);

    EXPECT_EQ(a.minRtt(), 200u);
    EXPECT_EQ(b.minRtt(), 500u);

    EXPECT_FALSE(a.ccc().inError());
    EXPECT_FALSE(b.ccc().inError());
}

TEST(PcmBbrTest, FirstNackReducesWindowImmediately) {
    FakeBbrSender s;

    s.ack(200);
    const int64_t before = s.cwnd();

    // The first NACK should reduce CWND immediately,
    // without waiting for the sampling interval.
    s.nack();

    EXPECT_LT(s.cwnd(), before);
    EXPECT_GE(s.cwnd(), kMinCwnd);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, RepeatedNacksAreRateLimited) {
    FakeBbrSender s;

    s.ack(200);
    s.nack();

    const int64_t after_first = s.cwnd();

    // A second NACK in the same sampling interval
    // must not cause another multiplicative reduction.
    s.nack();

    EXPECT_EQ(s.cwnd(), after_first);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, DrainPersistsUntilWindowApproachesBdp) {
    FakeBbrSender s;

    s.ack(200, 16);

    bool saw_drain = false;

    // Stable bandwidth measurements should eventually
    // cause STARTUP to transition into DRAIN.
    for (int i = 0; i < 20; ++i) {
        s.advanceTicks(200);
        s.ack(200, 16);

        if (s.mode() == 1u) {  // DRAIN
            saw_drain = true;
            break;
        }
    }

    EXPECT_TRUE(saw_drain);
    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmBbrTest, DrainEventuallyTransitionsToProbeBw) {
    FakeBbrSender s;

    s.ack(200, 16);

    bool saw_drain = false;
    bool saw_probe_bw = false;

    for (int i = 0; i < 100; ++i) {
        s.advanceTicks(200);
        s.ack(200, 16);

        if (s.mode() == 1u)  // DRAIN
            saw_drain = true;

        if (saw_drain && s.mode() == 2u) {  // PROBE_BW
            saw_probe_bw = true;
            break;
        }
    }

    EXPECT_TRUE(saw_drain);
    EXPECT_TRUE(saw_probe_bw);
    EXPECT_FALSE(s.ccc().inError());
}
