// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
// PCM NSCC: closer approximation to UecSrc's built-in sender-side NSCC.
// Not exact: PCM lacks an ACK RTT-valid bit and this handler does not model
// quick-adaptation intervals or time-triggered fulfill/eta adjustments.
#include "uet_pcm.h"
#include <algorithm>
#include <limits>
#include <cstdint>
#include <iostream>
PCM_ALGORITHM(nscc);
PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_ACK_BYTES);
PCM_SIGNAL(S_ACK_ECN);
PCM_SIGNAL(S_ACK_RTT);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_NACK_BYTES);
PCM_SIGNAL(S_NACK_RTT);
PCM_SIGNAL(S_NACK_LAST_HOP);
PCM_SIGNAL(S_CWND);
PCM_SIGNAL(S_NOW);
PCM_SIGNAL(S_INFLIGHT);
PCM_VARIABLE(V_BASE_RTT);
PCM_VARIABLE(V_AVG_DELAY);
PCM_VARIABLE(V_INC_BYTES);
PCM_VARIABLE(V_RECEIVED_BYTES);
PCM_VARIABLE(V_FI_COUNT);
PCM_VARIABLE(V_FAST_MODE);
PCM_VARIABLE(V_LAST_DEC_TIME);
PCM_VARIABLE(V_HAS_DECREASED);
PCM_VARIABLE(V_QA_ENDTIME);
PCM_VARIABLE(V_ACHIEVED_BYTES);
PCM_VARIABLE(V_TRIGGER_QA);
PCM_VARIABLE(V_BYTES_IGNORED);
PCM_VARIABLE(V_BYTES_TO_IGNORE);
PCM_VARIABLE(V_LAST_ADJUST_TIME);
PCM_PARAMETER_UINT(P_MTU);
PCM_PARAMETER_UINT(P_MSS);
PCM_PARAMETER_UINT(P_TARGET_QDELAY);
PCM_PARAMETER_UINT(P_INITIAL_BASE_RTT);
PCM_PARAMETER_UINT(P_NETWORK_RTT);
PCM_PARAMETER_UINT(P_MAX_CWND);
PCM_PARAMETER_UINT(P_ADJUST_BYTES);
PCM_PARAMETER_UINT(P_FAST_DELAY);
PCM_PARAMETER_UINT(P_QA_GATE);
PCM_PARAMETER_UINT(P_QA_THRESHOLD);
PCM_PARAMETER_FLOAT(P_DELAY_ALPHA);
PCM_PARAMETER_FLOAT(P_GAMMA);
PCM_PARAMETER_FLOAT(P_FAIR_GAIN);
PCM_PARAMETER_FLOAT(P_PROP_GAIN);
PCM_PARAMETER_FLOAT(P_FAST_SCALE);
PCM_PARAMETER_FLOAT(P_ETA);
PCM_PARAMETER_UINT(P_ADJUST_PERIOD);
PCM_HANDLER(nscc) {
    uet_trigmask_t fired = 0;
    uet_trigger_mask(&fired);

    const double min_cwnd = double(P_MTU);
    const double max_cwnd = std::max(min_cwnd, double(P_MAX_CWND));
    const double target = std::max(1.0, double(P_TARGET_QDELAY));
    double cwnd = std::max(min_cwnd, double(uet_signal_get(S_CWND)));

    uet_uint_t base_rtt = 0, received = 0, fi_count = 0, fast_mode = 0;
    uet_uint_t last_dec_time = 0, has_decreased = 0;
    uet_uint_t qa_endtime = 0;
    uet_uint_t achieved_bytes = 0;
    uet_uint_t trigger_qa = 0;
    uet_uint_t bytes_ignored = 0;
    uet_uint_t bytes_to_ignore = 0;
    uet_float_t avg_delay = 0.0, inc_bytes = 0.0;
    uet_uint_t last_adjust_time = 0;
    uet_get_variable_uint(V_LAST_ADJUST_TIME, &last_adjust_time);
    uet_get_variable_uint(V_BASE_RTT, &base_rtt);
    uet_get_variable_uint(V_RECEIVED_BYTES, &received);
    uet_get_variable_uint(V_FI_COUNT, &fi_count);
    uet_get_variable_uint(V_FAST_MODE, &fast_mode);
    uet_get_variable_uint(V_LAST_DEC_TIME, &last_dec_time);
    uet_get_variable_uint(V_HAS_DECREASED, &has_decreased);
    uet_get_variable_float(V_AVG_DELAY, &avg_delay);
    uet_get_variable_float(V_INC_BYTES, &inc_bytes);
    uet_get_variable_uint(V_QA_ENDTIME, &qa_endtime);
    uet_get_variable_uint(V_ACHIEVED_BYTES, &achieved_bytes);
    uet_get_variable_uint(V_TRIGGER_QA, &trigger_qa);
    uet_get_variable_uint(V_BYTES_IGNORED, &bytes_ignored);
    uet_get_variable_uint(V_BYTES_TO_IGNORE, &bytes_to_ignore);
    if (base_rtt == 0)
        base_rtt = P_INITIAL_BASE_RTT;
    const uet_uint_t now = static_cast<uet_uint_t>(
        std::max<uet_int_t>(0, uet_signal_get(S_NOW)));
    auto count_ignored_bytes = [&](uet_uint_t bytes) {
        if (bytes_ignored < bytes_to_ignore) {
            const uet_uint_t remaining =
                bytes_to_ignore - bytes_ignored;

            bytes_ignored += std::min(bytes, remaining);
        }
    };
        // Saturating addition for unsigned byte counters.
    auto add_bytes_saturating = [](uet_uint_t current,
                                   uet_uint_t bytes) -> uet_uint_t {
        const uet_uint_t maximum =
            std::numeric_limits<uet_uint_t>::max();

        if (bytes > maximum - current)
            return maximum;

        return current + bytes;
    };
    auto quick_adapt = [&](bool is_loss, bool skip, double delay) {
        bool qa_done_or_ignore = false;

        if (bytes_ignored < bytes_to_ignore && skip) {
            static unsigned long debug_ignored_events = 0;
            ++debug_ignored_events;

            if (debug_ignored_events % 5000 == 0) {
                std::cerr
                    << "NSCC_QA_IGNORED"
                    << " count=" << debug_ignored_events
                    << " bytes_ignored=" << bytes_ignored
                    << " bytes_to_ignore=" << bytes_to_ignore
                    << std::endl;
            }
            qa_done_or_ignore = true;
        } else if (now > qa_endtime) {
            const bool congestion =
                trigger_qa != 0 ||
                is_loss ||
                delay > double(P_QA_THRESHOLD);

            const uet_uint_t threshold =
                uet_uint_t(max_cwnd) >> P_QA_GATE;

            if (qa_endtime != 0 &&
                congestion &&
                achieved_bytes < threshold) {
                static unsigned long debug_qa_reductions = 0;
                ++debug_qa_reductions;

                if (debug_qa_reductions % 100 == 0) {
                    std::cerr
                        << "NSCC_QA_REDUCTION"
                        << " count=" << debug_qa_reductions
                        << " achieved_bytes=" << achieved_bytes
                        << " threshold=" << threshold
                        << " cwnd_before=" << cwnd
                        << std::endl;
                }
                cwnd = std::max(
                    min_cwnd,
                    double(achieved_bytes)
                );

                bytes_to_ignore = static_cast<uet_uint_t>(
                    std::max<uet_int_t>(
                        0, uet_signal_get(S_INFLIGHT)
                    )
                );

                bytes_ignored = 0;
                trigger_qa = 0;
                qa_done_or_ignore = true;
            }

            achieved_bytes = 0;
            qa_endtime = now + base_rtt + P_TARGET_QDELAY;
        }

        if (qa_done_or_ignore) {
            static unsigned long debug_qa_resets = 0;
            static unsigned long debug_qa_nonzero_resets = 0;

            ++debug_qa_resets;

            if (inc_bytes > 0.0) {
                ++debug_qa_nonzero_resets;
            }

            if (debug_qa_resets % 1000 == 0) {
                std::cerr
                    << "NSCC_QA_STATS"
                    << " resets=" << debug_qa_resets
                    << " nonzero_resets=" << debug_qa_nonzero_resets
                    << " inc_bytes_before_reset=" << inc_bytes
                    << " received_before_reset=" << received
                    << std::endl;
            }

            inc_bytes = 0.0;
            received = 0;
        }

        return qa_done_or_ignore;
    };

    if (fired & T_ACK) {
        const double acked = std::max(0.0, double(uet_signal_get(S_ACK_BYTES)));
        const bool ecn = uet_signal_get(S_ACK_ECN) != 0;
        const uet_int_t sample = uet_signal_get(S_ACK_RTT);
        // The LAST signal can be stale when this ACK had no valid RTT sample.
        // Repeated equal samples are legitimate, so do not compare to last RTT.
        // Exact parity requires an explicit RTT-valid signal from the sender.
        if (sample > 0) {
            const uet_uint_t raw_rtt = uet_uint_t(sample);
            if (raw_rtt < base_rtt)
                base_rtt = raw_rtt;
            const double delay_sample = double(raw_rtt - base_rtt);
            const double observed = (!ecn && delay_sample > target)
                ? double(base_rtt) * 0.25 : delay_sample;
            const double weight = delay_sample > 5.0 * double(base_rtt)
                ? 0.0125 : double(P_DELAY_ALPHA);
            avg_delay = weight * observed + (1.0 - weight) * avg_delay;
        }

        const double delay = sample > 0
            ? std::max(0.0, double(sample) - double(base_rtt))
            : avg_delay;
        // ACK byte counts are reported in 256-byte units.
        // Saturate before multiplication to prevent overflow.
        const uet_uint_t max_units =
            std::numeric_limits<uet_uint_t>::max() / 256;

        const uet_uint_t ack_units =
            static_cast<uet_uint_t>(
                std::min(acked, double(max_units)));

        const uet_uint_t acked_bytes = ack_units * 256;

        received = add_bytes_saturating(received, acked_bytes);

        achieved_bytes =
            add_bytes_saturating(achieved_bytes, acked_bytes);

        count_ignored_bytes(acked_bytes);
        static int debug_ack_count = 0;

        if (debug_ack_count < 10) {
            std::cerr
                << "NSCC_DEBUG_ACK"
                << " acked_units=" << acked
                << " acked_bytes=" << acked_bytes
                << " cwnd=" << cwnd
                << " now=" << now
                << std::endl;

            ++debug_ack_count;
        }
        if (!quick_adapt(false, ecn, delay)) {
            if (!ecn && delay >= target) {
                // Built-in fair_increase: inc_bytes += fi * newly_acked_bytes.
                inc_bytes += P_FAIR_GAIN * double(acked_bytes);
            } else if (!ecn) {
                // Built-in fast_increase precedes proportional_increase.
                if (delay < double(P_FAST_DELAY)) {
                    fi_count += acked_bytes;
                    if (double(fi_count) > cwnd || fast_mode != 0) {
                        cwnd += double(acked_bytes) * P_FAST_SCALE;
                        fast_mode = 1;
                    } else {
                        fast_mode = 0;
                        inc_bytes += P_PROP_GAIN * double(acked_bytes) * (target - delay);
                    }
                } else {
                    fi_count = 0;
                    fast_mode = 0;
                    inc_bytes += P_PROP_GAIN * double(acked_bytes) * (target - delay);
                }
            } else if (delay >= target) {
                fast_mode = 0;
                fi_count = 0;
                // Approximation: built-in multiplicative decrease is gated
                // to once per base RTT; PCM version needs a time signal/state.
                // Built-in NSCC allows a multiplicative decrease only after
                // more than one base RTT has elapsed since the previous decrease.
                const bool decrease_due =
                    (has_decreased == 0)
                        ? (now > base_rtt)
                        : (now >= last_dec_time &&
                        now - last_dec_time > base_rtt);

                if (avg_delay > target && decrease_due) {
                    const double factor = std::max(
                        0.5,
                        1.0 - double(P_GAMMA) *
                                (avg_delay - target) / avg_delay);

                    cwnd *= factor;
                    last_dec_time = now;
                    has_decreased = 1;
                }
            }

            cwnd = std::max(min_cwnd, std::min(cwnd, max_cwnd));
            const bool time_due =
                now >= last_adjust_time &&
                now - last_adjust_time > P_ADJUST_PERIOD;
            if (received > P_ADJUST_BYTES || time_due) {
                static unsigned long debug_byte_adjustments = 0;
                static unsigned long debug_time_adjustments = 0;
                static unsigned long debug_nonzero_increases = 0;

                if (received > P_ADJUST_BYTES)
                    ++debug_byte_adjustments;

                if (time_due)
                    ++debug_time_adjustments;

                if (inc_bytes > 0.0)
                    ++debug_nonzero_increases;

                static unsigned long debug_total_adjustments = 0;
                ++debug_total_adjustments;

                if (debug_total_adjustments % 1000 == 0) {
                    std::cerr
                        << "NSCC_ADJUST_STATS"
                        << " total=" << debug_total_adjustments
                        << " byte=" << debug_byte_adjustments
                        << " time=" << debug_time_adjustments
                        << " nonzero=" << debug_nonzero_increases
                        << std::endl;
                }
                static int debug_fulfill_count = 0;
                const double before = cwnd;
                const double pending_increase = inc_bytes / cwnd;
                const uet_uint_t received_before = received;

                cwnd += pending_increase;

                const bool eta_due =
                    now >= last_adjust_time &&
                    now - last_adjust_time >= P_ADJUST_PERIOD;

                if (eta_due) {
                    cwnd += P_ETA;
                    last_adjust_time = now;
                }

                if (debug_fulfill_count < 20) {
                    std::cerr
                        << "NSCC_DEBUG_FULFILL"
                        << " now=" << now
                        << " received=" << received_before
                        << " byte_due=" << (received_before > P_ADJUST_BYTES)
                        << " time_due=" << time_due
                        << " eta_due=" << eta_due
                        << " cwnd_before=" << before
                        << " pending_increase=" << pending_increase
                        << " cwnd_after=" << cwnd
                        << std::endl;

                    ++debug_fulfill_count;
                }

                received = 0;
                inc_bytes = 0.0;
            }

        }
    }
    if (fired & T_NACK) {
        const double nacked =
            std::max(0.0, double(uet_signal_get(S_NACK_BYTES)));

        const uet_uint_t nacked_bytes =
            static_cast<uet_uint_t>(nacked);

        count_ignored_bytes(nacked_bytes);

        // Built-in NACK handling injects a synthetic delay sample.
        const double observed = double(P_NETWORK_RTT);
        const double weight =
            observed > 5.0 * double(base_rtt)
                ? 0.0125
                : double(P_DELAY_ALPHA);

        avg_delay =
            weight * observed +
            (1.0 - weight) * avg_delay;

        trigger_qa = 1;

        // Quick adaptation takes precedence over ordinary NACK reduction.
        if (!quick_adapt(true, true, 0.0)) {
            cwnd -= nacked;
        }

        (void)uet_signal_get(S_NACK_LAST_HOP);
        (void)uet_signal_get(S_NACK_RTT);
    }
    static int debug_final_count = 0;

    if (debug_final_count < 20) {
        std::cerr
            << "NSCC_DEBUG_FINAL"
            << " cwnd_before_bounds=" << cwnd
            << " max_cwnd=" << max_cwnd
            << " inc_bytes=" << inc_bytes
            << " received=" << received
            << " now=" << now
            << std::endl;

        ++debug_final_count;
    }
    cwnd = std::max(min_cwnd, std::min(cwnd, max_cwnd));
    cwnd = std::min(cwnd, double(std::numeric_limits<unsigned int>::max()));
    uet_set_variable_uint(V_BASE_RTT, base_rtt);
    uet_set_variable_uint(V_RECEIVED_BYTES, received);
    uet_set_variable_uint(V_FI_COUNT, fi_count);
    uet_set_variable_uint(V_FAST_MODE, fast_mode);
    uet_set_variable_uint(V_LAST_DEC_TIME, last_dec_time);
    uet_set_variable_uint(V_HAS_DECREASED, has_decreased);
    uet_set_variable_float(V_AVG_DELAY, avg_delay);
    uet_set_variable_float(V_INC_BYTES, inc_bytes);
    uet_set_variable_uint(V_QA_ENDTIME, qa_endtime);
    uet_set_variable_uint(V_ACHIEVED_BYTES, achieved_bytes);
    uet_set_variable_uint(V_TRIGGER_QA, trigger_qa);
    uet_set_variable_uint(V_BYTES_IGNORED, bytes_ignored);
    uet_set_variable_uint(V_BYTES_TO_IGNORE, bytes_to_ignore);
    uet_set_variable_uint(V_LAST_ADJUST_TIME, last_adjust_time);
    uet_set_cwnd(static_cast<unsigned int>(cwnd));
}
