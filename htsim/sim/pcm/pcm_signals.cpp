// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_signals.h"

namespace {

using D = PcmSignalDir;
using U = PcmUnit;

// Spec v1.1 Table 3-109 (mandatory signals), Table 3-112 (optional signals),
// then the vendor signals added by this simulator.
//
// Only the sender-side congestion control signals listed in
// docs/pcm-plan.md §3 are supported. The spec requires implementations to
// support every mandatory signal (§3.6.18.4). We support a subset, because the
// receiver side, credits, load balancing, telegrams and remote service time are
// out of scope or not modelled in htsim. Loading a YAML file that uses an
// unsupported signal fails, which is the behaviour the spec requires for
// signals an implementation does not support. The unsupported signals are
// listed so that the error can say "not supported" rather than "unknown".
const std::vector<PcmSignalInfo> kSignals = {
    //  name                          dir      unit             vendor supported
    // Mandatory signals, Table 3-109.
    {"uet_rx_ack",                    D::RX,   U::COUNT,        false, true},
    {"uet_rx_ev",                     D::RX,   U::COUNT,        false, false},
    {"uet_rx_ack_bytes",              D::RX,   U::BYTES_256,    false, true},
    {"uet_rx_ack_count",              D::RX,   U::COUNT,        false, true},
    {"uet_rx_ack_ecn",                D::RX,   U::COUNT,        false, true},
    {"uet_rx_ack_probe",              D::RX,   U::COUNT,        false, true},
    {"uet_rx_ack_rtt",                D::RX,   U::TIME_128NS,   false, true},
    {"uet_rx_ack_svc",                D::RX,   U::TIME_128NS,   false, false},
    {"uet_rx_credit",                 D::RX,   U::COUNT,        false, false},
    {"uet_rx_credit_req",             D::RX,   U::COUNT,        false, false},
    {"uet_rx_credit_req_value",       D::RX,   U::COUNT,        false, false},
    {"uet_rx_ctl",                    D::RX,   U::COUNT,        false, false},
    {"uet_rx_nack",                   D::RX,   U::COUNT,        false, true},
    {"uet_rx_nack_ecn",               D::RX,   U::COUNT,        false, true},
    {"uet_rx_nack_last_hop_trim",     D::RX,   U::COUNT,        false, true},
    {"uet_rx_nack_rtt",               D::RX,   U::TIME_128NS,   false, true},
    {"uet_rx_nack_trim",              D::RX,   U::COUNT,        false, true},
    {"uet_rx_probe",                  D::RX,   U::COUNT,        false, false},
    {"uet_rx_probe_id",               D::RX,   U::COUNT,        false, false},
    {"uet_rx_req",                    D::RX,   U::COUNT,        false, false},
    {"uet_rx_req_ecn",                D::RX,   U::COUNT,        false, false},
    {"uet_rx_req_trim",               D::RX,   U::COUNT,        false, false},
    {"uet_rx_req_last_hop_trim",      D::RX,   U::COUNT,        false, false},
    {"uet_time_current",              D::TIME, U::TIME_128NS,   false, true},
    {"uet_time_last_update",          D::TIME, U::TIME_128NS,   false, true},
    {"uet_tx_backlog_bytes",          D::TX,   U::BYTES,        false, true},
    {"uet_tx_req",                    D::TX,   U::COUNT,        false, true},
    {"uet_rx_req_bytes",              D::RX,   U::BYTES,        false, false},
    {"uet_tx_req_bytes",              D::TX,   U::BYTES,        false, true},
    {"uet_tx_rto",                    D::TX,   U::COUNT,        false, true},
    {"uet_tx_cookie",                 D::TX,   U::COUNT,        false, false},
    {"uet_rx_cookie",                 D::RX,   U::COUNT,        false, false},
    {"uet_tx_ev",                     D::TX,   U::COUNT,        false, false},
    {"uet_tx_inflight_bytes",         D::TX,   U::BYTES,        false, true},
    {"uet_tx_cwnd",                   D::TX,   U::BYTES,        false, true},
    {"uet_tx_send_rate",              D::TX,   U::RATE_128MIBS, false, false},
    {"uet_tx_ack_per_packet",         D::TX,   U::BOOL,         false, false},
    {"uet_tx_remote_ack_per_packet",  D::TX,   U::BOOL,         false, false},
    {"uet_rx_ack_gen_bytes",          D::RX,   U::BYTES,        false, false},
    {"uet_rx_ack_gen_timer",          D::RX,   U::TIME_128NS,   false, false},
    {"uet_rx_cwnd_penalty",           D::RX,   U::COUNT,        false, false},
    {"uet_tx_fep_credit_rate",        D::TX,   U::PERCENT_1,    false, false},
    {"uet_rx_telegram_local",         D::RX,   U::COUNT,        false, false},
    {"uet_rx_telegram_peer",          D::RX,   U::COUNT,        false, false},
    {"uet_rx_telegram_type",          D::RX,   U::COUNT,        false, false},
    {"uet_tx_ev_tbl_depth",           D::TX,   U::COUNT,        false, false},
    {"uet_tx_ev_tbl_idx",             D::TX,   U::COUNT,        false, false},
    // Listed only in the load-balancing table (Table 3-111), not in Table 3-109.
    {"uet_tx_probe_id",               D::TX,   U::COUNT,        false, false},

    // Optional signals, Table 3-112.
    {"uet_rx_csig_d",                 D::RX,   U::COUNT,        false, false},
    {"uet_rx_csig_m",                 D::RX,   U::COUNT,        false, false},
    {"uet_rx_csig_s",                 D::RX,   U::COUNT,        false, false},
    {"uet_rx_csig_t",                 D::RX,   U::COUNT,        false, false},
    {"uet_rx_csig_type",              D::RX,   U::BOOL,         false, false},
    {"uet_rx_restore_cwnd",           D::RX,   U::COUNT,        false, false},
    {"uet_rx_staging_buffer_occ",     D::RX,   U::COUNT,        false, false},

    // Vendor signals added by this simulator. The spec has no signal for the
    // number of NACKed bytes, which a port of htsim's existing DCTCP needs. The
    // name follows the spec's rule for vendor signals (§3.6.18.4.6):
    // uet_rx_<pkt>_vend_<signal_name>.
    {"uet_rx_nack_vend_htsim_bytes",  D::RX,   U::BYTES,        true,  true},
};

}  // namespace

const PcmSignalInfo* pcmFindSignal(const std::string& name) {
    for (const auto& s : kSignals) {
        if (name == s.name)
            return &s;
    }
    return nullptr;
}

const std::vector<PcmSignalInfo>& pcmAllSignals() { return kSignals; }
