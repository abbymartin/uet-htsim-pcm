// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
// The DCTCP algorithm (algorithms/dctcp.{yaml,cpp}) on scripted ACKs and NACKs.
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "pcm_runtime.h"

namespace {

constexpr int64_t kMtu = 4150;  // dctcp.yaml's P_MTU

// Stands in for the sender: passes ACKs and NACKs to the CCC with the current
// window, and takes the window the handler sets, as UecSrc does.
class FakeSender {
public:
    explicit FakeSender(int64_t cwnd)
        : _ccc(std::make_shared<const PcmAlgorithm>(
                   pcmLoadSpecFile(std::string(PCM_ALGORITHMS_DIR) + "/dctcp.yaml")),
               0),
          _cwnd(cwnd) {}

    void ack(int64_t packets, bool ecn) {
        deliver({{pcmFindSignal("uet_rx_ack"), 1},
                 {pcmFindSignal("uet_rx_ack_count"), packets},
                 {pcmFindSignal("uet_rx_ack_ecn"), ecn ? 1 : 0}});
    }
    void nack() { deliver({{pcmFindSignal("uet_rx_nack"), 1}}); }

    int64_t cwnd() const { return _cwnd; }
    double alpha() const { return std::get<double>(_ccc.variable(0)); }  // V_ALPHA
    const PcmCcc& ccc() const { return _ccc; }

private:
    void deliver(std::vector<PcmSignalUpdate> u) {
        u.push_back({pcmFindSignal("uet_tx_cwnd"), _cwnd});
        uint64_t before = _ccc.counters().cwnd_updates;
        _ccc.onEvent(u, ++_now);
        if (_ccc.counters().cwnd_updates != before)
            _cwnd = int64_t(*_ccc.cwnd());
    }

    PcmCcc _ccc;
    int64_t _cwnd;
    PcmTime _now = 0;
};

}  // namespace

TEST(PcmDctcpTest, Loads) {
    FakeSender s(10 * kMtu);
    EXPECT_FALSE(s.ccc().inError());
    EXPECT_TRUE(s.ccc().algorithm().warnings().empty());
    EXPECT_DOUBLE_EQ(s.alpha(), 1.0);
}

// A window without marks: alpha decays by (1 - g), and the window grows by
// about one packet.
TEST(PcmDctcpTest, AlphaDecaysWithoutMarks) {
    FakeSender s(10 * kMtu);
    for (int i = 0; i < 10; i++)
        s.ack(1, false);
    EXPECT_DOUBLE_EQ(s.alpha(), 1.0 - 0.0625);
    EXPECT_GT(s.cwnd(), 10 * kMtu + kMtu * 9 / 10);
    EXPECT_LT(s.cwnd(), 11 * kMtu);
}

// alpha = (1 - g) * alpha + g * F, with F the window's marked fraction.
TEST(PcmDctcpTest, AlphaFollowsMarkedFraction) {
    FakeSender s(10 * kMtu);
    for (int i = 0; i < 10; i++)
        s.ack(1, i % 2 == 0);  // 5 of 10 packets marked
    EXPECT_DOUBLE_EQ(s.alpha(), 0.9375 + 0.0625 * 0.5);
}

TEST(PcmDctcpTest, AckCoveringSeveralPackets) {
    FakeSender s(10 * kMtu);
    s.ack(4, true);   // 4 marked
    s.ack(6, false);  // 6 not: the 10-packet window ends here
    EXPECT_DOUBLE_EQ(s.alpha(), 0.9375 + 0.0625 * 0.4);
}

// The first mark in a window cuts it by alpha / 2; later marks in the same
// window don't cut again.
TEST(PcmDctcpTest, CutOncePerWindow) {
    FakeSender s(10 * kMtu);
    s.ack(1, true);  // alpha = 1: halve
    EXPECT_EQ(s.cwnd(), 5 * kMtu);
    s.ack(1, true);
    s.ack(1, true);
    EXPECT_EQ(s.cwnd(), 5 * kMtu);
}

TEST(PcmDctcpTest, NewWindowCanCutAgain) {
    FakeSender s(10 * kMtu);
    for (int i = 0; i < 10; i++)
        s.ack(1, true);  // one cut, then the window ends with alpha = 1
    EXPECT_DOUBLE_EQ(s.alpha(), 1.0);
    const int64_t before = s.cwnd();
    s.ack(1, true);  // first mark of the next window
    EXPECT_EQ(s.cwnd(), before / 2);
}

TEST(PcmDctcpTest, SmallAlphaCutsLess) {
    FakeSender s(100 * kMtu);
    for (int w = 0; w < 20; w++)  // 20 clean windows: alpha = 0.9375^20
        for (int64_t i = 0, n = s.cwnd() / kMtu; i < n; i++)
            s.ack(1, false);
    const double alpha = s.alpha();
    EXPECT_NEAR(alpha, 0.2753, 0.001);
    const int64_t before = s.cwnd();
    s.ack(1, true);
    EXPECT_EQ(s.cwnd(), int64_t(double(before) * (1 - alpha / 2)));
}

// A NACK halves the window, but not when the window was already cut.
TEST(PcmDctcpTest, NackHalvesOncePerWindow) {
    FakeSender s(10 * kMtu);
    s.nack();
    EXPECT_EQ(s.cwnd(), 5 * kMtu);
    s.nack();
    EXPECT_EQ(s.cwnd(), 5 * kMtu);

    FakeSender t(10 * kMtu);
    t.ack(1, true);  // ECN cut
    t.nack();        // same window: no second cut
    EXPECT_EQ(t.cwnd(), 5 * kMtu);
}

// Under heavy trimming almost nothing is ACKed. NACKed packets still end
// windows, so the window keeps halving, once per window, down to one packet.
TEST(PcmDctcpTest, OnlyNacksStillShrinkTheWindow) {
    FakeSender s(16 * kMtu);
    std::vector<int64_t> windows;
    for (int i = 0; i < 31; i++) {
        s.nack();
        windows.push_back(s.cwnd() / kMtu);
    }
    // 16 NACKs end the first window (cut to 8), the next 8 end the second
    // (cut to 4), then 4 (to 2), then 2 (to 1).
    EXPECT_EQ(windows[0], 8);
    EXPECT_EQ(windows[16], 4);
    EXPECT_EQ(windows[24], 2);
    EXPECT_EQ(windows[28], 1);
    EXPECT_EQ(windows[30], 1);
    // NACKed packets count as congested: alpha stays at 1.
    EXPECT_DOUBLE_EQ(s.alpha(), 1.0);
}

TEST(PcmDctcpTest, NeverBelowOnePacket) {
    FakeSender s(2 * kMtu);
    for (int i = 0; i < 50; i++) {
        s.nack();
        s.ack(1, true);
    }
    EXPECT_GE(s.cwnd(), kMtu);
    EXPECT_FALSE(s.ccc().inError());
}
