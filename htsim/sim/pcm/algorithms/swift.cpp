
#include "uet_pcm.h"

#include <algorithm>
#include <cmath>

PCM_ALGORITHM(swift);

PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_ACK_BYTES);
PCM_SIGNAL(S_ACK_RTT);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_NACK_RTT);
PCM_SIGNAL(S_NOW);
PCM_SIGNAL(S_CWND);

PCM_VARIABLE(V_SMOOTHED_RTT);
PCM_VARIABLE(V_LAST_DECREASE);
PCM_VARIABLE(V_HAS_DECREASED);
PCM_VARIABLE(V_INITIALIZED);
PCM_VARIABLE(V_DECREASE_INTERVAL);
PCM_PARAMETER_UINT(P_MSS);
PCM_PARAMETER_UINT(P_MIN_CWND);
PCM_PARAMETER_UINT(P_MAX_CWND);
PCM_PARAMETER_UINT(P_INITIAL_CWND);
PCM_PARAMETER_UINT(P_BASE_DELAY);
PCM_PARAMETER_UINT(P_FS_RANGE);
PCM_PARAMETER_FLOAT(P_HOP_DELAY);
PCM_PARAMETER_UINT(P_HOP_COUNT);
PCM_PARAMETER_FLOAT(P_AI);
PCM_PARAMETER_FLOAT(P_BETA);
PCM_PARAMETER_FLOAT(P_MAX_MDF);

PCM_HANDLER(swift) {
    uet_trigmask_t fired = 0;
    uet_trigger_mask(&fired);

    const bool ack = (fired & T_ACK) != 0;
    const bool nack = (fired & T_NACK) != 0;

    const uet_uint_t now = static_cast<uet_uint_t>(
        std::max<uet_int_t>(0, uet_signal_get(S_NOW)));

    uet_uint_t smoothed_rtt = 0;
    uet_uint_t last_decrease = 0;
    uet_uint_t has_decreased = 0;
    uet_uint_t initialized = 0;
    uet_uint_t decrease_interval = 0;
    uet_get_variable_uint(V_SMOOTHED_RTT, &smoothed_rtt);
    uet_get_variable_uint(V_LAST_DECREASE, &last_decrease);
    uet_get_variable_uint(V_HAS_DECREASED, &has_decreased);
    uet_get_variable_uint(V_INITIALIZED, &initialized);
    uet_get_variable_uint(V_DECREASE_INTERVAL, &decrease_interval);

    const double mss = std::max(1.0, double(P_MSS));
    const double min_cwnd = std::max(1.0, double(P_MIN_CWND));
    const double max_cwnd =
        std::max(min_cwnd, double(P_MAX_CWND));

    double cwnd = double(uet_signal_get(S_CWND));

    if (!initialized) {
        // Start with the sender's existing window when available.
        if (cwnd <= 0.0)
            cwnd = double(P_INITIAL_CWND);

        cwnd = std::clamp(cwnd, min_cwnd, max_cwnd);
        initialized = 1;
    }

    // Calculate Swift's flow-scaling delay.
    const double fs_min_cwnd = 0.1;
    const double fs_max_cwnd = 100.0;

    const double fs_alpha =
        double(P_FS_RANGE) /
        (1.0 / std::sqrt(fs_min_cwnd) -
         1.0 / std::sqrt(fs_max_cwnd));

    const double fs_beta =
        -fs_alpha / std::sqrt(fs_max_cwnd);

    const double window_packets = std::max(0.001, cwnd / mss);

    const double fs_delay = std::clamp(
        fs_alpha / std::sqrt(window_packets) + fs_beta,
        0.0,
        double(P_FS_RANGE));

    const double target_delay =
        double(P_BASE_DELAY) +
        fs_delay +
        double(P_HOP_COUNT) * double(P_HOP_DELAY);

    if (ack) {
        const uet_int_t raw_rtt = uet_signal_get(S_ACK_RTT);

        if (raw_rtt > 0) {
            const uet_uint_t rtt =
                static_cast<uet_uint_t>(raw_rtt);

            // Maintain a 7/8 RTT EWMA.
            // Compare the raw RTT sample with the target delay.

            if (smoothed_rtt == 0) {
                smoothed_rtt = rtt;
            } else {
                const double ewma =
                    0.875 * double(smoothed_rtt) +
                    0.125 * double(rtt);

                smoothed_rtt =
                    static_cast<uet_uint_t>(ewma);
            }

            const uet_uint_t gating_rtt =
                std::max(smoothed_rtt, rtt);

                const bool can_decrease =
                    !has_decreased ||
                    (now >= last_decrease &&
                    now - last_decrease >= decrease_interval);

            if (double(rtt) < target_delay) {
                // ACK bytes are reported in 256-byte units.
                const double acked_bytes =
                    256.0 * std::max<uet_int_t>(
                        0, uet_signal_get(S_ACK_BYTES));

                    if (acked_bytes > 0.0) {
                        if (cwnd >= mss) {
                            cwnd +=
                                mss * double(P_AI) * acked_bytes / cwnd;
                        } else {
                            cwnd += double(P_AI) * acked_bytes;
                        }
                    }

            } else if (can_decrease) {
                // Swift delay-based multiplicative decrease.
                const double reduction =
                    1.0 -
                    double(P_BETA) *
                    (double(rtt) - target_delay) /
                    double(rtt);

                const double factor = std::max(
                    reduction,
                    1.0 - double(P_MAX_MDF));

                cwnd *= factor;
                last_decrease = now;
                decrease_interval = gating_rtt;
                has_decreased = 1;
            }
        }
    }

    if (nack) {
        // A NACK is not necessarily an ECN congestion indication.
        // Only reduce CWND when valid RTT feedback indicates
        // delay above Swift's target.
        const uet_int_t raw_nack_rtt =
            uet_signal_get(S_NACK_RTT);

        if (raw_nack_rtt > 0) {
            const uet_uint_t nack_rtt =
                static_cast<uet_uint_t>(raw_nack_rtt);

                const uet_uint_t gating_rtt =
                    std::max(smoothed_rtt, nack_rtt);

                const bool can_decrease =
                    !has_decreased ||
                    (now >= last_decrease &&
                    now - last_decrease >= decrease_interval);

            if (double(nack_rtt) >= target_delay &&
                can_decrease) {

                const double reduction =
                    1.0 -
                    double(P_BETA) *
                    (double(nack_rtt) - target_delay) /
                    double(nack_rtt);

                const double factor = std::max(
                    reduction,
                    1.0 - double(P_MAX_MDF));

                cwnd *= factor;
                last_decrease = now;
                decrease_interval = gating_rtt;
                has_decreased = 1;
            }
        }
    }

    cwnd = std::clamp(cwnd, min_cwnd, max_cwnd);

    uet_set_variable_uint(V_SMOOTHED_RTT, smoothed_rtt);
    uet_set_variable_uint(V_LAST_DECREASE, last_decrease);
    uet_set_variable_uint(V_HAS_DECREASED, has_decreased);
    uet_set_variable_uint(V_INITIALIZED, initialized);
    uet_set_variable_uint(V_DECREASE_INTERVAL, decrease_interval);

    uet_set_cwnd(static_cast<unsigned int>(cwnd));
}
