// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// An exact port of htsim's built-in DCTCP (`-sender_cc_algo dctcp`,
// UecSrc::updateCwndOnAck_DCTCP / updateCwndOnNack_DCTCP in uec.cpp). It
// exists to show that running an algorithm through PCM changes nothing: with
// `-pcm dctcp_legacy.yaml -pcm_units native` a run sends exactly like the
// built-in version. datacenter/pcm_exact_match.sh checks this.
//
// The built-in version is a simplified DCTCP: it has no alpha, and cuts the
// window by a third of the acknowledged bytes on every ECN-marked ACK. For the
// real algorithm, see dctcp.cpp.
//
// Needs native units: the arithmetic uses exact byte counts.

#include "uet_pcm.h"

PCM_ALGORITHM(dctcp_legacy);
PCM_SIGNAL(T_ACK);
PCM_SIGNAL(S_ACKED_BYTES);
PCM_SIGNAL(S_ECN);
PCM_SIGNAL(T_NACK);
PCM_SIGNAL(S_NACKED_BYTES);
PCM_SIGNAL(S_CWND);
PCM_PARAMETER_UINT(P_MTU);

PCM_HANDLER(dctcp_legacy) {
    uet_trigmask_t fired;
    uet_trigger_mask(&fired);

    const int64_t mtu = int64_t(uet_uint_t(P_MTU));
    int64_t cwnd = uet_signal_get(S_CWND);

    if (fired & T_ACK) {
        const int64_t acked = uet_signal_get(S_ACKED_BYTES);
        if (uet_signal_get(S_ECN) == 0) {
            // additive increase, 1 packet per RTT
            cwnd += acked * mtu / cwnd;
        } else {
            // multiplicative decrease, done per mark
            cwnd -= acked / 3;
            cwnd = cwnd > mtu ? cwnd : mtu;
        }
    }
    if (fired & T_NACK) {
        cwnd -= uet_signal_get(S_NACKED_BYTES);
        cwnd = cwnd > mtu ? cwnd : mtu;
    }
    uet_set_cwnd(unsigned(cwnd));
}
