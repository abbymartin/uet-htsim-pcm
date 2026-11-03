// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
// Time triggers, invoke_deadline, and the time signals.
#include "pcm_runtime.h"

#include <algorithm>
#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "uet_pcm.h"

namespace timed {
PCM_ALGORITHM(timed);
PCM_SIGNAL(T_TIME);
PCM_SIGNAL(S_LAST_UPDATE);
PCM_SIGNAL(T_ACK);
PCM_SIGNAL(T_NACK);

std::function<void()> body;

PCM_HANDLER(timed) {
    if (body)
        body();
}
}  // namespace timed

using namespace timed;

namespace {

constexpr PcmTime kUnit = kPcmTimeUnitPs;  // 128 ns

// Entries: T_TIME (0) fires every 10 units; S_LAST_UPDATE (1); T_ACK (2) calls
// the handler on every ACK; T_NACK (3) on every NACK, deferred up to 4 units.
std::string yaml(const std::string& time_arm_mode = "continuous") {
    return R"(
cc_algorithm:
  name: timed
signal:
  - name: uet_time_current
    macro: T_TIME
    accumulator: uet_op_last
    trigger: {op: wc, mode_refval: 0 10, arm_mode: )" + time_arm_mode + R"(, action: invoke,
              invoke_deadline: 0}
  - name: uet_time_last_update
    macro: S_LAST_UPDATE
    accumulator: uet_op_last
  - name: uet_rx_ack
    macro: T_ACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode_refval: 1 0, arm_mode: continuous, action: invoke,
              invoke_deadline: 0}
  - name: uet_rx_nack
    macro: T_NACK
    accumulator: uet_op_sum
    trigger: {op: wc, mode_refval: 1 0, arm_mode: continuous, action: invoke,
              invoke_deadline: 4}
control:
  - {name: uet_tx_cwnd, initial_value: 1000}
)";
}

const PcmSignalInfo* sig(const char* name) { return pcmFindSignal(name); }
std::vector<PcmSignalUpdate> ack() { return {{sig("uet_rx_ack"), 1}}; }
std::vector<PcmSignalUpdate> nack() { return {{sig("uet_rx_nack"), 1}}; }

// Stands in for the simulator's event list: records the wake-ups a CCC asks
// for, and delivers them in time order.
class FakeClock {
public:
    PcmCcc::Scheduler scheduler() {
        return [this](PcmTime when) { requests.push_back(when); };
    }
    // Deliver every requested wake-up up to and including `until`.
    void runUntil(PcmCcc& ccc, PcmTime until) {
        while (true) {
            auto it = std::min_element(requests.begin(), requests.end());
            if (it == requests.end() || *it > until)
                break;
            PcmTime t = *it;
            requests.erase(it);
            woken_at.push_back(t);
            ccc.onTimer(t);
        }
    }
    std::vector<PcmTime> requests;
    std::vector<PcmTime> woken_at;
};

class PcmTimerTest : public ::testing::Test {
protected:
    PcmTimerTest() {
        PcmCcc::print_errors = false;
        timed::body = nullptr;
    }
    ~PcmTimerTest() override { timed::body = nullptr; }

    std::shared_ptr<const PcmAlgorithm> algo(const std::string& arm = "continuous") {
        return std::make_shared<const PcmAlgorithm>(pcmParseSpec(yaml(arm), "timed.yaml"));
    }
};

// Records the trigger mask and time signals at each handler call.
struct Call {
    uet_trigmask_t mask;
    int64_t time;
    int64_t last_update;
};

std::function<void()> recordCalls(std::vector<Call>& calls) {
    return [&calls] {
        Call c{};
        uet_trigger_mask(&c.mask);
        c.time = uet_signal_get(T_TIME);
        c.last_update = uet_signal_get(S_LAST_UPDATE);
        calls.push_back(c);
    };
}

}  // namespace

TEST(PcmUnitsTest, Conversion) {
    EXPECT_EQ(pcmToSignalUnits(PcmUnit::TIME_128NS, 10 * kUnit + 5, PcmUnits::SPEC), 10);
    EXPECT_EQ(pcmToSignalUnits(PcmUnit::TIME_128NS, 10 * kUnit + 5, PcmUnits::NATIVE),
              int64_t(10 * kUnit + 5));
    EXPECT_EQ(pcmToSignalUnits(PcmUnit::BYTES_256, 4150, PcmUnits::SPEC), 16);  // truncated
    EXPECT_EQ(pcmToSignalUnits(PcmUnit::BYTES_256, 4150, PcmUnits::NATIVE), 4150);
    EXPECT_EQ(pcmToSignalUnits(PcmUnit::BYTES, 4150, PcmUnits::SPEC), 4150);
    EXPECT_EQ(pcmToSignalUnits(PcmUnit::COUNT, 3, PcmUnits::SPEC), 3);
}

TEST_F(PcmTimerTest, TimeTriggerFiresOnSchedule) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    std::vector<Call> calls;
    timed::body = recordCalls(calls);

    ccc.setScheduler(clock.scheduler(), 0);
    EXPECT_EQ(clock.requests, (std::vector<PcmTime>{10 * kUnit}));

    clock.runUntil(ccc, 35 * kUnit);
    EXPECT_EQ(clock.woken_at, (std::vector<PcmTime>{10 * kUnit, 20 * kUnit, 30 * kUnit}));
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].time, 10);  // in 128 ns units
    EXPECT_EQ(calls[1].time, 20);
    EXPECT_EQ(calls[2].time, 30);
    EXPECT_EQ(calls[0].mask, PcmSignalState::bit(0));
    EXPECT_EQ(ccc.counters().timer_events, 3u);
    EXPECT_EQ(ccc.nextWakeup().value(), 40 * kUnit);
}

TEST_F(PcmTimerTest, OnceTimeTriggerFiresOnce) {
    PcmCcc ccc(algo("once"), 0);
    FakeClock clock;
    ccc.setScheduler(clock.scheduler(), 0);
    clock.runUntil(ccc, 100 * kUnit);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_FALSE(ccc.nextWakeup().has_value());
    EXPECT_TRUE(clock.requests.empty());
}

// Handlers called for an ACK read the current time, not the last timer event's.
TEST_F(PcmTimerTest, HandlersReadTheCurrentTime) {
    PcmCcc ccc(algo(), 0);
    std::vector<Call> calls;
    timed::body = recordCalls(calls);

    ccc.onEvent(ack(), 3 * kUnit + 500);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].mask, PcmSignalState::bit(2));
    EXPECT_EQ(calls[0].time, 3);         // truncated to whole 128 ns units
    EXPECT_EQ(calls[0].last_update, 3);  // the ACK itself
}

TEST_F(PcmTimerTest, LastUpdateIsTheLastSignalUpdate) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    std::vector<Call> calls;
    timed::body = recordCalls(calls);
    ccc.setScheduler(clock.scheduler(), 0);

    ccc.onEvent(ack(), 2 * kUnit);   // call 1
    clock.runUntil(ccc, 10 * kUnit);  // call 2, a timer event
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].time, 10);
    EXPECT_EQ(calls[1].last_update, 2);  // the ACK, not the timer event
}

TEST_F(PcmTimerTest, NativeUnits) {
    PcmCcc ccc(algo(), 0, "native", PcmUnits::NATIVE);
    std::vector<Call> calls;
    timed::body = recordCalls(calls);
    ccc.onEvent(ack(), 3 * kUnit + 500);
    EXPECT_EQ(calls[0].time, int64_t(3 * kUnit + 500));
}

// A NACK's trigger may be deferred 4 units: the handler is called at the
// deadline, not straight away.
TEST_F(PcmTimerTest, DeadlineCall) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    std::vector<Call> calls;
    timed::body = recordCalls(calls);
    ccc.setScheduler(clock.scheduler(), 0);

    ccc.onEvent(nack(), 1 * kUnit);
    EXPECT_TRUE(calls.empty());
    EXPECT_EQ(ccc.nextWakeup().value(), 5 * kUnit);

    clock.runUntil(ccc, 6 * kUnit);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].mask, PcmSignalState::bit(3));
    EXPECT_EQ(calls[0].time, 5);
    EXPECT_EQ(ccc.counters().deadline_invocations, 1u);
    EXPECT_FALSE(ccc.invokeDeadline().has_value());
}

// Triggers firing before the deadline are combined into the one call.
TEST_F(PcmTimerTest, DeadlineCombinesTriggers) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    std::vector<Call> calls;
    timed::body = recordCalls(calls);
    ccc.setScheduler(clock.scheduler(), 0);

    ccc.onEvent(nack(), 1 * kUnit);  // deadline 5
    ccc.onEvent(nack(), 2 * kUnit);  // deadline 6; the earlier one stands
    EXPECT_EQ(ccc.invokeDeadline().value(), 5 * kUnit);
    clock.runUntil(ccc, 9 * kUnit);
    EXPECT_EQ(calls.size(), 1u);
    EXPECT_EQ(ccc.signals().get(3), 2);  // both NACKs counted
}

// An immediate call before the deadline delivers the deferred trigger too, and
// the wake-up already requested for the deadline does nothing.
TEST_F(PcmTimerTest, ImmediateCallBeforeDeadline) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    std::vector<Call> calls;
    timed::body = recordCalls(calls);
    ccc.setScheduler(clock.scheduler(), 0);

    ccc.onEvent(nack(), 1 * kUnit);
    ccc.onEvent(ack(), 2 * kUnit);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].mask, PcmSignalState::bit(2) | PcmSignalState::bit(3));

    clock.runUntil(ccc, 9 * kUnit);  // includes the stale wake-up at 5
    EXPECT_EQ(calls.size(), 1u);
    EXPECT_EQ(ccc.counters().deadline_invocations, 0u);
    EXPECT_EQ(ccc.nextWakeup().value(), 10 * kUnit);
}

// A wake-up needed earlier than the one already requested is requested too;
// a later one isn't.
TEST_F(PcmTimerTest, OnlyEarlierWakeupsAreRequested) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    ccc.setScheduler(clock.scheduler(), 0);
    EXPECT_EQ(clock.requests, (std::vector<PcmTime>{10 * kUnit}));

    ccc.onEvent(nack(), 1 * kUnit);  // deadline at 5: earlier
    EXPECT_EQ(clock.requests, (std::vector<PcmTime>{10 * kUnit, 5 * kUnit}));

    ccc.onEvent(ack(), 2 * kUnit);  // nothing new needed
    EXPECT_EQ(clock.requests.size(), 2u);
}

TEST_F(PcmTimerTest, NoTimersWithoutScheduler) {
    PcmCcc ccc(algo(), 0);
    ccc.onEvent(nack(), 1 * kUnit);
    EXPECT_EQ(ccc.nextWakeup().value(), 5 * kUnit);
    ccc.onTimer(5 * kUnit);  // the datapath may still call onTimer itself
    EXPECT_EQ(ccc.counters().invocations, 1u);
}

TEST_F(PcmTimerTest, NoTimersInErrorState) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    timed::body = [] { uet_ccc_fatal(1); };
    ccc.setScheduler(clock.scheduler(), 0);
    ccc.onEvent(ack(), 1 * kUnit);
    ASSERT_TRUE(ccc.inError());
    EXPECT_FALSE(ccc.nextWakeup().has_value());
    clock.runUntil(ccc, 100 * kUnit);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_EQ(ccc.counters().timer_events, 0u);
}

// --- Asynchronous mode (a handler time) -----------------------------------------

// The handler's window takes effect a handler time after the call starts.
TEST_F(PcmTimerTest, AsyncChangesTakeEffectAfterHandlerTime) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    ccc.setHandlerTime(2 * kUnit);
    ccc.setScheduler(clock.scheduler(), 0);
    timed::body = [] { uet_set_cwnd(2000); };

    ccc.onEvent(ack(), 1 * kUnit);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_TRUE(ccc.busy());
    EXPECT_EQ(ccc.cwnd().value(), 1000u);  // not yet
    EXPECT_EQ(ccc.nextWakeup().value(), 3 * kUnit);

    clock.runUntil(ccc, 3 * kUnit);
    EXPECT_FALSE(ccc.busy());
    EXPECT_EQ(ccc.cwnd().value(), 2000u);
    EXPECT_EQ(ccc.counters().cwnd_updates, 1u);
}

// Events during a call still update signals; the calls they ask for are
// combined into one, which starts when the busy call finishes and reads the
// signals as they are then.
TEST_F(PcmTimerTest, AsyncCombinesRequestsWhileBusy) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    ccc.setHandlerTime(2 * kUnit);
    ccc.setScheduler(clock.scheduler(), 0);
    std::vector<int64_t> acks_seen;
    std::vector<int64_t> time_seen;
    timed::body = [&] {
        acks_seen.push_back(uet_signal_get(T_ACK));
        time_seen.push_back(uet_signal_get(T_TIME));
    };

    ccc.onEvent(ack(), 1 * kUnit);            // call 1 starts; busy until 3
    ccc.onEvent(ack(), 1 * kUnit + kUnit / 2);
    ccc.onEvent(ack(), 2 * kUnit);
    EXPECT_EQ(ccc.counters().invocations, 1u);
    EXPECT_EQ(ccc.counters().requests_while_busy, 2u);
    EXPECT_EQ(ccc.signals().get(2), 3);       // all three ACKs counted

    clock.runUntil(ccc, 6 * kUnit);           // call 1 ends at 3, call 2 runs 3..5
    EXPECT_EQ(ccc.counters().invocations, 2u);
    EXPECT_EQ(acks_seen, (std::vector<int64_t>{1, 3}));
    EXPECT_EQ(time_seen, (std::vector<int64_t>{1, 3}));
    EXPECT_FALSE(ccc.busy());
}

// A deadline reached while busy is served by the call after.
TEST_F(PcmTimerTest, AsyncDeadlineWhileBusy) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    ccc.setHandlerTime(6 * kUnit);
    ccc.setScheduler(clock.scheduler(), 0);
    std::vector<uet_trigmask_t> masks;
    timed::body = [&] {
        uet_trigmask_t m;
        uet_trigger_mask(&m);
        masks.push_back(m);
    };

    ccc.onEvent(ack(), 1 * kUnit);   // call 1, busy until 7
    ccc.onEvent(nack(), 2 * kUnit);  // deadline at 6, while busy
    clock.runUntil(ccc, 9 * kUnit);
    ASSERT_EQ(masks.size(), 2u);
    EXPECT_EQ(masks[1], PcmSignalState::bit(3));  // T_NACK, delivered by call 2 at 7
    EXPECT_EQ(ccc.counters().deadline_invocations, 0u);
}

TEST_F(PcmTimerTest, AsyncFatalChangesNothing) {
    PcmCcc ccc(algo(), 0);
    FakeClock clock;
    ccc.setHandlerTime(2 * kUnit);
    ccc.setScheduler(clock.scheduler(), 0);
    timed::body = [] {
        uet_set_cwnd(5000);
        uet_ccc_fatal(3);
    };
    ccc.onEvent(ack(), 1 * kUnit);
    EXPECT_TRUE(ccc.inError());
    EXPECT_FALSE(ccc.busy());
    clock.runUntil(ccc, 10 * kUnit);
    EXPECT_EQ(ccc.cwnd().value(), 1000u);
}
