
#include "uet_pcm.h"

#include <algorithm>
#include <cmath>
PCM_ALGORITHM(timely);

PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_ACK_BYTES);
PCM_SIGNAL(S_ACK_RTT);
PCM_SIGNAL(S_NOW);
PCM_SIGNAL(S_CWND);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_NACK_RTT);
PCM_VARIABLE(V_PREV_RTT);
PCM_VARIABLE(V_GRADIENT);
PCM_VARIABLE(V_LAST_UPDATE);
PCM_VARIABLE(V_INITIALIZED);

PCM_PARAMETER_UINT(P_MIN_CWND);
PCM_PARAMETER_UINT(P_MAX_CWND);
PCM_PARAMETER_UINT(P_INITIAL_CWND);
PCM_PARAMETER_UINT(P_T_LOW);
PCM_PARAMETER_UINT(P_T_HIGH);
PCM_PARAMETER_FLOAT(P_ALPHA);
PCM_PARAMETER_FLOAT(P_BETA);
PCM_PARAMETER_FLOAT(P_SMOOTHING);
PCM_PARAMETER_FLOAT(P_NACK_REDUCTION);
PCM_PARAMETER_UINT(P_UPDATE_INTERVAL);

PCM_HANDLER(timely) {
    uet_trigmask_t fired = 0;
    uet_trigger_mask(&fired);

    const bool ack = (fired & T_ACK) != 0;
    const bool nack = (fired & T_NACK) != 0;

    if (!ack && !nack)
        return;

    const uet_int_t raw_rtt = ack
        ? uet_signal_get(S_ACK_RTT)
        : uet_signal_get(S_NACK_RTT);

    if (raw_rtt <= 0)
        return;

    const uet_uint_t now = static_cast<uet_uint_t>(
        std::max<uet_int_t>(0, uet_signal_get(S_NOW)));

    uet_uint_t prev_rtt = 0;
    uet_float_t gradient = 0.0;
    uet_uint_t last_update = 0;
    uet_uint_t initialized = 0;

    uet_get_variable_uint(V_PREV_RTT, &prev_rtt);
    uet_get_variable_float(V_GRADIENT, &gradient);
    uet_get_variable_uint(V_LAST_UPDATE, &last_update);
    uet_get_variable_uint(V_INITIALIZED, &initialized);

    const double min_cwnd = std::max(
        1.0, double(P_MIN_CWND));

    const double max_cwnd = std::max(
        min_cwnd, double(P_MAX_CWND));

    double cwnd = double(uet_signal_get(S_CWND));

    if (!initialized) {
        if (cwnd <= 0.0)
            cwnd = double(P_INITIAL_CWND);

        cwnd = std::clamp(cwnd, min_cwnd, max_cwnd);

        prev_rtt = static_cast<uet_uint_t>(raw_rtt);
        last_update = now;
        initialized = 1;

        uet_set_variable_uint(V_PREV_RTT, prev_rtt);
        uet_set_variable_float(V_GRADIENT, gradient);
        uet_set_variable_uint(V_LAST_UPDATE, last_update);
        uet_set_variable_uint(V_INITIALIZED, initialized);

        uet_set_cwnd(static_cast<unsigned int>(cwnd));
        return;
    }
    // NACK feedback is handled separately from TIMELY's
    // ACK-based RTT-gradient calculations.
        // NACK feedback is handled separately from TIMELY's
    // ACK-based RTT-gradient calculations.
    if (nack && !ack) {
        const uet_uint_t update_interval =
            std::max<uet_uint_t>(1, P_UPDATE_INTERVAL);

        const bool update_due =
            now >= last_update &&
            now - last_update >= update_interval;

        if (update_due &&
            double(raw_rtt) > double(P_T_HIGH)) {

            const double reduction = std::clamp(
                double(P_NACK_REDUCTION), 0.0, 1.0);

            cwnd *= 1.0 - reduction;
            cwnd = std::clamp(cwnd, min_cwnd, max_cwnd);
            last_update = now;
        }

        uet_set_variable_uint(V_PREV_RTT, prev_rtt);
        uet_set_variable_float(V_GRADIENT, gradient);
        uet_set_variable_uint(V_LAST_UPDATE, last_update);
        uet_set_variable_uint(V_INITIALIZED, initialized);

        uet_set_cwnd(static_cast<unsigned int>(cwnd));
        return;
    }
    const double rtt = double(raw_rtt);

    const uet_uint_t update_interval =
        std::max<uet_uint_t>(1, P_UPDATE_INTERVAL);

    const bool update_due =
        now >= last_update &&
        now - last_update >= update_interval;

    if (update_due) {
        // Calculate the RTT gradient over the control
        // update interval, not between consecutive ACKs.
        const double new_gradient =
            (rtt - double(prev_rtt)) /
            std::max(1.0, double(prev_rtt));

        const double smoothing = std::clamp(
            double(P_SMOOTHING), 0.0, 1.0);

        gradient =
            smoothing * gradient +
            (1.0 - smoothing) * new_gradient;

        // Save the RTT associated with this decision.
        prev_rtt = static_cast<uet_uint_t>(raw_rtt);


        const double alpha = std::max(
            0.0, double(P_ALPHA));

        const double beta = std::clamp(
            double(P_BETA), 0.0, 1.0);

        if (rtt < double(P_T_LOW)) {
            // Low latency: additive increase.
            cwnd *= 1.0 + alpha;

        } else if (rtt > double(P_T_HIGH)) {
            // High latency: multiplicative decrease.
            cwnd *= 1.0 - beta *
                (1.0 - double(P_T_HIGH) / rtt);

        } else if (gradient <= 0.0) {
            // RTT is stable or falling.
            cwnd *= 1.0 + alpha;

        } else {
            // Rising RTT: gradient-based decrease.
            const double reduction =
                std::clamp(beta * double(gradient), 0.0, 0.5);

            cwnd *= 1.0 - reduction;
        }

        cwnd = std::clamp(cwnd, min_cwnd, max_cwnd);
        last_update = now;
    }

    uet_set_variable_uint(V_PREV_RTT, prev_rtt);
    uet_set_variable_float(V_GRADIENT, gradient);
    uet_set_variable_uint(V_LAST_UPDATE, last_update);
    uet_set_variable_uint(V_INITIALIZED, initialized);

    uet_set_cwnd(static_cast<unsigned int>(cwnd));
}
