
// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// BBRv1-inspired congestion control for the PCM/UET simulator.
//
// This is a CWND-only approximation, not a full implementation
// of BBRv1.
//
// The controller estimates:
//   - Bottleneck bandwidth from ACKed bytes over elapsed time.
//   - Minimum RTT from ACK RTT observations.
//   - BDP = estimated bandwidth * minimum RTT.
//
// Operating modes:
//   STARTUP:   Increase CWND while discovering available bandwidth.
//   DRAIN:     Reduce excess inflight data after STARTUP.
//   PROBE_BW:  Explore bandwidth using an eight-phase CWND cycle.
//   PROBE_RTT: Temporarily reduce CWND to refresh minimum RTT.
//
// Simulator-specific adaptations:
//   - PCM has no writable pacing-rate control. The PROBE_BW
//     gain cycle therefore modifies CWND rather than pacing.
//   - NACKs indicate negative congestion feedback and cause
//     multiplicative CWND reduction.
//   - ACK bandwidth sampling is a simplified approximation,
//     not BBR's packet-level delivery-rate estimator.
//   - ACK byte signals use 256-byte units.
//   - RTT and time signals use 128-nanosecond ticks.
//   - The simulator's positive initial CWND is preserved;
//     P_INITIAL_CWND is only a fallback.
//
// The implementation supports the PCM spec-unit configuration
// used by the project's simulations. Native-unit behavior
// requires separate validation.

#include "uet_pcm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

PCM_ALGORITHM(bbr);

PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_ACK_BYTES);
PCM_SIGNAL(S_ACK_RTT);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_NOW);
PCM_SIGNAL(S_CWND);

PCM_VARIABLE(V_MODE);
PCM_VARIABLE(V_INITIALIZED);
PCM_VARIABLE(V_MIN_RTT);
PCM_VARIABLE(V_MIN_RTT_STAMP);
PCM_VARIABLE(V_BW_ESTIMATE);
PCM_VARIABLE(V_BW_BEST);
PCM_VARIABLE(V_BW_STABLE_ROUNDS);
PCM_VARIABLE(V_SAMPLE_START);
PCM_VARIABLE(V_SAMPLE_BYTES);
PCM_VARIABLE(V_MODE_START);
PCM_VARIABLE(V_PROBE_PHASE);
PCM_VARIABLE(V_LAST_NACK_DECREASE);
PCM_VARIABLE(V_HAS_NACK_DECREASE);
PCM_PARAMETER_UINT(P_MSS);
PCM_PARAMETER_UINT(P_MIN_CWND);
PCM_PARAMETER_UINT(P_MAX_CWND);
PCM_PARAMETER_UINT(P_INITIAL_CWND);
PCM_PARAMETER_UINT(P_SAMPLE_INTERVAL);
PCM_PARAMETER_UINT(P_MIN_RTT_WINDOW);
PCM_PARAMETER_UINT(P_PROBE_RTT_DURATION);
PCM_PARAMETER_UINT(P_FULL_BW_ROUNDS);

PCM_PARAMETER_FLOAT(P_STARTUP_GAIN);
PCM_PARAMETER_FLOAT(P_CWND_GAIN);
PCM_PARAMETER_FLOAT(P_FULL_BW_THRESHOLD);
PCM_PARAMETER_FLOAT(P_PROBE_UP_GAIN);
PCM_PARAMETER_FLOAT(P_PROBE_DOWN_GAIN);
PCM_PARAMETER_FLOAT(P_NACK_REDUCTION);

namespace {
constexpr unsigned int STARTUP = 0;
constexpr unsigned int DRAIN = 1;
constexpr unsigned int PROBE_BW = 2;
constexpr unsigned int PROBE_RTT = 3;
}

PCM_HANDLER(bbr) {
    uet_trigmask_t fired = 0;
    uet_trigger_mask(&fired);

    const bool ack = (fired & T_ACK) != 0;
    const bool nack = (fired & T_NACK) != 0;

    if (!ack && !nack)
        return;

    const auto now = static_cast<uet_uint_t>(
        std::max<uet_int_t>(0, uet_signal_get(S_NOW)));

    const double min_cwnd =
        std::max(1.0, double(P_MIN_CWND));
    const double max_cwnd =
        std::max(min_cwnd, double(P_MAX_CWND));

    double cwnd = double(uet_signal_get(S_CWND));

    uet_uint_t mode = STARTUP;
    uet_uint_t initialized = 0;
    uet_uint_t min_rtt = 0;
    uet_uint_t min_rtt_stamp = 0;
    uet_float_t bw = 0.0;
    uet_float_t bw_best = 0.0;
    uet_uint_t stable_rounds = 0;
    uet_uint_t sample_start = 0;
    uet_uint_t sample_bytes = 0;
    uet_uint_t mode_start = 0;
    uet_uint_t probe_phase = 0;
    uet_uint_t last_nack_decrease = 0;
    uet_uint_t has_nack_decrease = 0;
    uet_get_variable_uint(V_MODE, &mode);
    uet_get_variable_uint(V_INITIALIZED, &initialized);
    uet_get_variable_uint(V_MIN_RTT, &min_rtt);
    uet_get_variable_uint(V_MIN_RTT_STAMP, &min_rtt_stamp);
    uet_get_variable_float(V_BW_ESTIMATE, &bw);
    uet_get_variable_float(V_BW_BEST, &bw_best);
    uet_get_variable_uint(V_BW_STABLE_ROUNDS, &stable_rounds);
    uet_get_variable_uint(V_SAMPLE_START, &sample_start);
    uet_get_variable_uint(V_SAMPLE_BYTES, &sample_bytes);
    uet_get_variable_uint(V_MODE_START, &mode_start);
    uet_get_variable_uint(V_PROBE_PHASE, &probe_phase);
    uet_get_variable_uint(V_LAST_NACK_DECREASE,
                          &last_nack_decrease);
    uet_get_variable_uint(V_HAS_NACK_DECREASE, &has_nack_decrease);
    const uet_uint_t interval =
        std::max<uet_uint_t>(1, P_SAMPLE_INTERVAL);

    if (!initialized) {
        initialized = 1;
        mode = STARTUP;
        mode_start = now;
        sample_start = now;
        last_nack_decrease = now;

        if (cwnd <= 0)
            cwnd = double(P_INITIAL_CWND);
    }

    cwnd = std::clamp(cwnd, min_cwnd, max_cwnd);

    // Only successful ACKs contribute to delivered bandwidth.
    if (ack) {
        const auto raw_rtt = uet_signal_get(S_ACK_RTT);

        if (raw_rtt > 0) {
            const auto rtt =
                static_cast<uet_uint_t>(raw_rtt);

            const bool expired =
                min_rtt > 0 &&
                now >= min_rtt_stamp &&
                now - min_rtt_stamp >= P_MIN_RTT_WINDOW;

            // Preserve the previous minimum until PROBE_RTT
            // finishes; do not replace it with a queued RTT.
            if (min_rtt == 0 || rtt < min_rtt) {
                min_rtt = rtt;
                min_rtt_stamp = now;
            } else if (expired && mode != PROBE_RTT) {
                mode = PROBE_RTT;
                mode_start = now;
            }
        }
        const auto ack_units =
        std::max<uet_int_t>(0, uet_signal_get(S_ACK_BYTES));

    const uet_uint_t max_uint =
        std::numeric_limits<uet_uint_t>::max();

    const uet_uint_t ack_count =
        static_cast<uet_uint_t>(ack_units);

    const uet_uint_t ack_bytes =
        ack_count > max_uint / 256
            ? max_uint
            : ack_count * 256;

    sample_bytes =
        ack_bytes > max_uint - sample_bytes
            ? max_uint
            : sample_bytes + ack_bytes;


        if (now >= sample_start &&
            now - sample_start >= interval) {

            const auto elapsed = now - sample_start;

            // Units: bytes per 128-ns tick.
            const double sample_bw =
                double(sample_bytes) / double(elapsed);

            // Simple decaying maximum, not BBR's original
            // packet-based max-bandwidth filter.
            // An interval with no delivered data must not create
            // a new positive bandwidth sample.
            //
            // The decaying maximum is a simple bandwidth filter,
            // not BBRv1's delivery-rate max filter.
            if (sample_bw > 0.0 && std::isfinite(sample_bw)) {
                bw = std::max(sample_bw, double(bw) * 0.9);
            }

            sample_bytes = 0;
            sample_start = now;

            if (mode == STARTUP && bw > 0) {
                const double threshold =
                    std::max(1.0, double(P_FULL_BW_THRESHOLD));

                if (double(bw) >=
                    double(bw_best) * threshold) {
                    bw_best = bw;
                    stable_rounds = 0;
                } else {
                    ++stable_rounds;
                }

                if (stable_rounds >= P_FULL_BW_ROUNDS) {
                    mode = DRAIN;
                    mode_start = now;
                }
            }
        }
    }

    // NACKs indicate negative feedback in this simulator.
    // This is a UET-specific adaptation, not native BBRv1.

    // UET-specific congestion response.
    // Apply the first NACK reduction immediately, then limit
    // further reductions to one per sampling interval.
    if (nack) {
        const bool reduction_due =
            !has_nack_decrease ||
            (now >= last_nack_decrease &&
            now - last_nack_decrease >= interval);

        if (reduction_due) {
            const double reduction = std::clamp(
                double(P_NACK_REDUCTION), 0.0, 1.0);

            cwnd *= 1.0 - reduction;
            last_nack_decrease = now;
            has_nack_decrease = 1;
        }
    }

    const double nack_cwnd_cap = cwnd;
    // Estimated bandwidth-delay product, in bytes.
    const double bdp =
        double(bw) * double(min_rtt);

    const double mss =
        std::max(1.0, double(P_MSS));

    if (mode == STARTUP) {
        // ACK-clocked growth while discovering bandwidth.
        if (ack) {
            const double ack_bytes =
                double(std::max<uet_int_t>(
                    0, uet_signal_get(S_ACK_BYTES))) * 256.0;

            cwnd += std::min(ack_bytes, cwnd);

            if (bdp > 0) {
                cwnd = std::min(
                    cwnd,
                    std::max(double(P_INITIAL_CWND),
                             double(P_STARTUP_GAIN) * bdp));
            }
        }

    } else if (mode == DRAIN) {
        if (bdp > 0) {
            const double target =
                std::clamp(bdp, min_cwnd, max_cwnd);

            // Reduce CWND gradually rather than forcing it
            // immediately to BDP.
            //
            // This is a CWND-only approximation of draining;
            // BBRv1 normally uses a reduced pacing gain.
            if (cwnd > target) {
                cwnd = std::max(target, cwnd * 0.75);
            }

            // Stay in DRAIN until CWND approaches the
            // estimated bandwidth-delay product.
            if (cwnd <= target * 1.05) {
                mode = PROBE_BW;
                mode_start = now;
                probe_phase = 0;
            }
        }
    } else if (mode == PROBE_BW) {
        const uet_uint_t phase_duration =
            std::max<uet_uint_t>(interval, min_rtt);

        if (now >= mode_start &&
            now - mode_start >= phase_duration) {
            probe_phase = (probe_phase + 1) % 8;
            mode_start = now;
        }

        double gain = 1.0;

        if (probe_phase == 0)
            gain = double(P_PROBE_UP_GAIN);
        else if (probe_phase == 1)
            gain = double(P_PROBE_DOWN_GAIN);

                if (bdp > 0) {
                    // PCM cannot adjust pacing rate, so the probing gain
                    // modifies the congestion-window target instead.
                    const double base_cwnd_target =
                        double(P_CWND_GAIN) * bdp;

                    const double probe_cwnd_target =
                        base_cwnd_target * gain;

                    const double target = std::clamp(
                        probe_cwnd_target, min_cwnd, max_cwnd);

            // Move gradually toward the estimated target.
            // ACKs can grow the window; NACKs cannot.
            if (ack && target > cwnd) {
                cwnd += std::min(
                    mss, target - cwnd);
            } else if (target < cwnd) {
                cwnd = std::max(target, cwnd * 0.9);
            }
        }
    } else if (mode == PROBE_RTT) {
        cwnd = std::min(cwnd, 4.0 * mss);

        if (now >= mode_start &&
            now - mode_start >= P_PROBE_RTT_DURATION) {
            mode = PROBE_BW;
            mode_start = now;
            probe_phase = 0;

            // Restart the minimum RTT measurement window.
            // The next valid ACK can establish a fresh minimum.
            min_rtt = 0;
            min_rtt_stamp = now;
        }
    }

    // Preserve congestion feedback when ACK and NACK fire together.
    if (nack)
        cwnd = std::min(cwnd, nack_cwnd_cap);

    cwnd = std::clamp(cwnd, min_cwnd, max_cwnd);

    uet_set_variable_uint(V_MODE, mode);
    uet_set_variable_uint(V_INITIALIZED, initialized);
    uet_set_variable_uint(V_MIN_RTT, min_rtt);
    uet_set_variable_uint(V_MIN_RTT_STAMP, min_rtt_stamp);
    uet_set_variable_float(V_BW_ESTIMATE, bw);
    uet_set_variable_float(V_BW_BEST, bw_best);
    uet_set_variable_uint(V_BW_STABLE_ROUNDS, stable_rounds);
    uet_set_variable_uint(V_SAMPLE_START, sample_start);
    uet_set_variable_uint(V_SAMPLE_BYTES, sample_bytes);
    uet_set_variable_uint(V_MODE_START, mode_start);
    uet_set_variable_uint(V_PROBE_PHASE, probe_phase);
    uet_set_variable_uint(V_LAST_NACK_DECREASE,
                          last_nack_decrease);
    uet_set_variable_uint(V_HAS_NACK_DECREASE, has_nack_decrease);
    uet_set_cwnd(static_cast<unsigned int>(cwnd));
}
