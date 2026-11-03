// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// DCTCP (Alizadeh et al., SIGCOMM 2010), in the form Linux implements it.
//
// - alpha estimates the fraction of packets that see congestion. Once per
//   window of data (about one RTT), with F the fraction of that window's
//   packets that were ECN-marked:
//       alpha = (1 - g) * alpha + g * F
// - On the first ECN mark in a window, the window is cut once:
//       cwnd = cwnd * (1 - alpha / 2)
// - Unmarked ACKs grow the window by about one packet per RTT.
// - A NACK (a trimmed packet) halves the window, at most once per window.
//   DCTCP has no trimming; a trimmed packet saw a full queue, so it counts as a
//   marked packet in F. NACKed packets also count towards the end of the
//   window, so that under heavy trimming, when almost nothing is ACKed,
//   windows still end and the window keeps shrinking.
//
// Counts packets (uet_rx_ack_count) rather than bytes, so it works the same
// in spec and native units.

#include "uet_pcm.h"

PCM_ALGORITHM(dctcp);
PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_ACKED_PKTS);
PCM_SIGNAL(S_ECN);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_CWND);
PCM_VARIABLE(V_ALPHA);
PCM_VARIABLE(V_WIN_TARGET);
PCM_VARIABLE(V_WIN_SEEN);
PCM_VARIABLE(V_WIN_MARKED);
PCM_VARIABLE(V_CUT);
PCM_PARAMETER_FLOAT(P_G);
PCM_PARAMETER_UINT(P_MTU);

PCM_HANDLER(dctcp) {
    uet_trigmask_t fired;
    uet_trigger_mask(&fired);

    const double mtu = double(uet_uint_t(P_MTU));
    double cwnd = double(uet_signal_get(S_CWND));
    uet_float_t alpha;
    uet_uint_t target, seen, marked, cut;
    uet_get_variable_float(V_ALPHA, &alpha);
    uet_get_variable_uint(V_WIN_TARGET, &target);
    uet_get_variable_uint(V_WIN_SEEN, &seen);      // packets ACKed or NACKed
    uet_get_variable_uint(V_WIN_MARKED, &marked);  // ... of which ECN-marked or NACKed
    uet_get_variable_uint(V_CUT, &cut);

    // An observation window is cwnd's worth of packets, fixed when it starts.
    if (target == 0)
        target = cwnd / mtu > 1 ? uet_uint_t(cwnd / mtu) : 1;

    if (fired & T_ACK) {
        const uet_uint_t n = uet_uint_t(uet_signal_get(S_ACKED_PKTS));
        const bool ecn = uet_signal_get(S_ECN) != 0;
        seen += n;
        if (ecn) {
            marked += n;
            if (!cut) {
                cwnd = cwnd * (1 - alpha / 2);
                cut = 1;
            }
        } else {
            cwnd += double(n) * mtu * mtu / cwnd;  // about one packet per RTT
        }
    }
    if (fired & T_NACK) {
        seen += 1;
        marked += 1;
        if (!cut) {
            cwnd = cwnd / 2;
            cut = 1;
        }
    }

    // End of the window: update alpha, start the next one.
    if (seen >= target) {
        const double f = double(marked) / double(seen);
        alpha = (1 - P_G) * alpha + P_G * f;
        seen = 0;
        marked = 0;
        cut = 0;
        target = cwnd / mtu > 1 ? uet_uint_t(cwnd / mtu) : 1;
    }

    if (cwnd < mtu)
        cwnd = mtu;
    uet_set_variable_float(V_ALPHA, alpha);
    uet_set_variable_uint(V_WIN_TARGET, target);
    uet_set_variable_uint(V_WIN_SEEN, seen);
    uet_set_variable_uint(V_WIN_MARKED, marked);
    uet_set_variable_uint(V_CUT, cut);
    uet_set_cwnd(unsigned(cwnd));
}
