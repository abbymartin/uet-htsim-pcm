// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// DCQCN-inspired congestion control for the PCM/UET simulator.
//
// DCQCN normally adjusts a sending rate. PCM exposes a writable
// congestion window instead, so this implementation approximates
// normalized rate control through CWND.
//
// Congestion response:
//   - ECN-marked ACKs/NACKs act as congestion notifications.
//   - Accepted notifications reduce the normalized rate.
//   - Alpha tracks binary congestion feedback once per interval.
//   - Notifications are rate-limited by P_CNP_INTERVAL.
//
// Recovery:
//   - Fast recovery: midpoint toward the saved target rate.
//   - Active increase: increment target by P_RAI.
//   - Hyperactive increase: increment target by P_RHAI.
//
// Limitations:
//   - No writable pacing-rate control.
//   - A fixed per-connection reference CWND limits recovery.
//   - Binary ECN feedback is not a full DCQCN CNP model.
//   - Unmarked NACKs do not cause rate reduction.
//   - Timing parameters use 128-nanosecond simulator ticks.
//
// This is a DCQCN-inspired CWND approximation, not a
// faithful implementation of hardware DCQCN.

#include "uet_pcm.h"

#include <algorithm>

PCM_ALGORITHM(dcqcn);

PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_ACK_BYTES);
PCM_SIGNAL(S_ACK_ECN);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_NACK_ECN);
PCM_SIGNAL(S_NOW);
PCM_SIGNAL(S_CWND);

PCM_VARIABLE(V_ALPHA);
PCM_VARIABLE(V_RATE);
PCM_VARIABLE(V_TARGET_RATE);
PCM_VARIABLE(V_LAST_CC_UPDATE);
PCM_VARIABLE(V_LAST_ALPHA_UPDATE);
PCM_VARIABLE(V_LAST_CNP);
PCM_VARIABLE(V_RECOVERY_STEPS);
PCM_VARIABLE(V_INITIALIZED);
PCM_VARIABLE(V_CONGESTION_SEEN);
PCM_VARIABLE(V_REFERENCE_CWND);
PCM_VARIABLE(V_HAS_CNP);
PCM_PARAMETER_FLOAT(P_G);
PCM_PARAMETER_UINT(P_UPDATE_PERIOD);
PCM_PARAMETER_UINT(P_CNP_INTERVAL);
PCM_PARAMETER_UINT(P_RECOVERY_THRESHOLD);
PCM_PARAMETER_FLOAT(P_RAI);
PCM_PARAMETER_FLOAT(P_RHAI);
PCM_PARAMETER_FLOAT(P_MIN_RATE);
PCM_PARAMETER_UINT(P_MIN_CWND);
PCM_PARAMETER_UINT(P_REFERENCE_CWND);

PCM_HANDLER(dcqcn) {
    uet_trigmask_t fired = 0;
    uet_trigger_mask(&fired);

    const uet_uint_t now =
        static_cast<uet_uint_t>(
            std::max<uet_int_t>(0, uet_signal_get(S_NOW)));

    uet_float_t alpha = 1.0;
    uet_float_t rate = 1.0;
    uet_float_t target_rate = 1.0;

    uet_uint_t last_cc = 0;
    uet_uint_t last_alpha = 0;
    uet_uint_t last_cnp = 0;
    uet_uint_t recovery_steps = 0;
    uet_uint_t initialized = 0;
    uet_uint_t congestion_seen = 0;
    uet_uint_t reference_cwnd_state = 0;
    uet_uint_t has_cnp = 0;
    uet_get_variable_float(V_ALPHA, &alpha);
    uet_get_variable_float(V_RATE, &rate);
    uet_get_variable_float(V_TARGET_RATE, &target_rate);

    uet_get_variable_uint(V_LAST_CC_UPDATE, &last_cc);
    uet_get_variable_uint(V_LAST_ALPHA_UPDATE, &last_alpha);
    uet_get_variable_uint(V_LAST_CNP, &last_cnp);
    uet_get_variable_uint(V_RECOVERY_STEPS, &recovery_steps);
    uet_get_variable_uint(V_INITIALIZED, &initialized);
    uet_get_variable_uint(V_CONGESTION_SEEN, &congestion_seen);
    uet_get_variable_uint(V_REFERENCE_CWND, &reference_cwnd_state);
    uet_get_variable_uint(V_HAS_CNP, &has_cnp);
    const double min_rate =
        std::max(0.001, double(P_MIN_RATE));

    const uet_uint_t update_period =
        std::max<uet_uint_t>(1, P_UPDATE_PERIOD);

    const uet_uint_t cnp_interval =
        std::max<uet_uint_t>(1, P_CNP_INTERVAL);

    if (!initialized) {
        alpha = 1.0;
        rate = 1.0;
        target_rate = 1.0;
        last_cc = now;
        last_alpha = now;
        last_cnp = 0;
        has_cnp = 0;
        recovery_steps = 0;
        congestion_seen = 0;

        // Preserve the simulator-provided initial CWND.
        const auto initial_cwnd = uet_signal_get(S_CWND);

        reference_cwnd_state =
            initial_cwnd > 0
                ? static_cast<uet_uint_t>(initial_cwnd)
                : P_REFERENCE_CWND;

        reference_cwnd_state = std::max<uet_uint_t>(
            P_MIN_CWND,
            std::min<uet_uint_t>(
                P_REFERENCE_CWND, reference_cwnd_state));

        initialized = 1;
    }

    const bool ack =
        (fired & T_ACK) != 0;

    const bool nack =
        (fired & T_NACK) != 0;

    const bool ack_ecn =
        ack && uet_signal_get(S_ACK_ECN) != 0;

    const bool nack_ecn =
        nack && uet_signal_get(S_NACK_ECN) != 0;

    const bool congestion_feedback = ack_ecn || nack_ecn;

    const bool cnp_due =
        !has_cnp ||
        (now >= last_cnp &&
        now - last_cnp >= cnp_interval);

    // Periodic alpha update.
    // Runs independently of whether congestion feedback arrives.
    // Record ECN feedback during the current alpha interval.
    if (congestion_feedback)
        congestion_seen = 1;

    // Update alpha once per interval using binary congestion feedback.
    if (now >= last_alpha &&
        now - last_alpha >= update_period) {

        const double feedback = congestion_seen ? 1.0 : 0.0;

        alpha = (1.0 - P_G) * alpha + P_G * feedback;
        alpha = std::clamp(double(alpha), 0.0, 1.0);

        congestion_seen = 0;
        last_alpha = now;
    }

    // Periodic rate recovery.
    // Congestion feedback must not suppress the recovery timer.
    // Periodic rate recovery.
    // Congestion feedback does not suppress the recovery timer.
        // Periodic rate recovery.
    // Fast recovery -> active increase -> hyperactive increase.
    // Recovery is timer-driven, independent of ACK arrival.
    if (now >= last_cc &&
        now - last_cc >= update_period) {

        recovery_steps++;

        const uet_uint_t threshold =
            std::max<uet_uint_t>(1, P_RECOVERY_THRESHOLD);

        if (recovery_steps <= threshold) {
            // Stage 1: Fast recovery.
            // Move halfway toward the saved target rate.
            rate = (target_rate + rate) / 2.0;

        } else if (recovery_steps - threshold <= threshold) {
            // Stage 2: Active increase.
            // Increase target rate by the smaller increment.
            target_rate = std::min(
                1.0, double(target_rate) + double(P_RAI));

            rate = (target_rate + rate) / 2.0;

        } else {
            // Stage 3: Hyperactive increase.
            // Increase target rate by the larger increment.
            target_rate = std::min(
                1.0, double(target_rate) + double(P_RHAI));

            rate = (target_rate + rate) / 2.0;
        }

        last_cc = now;
    }

    // ECN/NACK congestion feedback.
    // This can occur independently of the periodic updates.
    if (congestion_feedback && cnp_due) {
        target_rate = rate;
        rate *= (1.0 - alpha / 2.0);
        rate = std::max(min_rate, rate);

        last_cnp = now;
        has_cnp = 1;
        recovery_steps = 0;
    }

    rate = std::min(1.0, std::max(min_rate, double(rate)));
    target_rate = std::min(1.0, std::max(min_rate, double(target_rate)));

    // PCM currently exposes CWND control, not pacing-rate control.
    // Translate normalized rate to a topology-specific CWND target.
    const double min_cwnd = double(P_MIN_CWND);

    const double reference_cwnd = std::max(
        min_cwnd, double(reference_cwnd_state));

    double cwnd = reference_cwnd * double(rate);

    cwnd = std::clamp(cwnd, min_cwnd, reference_cwnd);

    uet_set_variable_float(V_ALPHA, alpha);
    uet_set_variable_float(V_RATE, rate);
    uet_set_variable_float(V_TARGET_RATE, target_rate);

    uet_set_variable_uint(V_LAST_CC_UPDATE, last_cc);
    uet_set_variable_uint(V_LAST_ALPHA_UPDATE, last_alpha);
    uet_set_variable_uint(V_LAST_CNP, last_cnp);
    uet_set_variable_uint(V_RECOVERY_STEPS, recovery_steps);
    uet_set_variable_uint(V_INITIALIZED, initialized);
    uet_set_variable_uint(V_CONGESTION_SEEN, congestion_seen);
    uet_set_variable_uint(V_REFERENCE_CWND, reference_cwnd_state);
    uet_set_variable_uint(V_HAS_CNP, has_cnp);

    uet_set_cwnd(static_cast<unsigned int>(cwnd));
}
