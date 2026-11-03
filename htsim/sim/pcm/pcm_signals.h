// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PCM_SIGNALS_H
#define PCM_SIGNALS_H

// The PCM signals defined by the UE spec v1.1 (§3.6.18.4, Tables 3-109 and
// 3-112), plus the vendor-specific signals this simulator adds, and which of
// them the simulator supports.

#include <string>
#include <vector>

// Which kind of event updates the signal. Taken from the signal name:
// uet_rx_* (packet received), uet_tx_* (packet sent), uet_time_* (timer).
enum class PcmSignalDir { RX, TX, TIME };

// Unit of the signal value, as listed in Table 3-109.
enum class PcmUnit {
    COUNT,         // unit "1": a count or an identifier
    BYTES,         // unit "1 B", and the byte counts listed with unit "1"
                   // (uet_tx_inflight_bytes, uet_tx_cwnd)
    BYTES_256,     // unit "256 B"
    TIME_128NS,    // unit "128 ns"
    RATE_128MIBS,  // unit "128 MiB/s"
    PERCENT_1,     // unit "0.01 (1% increments)"
    BOOL
};

struct PcmSignalInfo {
    const char* name;
    PcmSignalDir dir;
    PcmUnit unit;
    bool vendor;     // vendor-specific signal added by this simulator
    bool supported;  // implemented by this simulator
};

// Returns the signal with this exact name, or nullptr if the name is not a
// spec or vendor signal.
const PcmSignalInfo* pcmFindSignal(const std::string& name);

// All known signals, in table order.
const std::vector<PcmSignalInfo>& pcmAllSignals();

#endif  // PCM_SIGNALS_H
