
 // -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
 // Tests for the PCM NSCC approximation (algorithms/nscc.{yaml,cpp}).
 #include <cstdint>
 #include <memory>
 #include <string>
 #include <vector>

 #include <gtest/gtest.h>

 #include "pcm_runtime.h"

 namespace {

 constexpr int64_t kMtu = 4150;
 constexpr int64_t kMaxCwnd = 262926;
 constexpr int64_t kBaseRtt = 110; // 128-ns ticks in spec mode

 // Mimics UecSrc: supplies current CWND with each event and applies
 // the window returned by the PCM handler.
 class FakeNsccSender {
 public:
     void advanceTicks(uint64_t ticks) {
         _now += ticks * 128000;
     }
     explicit FakeNsccSender(int64_t cwnd)
         : _ccc(std::make_shared<const PcmAlgorithm>(
                    pcmLoadSpecFile(
                        std::string(PCM_ALGORITHMS_DIR) + "/nscc.yaml")),
                0),
           _cwnd(cwnd) {}

     // ACK bytes are in 256-byte units in PCM spec mode.
     // RTT values are in 128-ns units.
     void ack(int64_t ack_units, bool ecn = false,
              int64_t rtt = kBaseRtt) {
         deliver({
             {pcmFindSignal("uet_rx_ack"), 1},
             {pcmFindSignal("uet_rx_ack_bytes"), ack_units},
             {pcmFindSignal("uet_rx_ack_ecn"), ecn ? 1 : 0},
             {pcmFindSignal("uet_rx_ack_rtt"), rtt}
         });
     }

     // Vendor NACK bytes are raw bytes, not 256-byte units.
     void nack(int64_t bytes = kMtu) {
         deliver({
             {pcmFindSignal("uet_rx_nack"), 1},
             {pcmFindSignal("uet_rx_nack_vend_htsim_bytes"), bytes},
             {pcmFindSignal("uet_rx_nack_last_hop_trim"), 0},
             {pcmFindSignal("uet_rx_nack_rtt"), kBaseRtt}
         });
     }

     int64_t cwnd() const { return _cwnd; }
     void setInflight(int64_t bytes) {
         _inflight = bytes;
     }
     const PcmCcc& ccc() const { return _ccc; }
     int64_t _inflight = 0;

 private:
    void deliver(std::vector<PcmSignalUpdate> updates) {
        updates.push_back({pcmFindSignal("uet_tx_cwnd"), _cwnd});
        updates.push_back({
            pcmFindSignal("uet_tx_inflight_bytes"),
            _inflight
        });
        // Advance simulated time by one PCM spec tick (128 ns).
        // PcmTime is expressed in picoseconds.
        _now += 128000;

        updates.push_back({
            pcmFindSignal("uet_time_current"),
            static_cast<int64_t>(_now / 128000)
        });

        const uint64_t before = _ccc.counters().cwnd_updates;
        _ccc.onEvent(updates, _now);

         if (_ccc.counters().cwnd_updates != before)
             _cwnd = int64_t(*_ccc.cwnd());
     }

     PcmCcc _ccc;
     int64_t _cwnd;
     PcmTime _now = 0;
 };

 } // namespace

 TEST(PcmNsccTest, Loads) {
     FakeNsccSender s(10 * kMtu);

     EXPECT_FALSE(s.ccc().inError());
 }

 TEST(PcmNsccTest, CleanAcksIncreaseWindow) {
     FakeNsccSender s(10 * kMtu);
     const int64_t initial = s.cwnd();

     // Exceed the YAML's 33200 adjustment-unit threshold.
     // Low queueing delay should accumulate proportional increase.
     for (int i = 0; i < 140; ++i)
         s.ack(256, false, kBaseRtt + 20);

     EXPECT_GT(s.cwnd(), initial);
     EXPECT_LE(s.cwnd(), kMaxCwnd);
     EXPECT_FALSE(s.ccc().inError());
 }

 TEST(PcmNsccTest, NackReducesWindow) {
     FakeNsccSender s(10 * kMtu);
     const int64_t initial = s.cwnd();

     s.nack();

     EXPECT_EQ(s.cwnd(), initial - kMtu);
     EXPECT_FALSE(s.ccc().inError());
 }

 TEST(PcmNsccTest, MultipleNacksReduceWindow) {
     FakeNsccSender s(10 * kMtu);

     s.nack();
     s.nack();
     s.nack();

     EXPECT_EQ(s.cwnd(), 7 * kMtu);
     EXPECT_FALSE(s.ccc().inError());
 }

 TEST(PcmNsccTest, NeverBelowOnePacket) {
     FakeNsccSender s(2 * kMtu);

     for (int i = 0; i < 30; ++i)
         s.nack();

     EXPECT_EQ(s.cwnd(), kMtu);
     EXPECT_FALSE(s.ccc().inError());
 }

 TEST(PcmNsccTest, NeverAboveMaximumWindow) {
     FakeNsccSender s(kMaxCwnd);

     for (int i = 0; i < 500; ++i)
         s.ack(256, false, kBaseRtt + 20);

     EXPECT_LE(s.cwnd(), kMaxCwnd);
     EXPECT_GE(s.cwnd(), kMtu);
     EXPECT_FALSE(s.ccc().inError());
 }

 TEST(PcmNsccTest, SeparateConnectionsHaveIndependentWindows) {
     FakeNsccSender a(10 * kMtu);
     FakeNsccSender b(10 * kMtu);

     a.nack();

     EXPECT_EQ(a.cwnd(), 9 * kMtu);
     EXPECT_EQ(b.cwnd(), 10 * kMtu);

     b.nack();
     b.nack();

     EXPECT_EQ(a.cwnd(), 9 * kMtu);
     EXPECT_EQ(b.cwnd(), 8 * kMtu);
     EXPECT_FALSE(a.ccc().inError());
     EXPECT_FALSE(b.ccc().inError());
 }
 TEST(PcmNsccTest, EcnDecreaseOccursOnlyOncePerBaseRtt) {
     FakeNsccSender s(10 * kMtu);
     const int64_t initial = s.cwnd();

     // Warm up the EWMA with ECN-marked, high-delay ACKs.
     // All samples remain within the initial base RTT.
     for (int i = 0; i < 100; ++i)
         s.ack(0, true, kBaseRtt + 200);

     EXPECT_EQ(s.cwnd(), initial);

     // Advance beyond the initial base RTT.
     s.advanceTicks(kBaseRtt + 1);

     // First eligible decrease.
     s.ack(0, true, kBaseRtt + 200);
     const int64_t after_first_decrease = s.cwnd();

     EXPECT_LT(after_first_decrease, initial);

     // Another ACK immediately afterward must not decrease again.
     s.ack(0, true, kBaseRtt + 200);
     EXPECT_EQ(s.cwnd(), after_first_decrease);

     // Advance beyond the next base RTT.
     s.advanceTicks(kBaseRtt + 1);

     // A second decrease should now be permitted.
     s.ack(0, true, kBaseRtt + 200);
     EXPECT_LT(s.cwnd(), after_first_decrease);

     EXPECT_FALSE(s.ccc().inError());
 }
 TEST(PcmNsccTest, CongestedAckDoesNotGrowWindow) {
     FakeNsccSender s(10 * kMtu);
     const int64_t initial = s.cwnd();

     // ECN-marked ACK with queueing delay above target.
     s.ack(16, true, kBaseRtt + 150);

     EXPECT_LE(s.cwnd(), initial);
     EXPECT_FALSE(s.ccc().inError());
 }
 TEST(PcmNsccTest, EcnDecreaseIsGatedByBaseRtt) {
    FakeNsccSender s(10 * kMtu);

    // Repeated ECN ACKs within the initial RTT must not
    // cause repeated multiplicative decreases.
    for (int i = 0; i < 50; ++i)
        s.ack(0, true, kBaseRtt + 200);

    EXPECT_EQ(s.cwnd(), 10 * kMtu);
    EXPECT_FALSE(s.ccc().inError());
 }
 TEST(PcmNsccTest, QuickAdaptReducesCwnd) {
     FakeNsccSender s(10 * kMtu);

     // First NACK initializes the quick-adaptation interval.
     s.nack();

     EXPECT_EQ(s.cwnd(), 9 * kMtu);

     // Allow the interval to expire.
     s.advanceTicks(kBaseRtt + 82 + 1);

     // Another NACK triggers quick adaptation.
     // No ACKed bytes were accumulated, so CWND reaches its minimum.
     s.nack();

     EXPECT_EQ(s.cwnd(), kMtu);
     EXPECT_FALSE(s.ccc().inError());
 }
 TEST(PcmNsccTest, QuickAdaptSuppressesSubsequentNacks) {
     FakeNsccSender s(20 * kMtu);

     // Initialize the first interval.
     s.nack();

     // At adaptation time, record outstanding in-flight bytes.
     s.setInflight(4 * kMtu);
     s.advanceTicks(kBaseRtt + 82 + 1);

     // Trigger quick adaptation.
     s.nack();

     EXPECT_EQ(s.cwnd(), kMtu);

     const int64_t after_adapt = s.cwnd();

     // This NACK should be suppressed because the sender has
     // not yet ignored enough bytes to clear the suppression window.
     s.nack();

     EXPECT_EQ(s.cwnd(), after_adapt);
     EXPECT_FALSE(s.ccc().inError());
 }
 TEST(PcmNsccTest, QuickAdaptResetsAfterInterval) {
     FakeNsccSender s(20 * kMtu);

     // Initialize the first interval.
     s.nack();

     // Accumulate ACKed bytes within the interval.
     s.ack(16, false, kBaseRtt + 20);

     // Advance beyond the interval.
     s.advanceTicks(kBaseRtt + 82 + 1);

     // NACK triggers adaptation using the accumulated bytes.
     s.nack();

     EXPECT_EQ(s.cwnd(), kMtu);

     // The achieved-byte count should have been reset.
     // Another interval should not reuse the previous count.
     s.advanceTicks(kBaseRtt + 82 + 1);
     s.nack();

     EXPECT_EQ(s.cwnd(), kMtu);
     EXPECT_FALSE(s.ccc().inError());
 }
 TEST(PcmNsccTest, SuppressionCounterStartsAtZero) {
    FakeNsccSender s(10 * kMtu);

    s.ack(16, false, kBaseRtt + 20);

    // V_BYTES_IGNORED is variable index 11.
    EXPECT_EQ(
        std::get<uint64_t>(s.ccc().variable(11)),
        0u);

    EXPECT_FALSE(s.ccc().inError());
}

TEST(PcmNsccTest, SuppressionCounterIsBounded) {
    FakeNsccSender s(20 * kMtu);

    s.nack();
    s.setInflight(4 * kMtu);

    s.advanceTicks(kBaseRtt + 82 + 1);
    s.nack();

    // V_BYTES_TO_IGNORE is variable index 12.
    const uint64_t threshold =
        std::get<uint64_t>(s.ccc().variable(12));

    EXPECT_EQ(threshold, 4u * kMtu);

    for (int i = 0; i < 10; ++i)
        s.nack();

    const uint64_t ignored =
        std::get<uint64_t>(s.ccc().variable(11));

    EXPECT_LE(ignored, threshold);
    EXPECT_FALSE(s.ccc().inError());
}
