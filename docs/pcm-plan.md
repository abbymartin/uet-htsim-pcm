# PCM Support in UEC htsim — Phase 1 Plan

Branch: `pcm` · Base: `main` @ `65ea315` · Status: planning

References:
- **Spec:** UE Specification v1.1, §3.6.18 *Programmable Congestion Management*
  (pp. 516–547). Section numbers (§) refer to the spec unless they say "paper".
- **Paper:** Khalilov et al., *PCM: Programmable Congestion Management for Terabit
  RDMA NICs*, arXiv:2610.13130 (Oct 2026). The authors integrated PCM into
  UET-htsim (paper §6.1, about 600 lines of C++), but their code is not available to us.

Spec terms used in this doc:

| Term | Meaning |
|---|---|
| FEP | Fabric endpoint, i.e. a NIC |
| PDC | Packet delivery context, i.e. a connection. One htsim `UecSrc` is one PDC. |
| RUD / ROD | Reliable unordered / ordered delivery (PDC modes) |
| CCC | Congestion control context: the congestion state (window, signals, algorithm state) that one or more PDCs share |
| TC | Traffic class |
| NSCC / RCCC | UET's built-in sender-based / receiver-credit-based congestion control |
| LB, EV | Load balancing; entropy value (picks the network path) |

## 1. Goal of this phase

Make `htsim_uec` able to run PCM algorithms as the spec defines them. In the spec,
an algorithm is a C handler function plus a YAML file that declares what the
handler uses: signals, triggers, controls, variables and parameters (§3.6.18.8.5).
The paper calls these two together a *handler package*. The simulator delivers
signals, checks triggers, calls the handler, and applies the controls it sets
(e.g. the congestion window) to the existing `UecSrc` sender.

Phase 1 delivers this runtime, shared CCCs (groups of flows sharing one window),
and **DCTCP**, which is used to verify that everything works. NSCC, Swift, DCQCN,
TIMELY and BBR come in later phases.

Scope decisions:
- **Sender-side congestion control only.** No receiver-side or credit-based PCM,
  no load-balancing algorithms, no telegrams, no CSIG (in-network telemetry).
  Evaluation runs use `-sender_cc_only`.
- **Follow the spec exactly by default.** Use the spec's names, YAML format, API,
  trigger behaviour and runtime model. **Every place we differ from the spec is
  explained in two places:** §7 of this doc, and a plain comment at the code that
  implements it, saying what the spec says, what we do, and why. Each behaviour has
  one chosen meaning; there are no runtime flags for alternative readings of the
  spec.
- **Work lands as a series of small commits**, each building and passing tests
  (§8), rather than as separate phases.

**Done means**

1. `htsim_uec -sender_cc_only -pcm pcm/algorithms/dctcp.yaml` runs a PCM algorithm.
2. **Exact match with the existing DCTCP:** `dctcp_legacy`, a PCM port of the DCTCP
   already in htsim, produces output identical to `-sender_cc_algo dctcp`.
3. Textbook DCTCP (with its α moving average) runs on PCM and behaves sensibly on
   incast and permutation workloads.
4. With PCM unused, `commit_check.sh` passes, so NSCC/RCCC behaviour is unchanged.
5. The settings needed for the proposal's ablations work: trigger thresholds and
   timers set in YAML, synchronous vs. asynchronous execution with a handler
   processing time, and a count of handler invocations per CCC.
6. **Shared CCCs work.** Flows tagged with the same group, source NIC and
   destination NIC share one CCC (one window, one set of signals, variables and
   triggers). Different groups can use different algorithms, or the same algorithm
   with different parameters.

## 2. The PCM model, from the spec

| Concept | Spec | What it means for us |
|---|---|---|
| YAML file | §3.6.18.8.1, §3.6.18.8.5 | Top-level keys `cc_algorithm`, `signal`, `control`, `variable`, `global_variable`, `parameter`, `tgm`. `cc_algorithm.name` is the name of the handler function. |
| Signals | §3.6.18.4, Table 3-109 | Named `uet_<rx\|tx\|time>_<pkt>_<name>`, e.g. `uet_rx_ack_bytes`, `uet_rx_ack_ecn`, `uet_rx_ack_rtt`, `uet_tx_rto`, `uet_time_current`. If a YAML file uses a signal the implementation doesn't support, loading **must fail**. |
| Accumulators | Tables 3-106, 3-107, 3-110 | How repeated updates to a signal combine: `uet_op_sum/max/min/last` (mandatory), `uet_op_ewma` (optional). All updates from one packet apply together. One signal may have several accumulators. |
| Snapshot | §3.6.18.2 | Signal values are frozen when the handler is called. Updates that arrive while it runs count toward the next call. |
| Triggers | §3.6.18.5 | Decide when the handler is called. Each watches one accumulator, with a comparison (`== != > >= < <= *`), a `(count, time)` threshold, `arm_mode` (DISARMED/ONCE/CONTINUOUS), `action` (INVOKE calls the handler, RECORD only notes that the trigger fired), and `invoke_deadline`. The handler reads which triggers fired with `uet_trigger_mask()` and can change them with `uet_modify_trigger()`. |
| Controls | §3.6.18.6.1 | What the handler sets. `uet_set_cwnd` is mandatory. Optional: `uet_set_send_rate`, `uet_set_ack_per_packet`, `uet_set_ack_gen_bytes/timer`, `uet_set_rx_cwnd_penalty`. |
| Variables | §3.6.18.6.2 | Local variables are private to one CCC. Global variables are shared across CCCs, read as a snapshot, and changed only through their accumulator. |
| Parameters | §3.6.18.8.5 | Constants per instance. The spec's example is two NSCC instances with different parameters, one for AI traffic and one for storage; it calls this the instance's "personality". |
| Runtime model | §3.6.18.9 | At most one handler call at a time per CCC. RX and TX events are each delivered in order but **may be skipped**. Time events are always delivered but may be late. |
| Errors | §3.6.18.7 | `uet_ccc_fatal(reason)` puts the CCC into the ERROR state, and so does invalid API use. |
| Units | Tables 3-109, 3-113 | Time in 128 ns units, ACK bytes in 256 B, rate in 128 MiB/s. Integer types are required; float is optional. |
| Assignment | §3.6.18.8.4 | Algorithms are assigned per UET instance: all traffic in the instance uses one algorithm instance. |

From the paper:
- **Execution modes (paper §5.3).** *Synchronous*: the handler runs as part of
  processing the event. *Asynchronous*: it runs separately from the transport, so
  each call takes time `Th`, and events that arrive meanwhile build up for the next
  call. In their htsim version, asynchronous mode was "a separate event chain".
- **Reaction-delay experiment (paper §7.3, Fig. 6).** Swift on an 8:1 incast of
  3 MB, senders starting 25 µs apart, with the handler delayed by `T = N·Th`
  (`Th ≈ 1.1 µs`). This matches the proposal's Challenge 2 and is a good target to
  reproduce in a later phase.
- **Handler examples.** Paper Listing 1 is a typical sender handler. It keeps a
  running SUM signal plus a private `PREV_*` variable, and subtracts the two to get
  the change since the last call. Paper App. B, Listing 4, is its YAML file.
- **Test setup** matches the htsim defaults: 1024-host three-tier fat tree,
  100 Gbit/s links, 4150 B packets.

## 3. Where PCM plugs into the current code

| What | Where | Notes |
|---|---|---|
| Algorithm enum | `htsim/sim/uec.h:215` | `enum Sender_CC { DCTCP, NSCC, CONSTANT }`; add `PCM` |
| Algorithm selection | `htsim/sim/uec.cpp:556-574` | sets the `updateCwndOnAck` / `updateCwndOnNack` member pointers |
| ACK handling | `htsim/sim/uec.cpp:904` `processAck` | has `raw_rtt`, `newly_recvd_bytes`, `ecn_echo`, probe flag; the CC call is at `:1058` |
| NACK / trim handling | `htsim/sim/uec.cpp:1524` `processNack` | `pkt_size`, `last_hop`, `ecn_echo`, RTT; the CC call is at `:1582` |
| Retransmit timeout | `htsim/sim/uec.cpp:2319` `rtxTimerExpired` | no CC call today; add one for `uet_tx_rto` |
| Sending | `htsim/sim/uec.cpp:2046` `sendNewPacket`, `:2116` `sendRtxPacket` | for `uet_tx_req`, `uet_tx_req_bytes` |
| May-I-send check | `htsim/sim/uec.cpp:1130` `can_send_NSCC` | window only; rate limiting comes later (§8) |
| Command line | `htsim/sim/datacenter/main_uec.cpp:325` | `-sender_cc_algo` parsing |
| Per-flow setup | `htsim/sim/datacenter/main_uec.cpp:994-999` | `initNscc()` sets `_base_rtt`, `_bdp`, `_maxwnd`; PCM flows reuse it |
| Existing DCTCP | `htsim/sim/uec.cpp:1105-1123` | simplified: no α; `cwnd -= acked/3` per marked ACK; prints a line on every ACK |
| ECN at the receiver | `htsim/sim/uec.cpp:2622` | ECN-marked packets are ACKed immediately; unmarked ones are batched (`-sack_threshold`) |

### Signal mapping (sender side)

| Spec signal | htsim source | Phase 1 |
|---|---|---|
| `uet_rx_ack` | `processAck` | ✅ |
| `uet_rx_ack_bytes` | `newly_recvd_bytes` | ✅ |
| `uet_rx_ack_count` | packets newly acknowledged (cumulative ACK + SACK) | ✅ |
| `uet_rx_ack_ecn` | `pkt.ecn_echo()` | ✅ |
| `uet_rx_ack_rtt` | `raw_rtt` (only when the send record is valid) | ✅ |
| `uet_rx_ack_probe` | `pkt.is_probe_ack()` | ✅ |
| `uet_rx_nack`, `_nack_ecn`, `_nack_rtt`, `_nack_trim`, `_nack_last_hop_trim` | `processNack` | ✅ |
| `uet_tx_req`, `uet_tx_req_bytes` | `sendNewPacket` / `sendRtxPacket` | ✅ |
| `uet_tx_rto` | `rtxTimerExpired` | ✅ |
| `uet_tx_cwnd`, `uet_tx_inflight_bytes`, `uet_tx_backlog_bytes` | `_cwnd`, `_in_flight`, `_backlog` | ✅ |
| `uet_time_current`, `uet_time_last_update` | simulator clock | ✅ |
| `uet_rx_ack_svc` | `UecAckPacket::_residency_time`, which is **never set** today | ❌ (later) |
| `uet_tx_send_rate` | needs rate limiting in the sender | ❌ (with DCQCN/TIMELY) |
| credit, control, probe-received, telegram, EV, cookie, `uet_rx_req*` | receiver side or load balancing | ❌ out of scope |

**One extra signal.** The spec has no signal for the number of NACKed bytes, which
`dctcp_legacy` needs. We add `uet_rx_nack_vend_htsim_bytes`, following the spec's
naming rule for vendor-specific signals (§3.6.18.4.6). The paper had the same gap
and used its own `DATA_NACK_BYTES`.

**ECN detail.** An ACK carries a single ECN bit, for the packet that triggered it.
Since ECN-marked packets are always ACKed immediately, counting marked ACKs
(`uet_rx_ack_ecn`) alongside `uet_rx_ack_bytes` is a reasonable approximation of
DCTCP's fraction of marked bytes. This is a simulator assumption and will be noted
in the code.

## 4. Design

### 4.1 Components and files

```
htsim/sim/pcm/
  uet_pcm.h           the spec's API, used by handlers: uet_signal_get/set/update,
                      uet_set_cwnd, uet_set_send_rate, uet_get/set/update_variable_*,
                      uet_trigger_mask, uet_modify_trigger/query_trigger,
                      uet_trigger_drop_mask, uet_ccc_fatal
  pcm_spec.{h,cpp}    YAML loader: reads and checks an algorithm's YAML file; rejects
                      anything we don't support
  pcm_runtime.{h,cpp} class PcmCcc, one per CCC (§4.5): accumulators, triggers,
                      snapshot, calling the handler, applying controls, ERROR state,
                      counters. It schedules its own timers (time triggers,
                      invoke_deadline, asynchronous completion) on the simulator's
                      event list, so UecSrc's timer code stays untouched.
  pcm_registry.{h,cpp} maps the handler name in the YAML to the compiled function
  algorithms/
    dctcp_legacy.{yaml,cpp}   exact port of the existing DCTCP (for the exact-match test)
    dctcp.{yaml,cpp}          textbook DCTCP
  tests/pcm_*_test.cpp        googletest unit tests
```

### 4.2 Writing handlers like the spec does

Handlers are plain functions with no arguments, as in the spec's examples:

```cpp
// algorithms/dctcp.cpp
#include "uet_pcm.h"
PCM_HANDLER(dctcp) {
    uet_trigmask_t m; uet_trigger_mask(&m);
    uet_uint_t cwnd; uet_get_variable_uint(V_CWND, &cwnd);
    if (m & T_ACK) { /* ... */ }
    uet_set_cwnd(cwnd);
}
```

- The simulator is single-threaded. Before each call, the runtime records which CCC
  is being served, and the `uet_*` functions act on that CCC. Handlers therefore
  read like the spec's C code, and could later be compiled for real hardware.
- **Names** such as `T_ACK`, `S_ACK_BYTES` and `V_CWND` come from the `macro:`
  fields in the YAML. The handler lists the names it uses
  (`PCM_BIND_SIGNAL(S_ACK_BYTES)` etc.). At load time each name is matched to its
  YAML entry, and loading fails on a mismatch. This avoids generating code during
  the build.
- **Parameters** are read from the loaded YAML when the program runs, not compiled
  in as constants. One compiled handler can then run with different parameter
  values from different YAML files, or with `-pcm_param NAME=VALUE`.

### 4.3 YAML

- Follow the §3.6.18.8.5 format exactly where it is clear. Additions are listed
  in §7.
- Parse with **yaml-cpp**, downloaded at configure time via CMake's `FetchContent`
  with a pinned version. The build already gets googletest the same way.
- Algorithm YAML files live in `htsim/sim/pcm/algorithms/`. Experiments refer to
  them by path.

### 4.4 Runtime behaviour

- **Signal updates.** For each packet event, all of that packet's signal updates go
  into their accumulators together. Then the triggers watching those signals are
  checked.
- **Calling the handler.** If any INVOKE trigger fires, the runtime freezes the
  signal values, records which triggers fired, and calls the handler once for all
  of them. It then applies the controls the handler set (`uet_set_cwnd` →
  `UecSrc::_cwnd`) and restarts the CONTINUOUS triggers' counting.
- **Execution modes**, chosen on the command line: `-pcm_exec sync` (the default)
  or `-pcm_exec async -pcm_handler_time_ns Th`.
  - Synchronous: the handler runs inside the ACK/NACK processing, and its new
    window applies before the next send decision. This is what the exact-match
    test uses.
  - Asynchronous: each call takes `Th`, and its controls apply when it finishes.
    Events arriving meanwhile still update the accumulators. Triggers that fire
    meanwhile are combined into the next call, since only one call per CCC can run
    at a time (§3.6.18.9).
  - The paper's `T = N·Th` experiment becomes a sweep over `-pcm_handler_time_ns`.
- **No skipped events.** The spec *allows* an implementation to skip RX/TX events
  under load but doesn't require it, so never skipping is within the spec. All
  events go through one function, so a drop model can be added there later.
  `uet_trigger_drop_mask()` exists and always reports no drops.
- **Time triggers** schedule a wake-up on the simulator's event list.
  `invoke_deadline` follows §3.6.18.5.3: the CCC's next deadline is
  `min(current deadline, now + trigger.invoke_deadline)`.
- **The runtime doesn't clamp the window.** Keeping the window within bounds is the
  algorithm's job, as in the spec. The runtime only rejects impossible values
  (cwnd = 0, overflow) by putting the CCC into the ERROR state. Clamping would also
  break the exact match with the existing DCTCP, which has no upper limit.
- **Units.** `-pcm_units spec` (the default) rounds signals and controls to the
  spec's units (128 ns, 256 B). `-pcm_units native` passes picoseconds and bytes
  through unchanged, which the exact-match test needs. Evaluation should use spec
  units: the paper shows (§7.3) that numeric precision can change an algorithm's
  behaviour.
- **Counters** per CCC and in total: signal updates, triggers fired, handler calls,
  calls that were combined or delayed, and controls applied. They are printed on
  each flow's finish line and in the end-of-run summary. This provides the
  proposal's "number of handler invocations" metric.

### 4.5 CCCs and flow groups

In the spec, a CCC is congestion state shared by a group of PDCs: all RUD PDCs
between the same two FEPs in the same traffic class share one CCC (§3.6, §3.5.8.1).
In htsim, each `UecSrc` is one PDC.

- **Flows are grouped by (group name, source NIC, destination NIC).** Flows with
  the same three values share one `PcmCcc`. They're looked up in a hash map on
  these three values. Traffic class is left out because htsim has none (see §7).
- **Group names come from the connection matrix,** via a new optional
  `ccc <group>` token. The parser already reads `key value` tokens on each line:
  ```
  0->15 start 0  size 2000000 ccc ai        # CCC (ai, 0, 15)
  0->15 start 50 size 2000000 ccc ai        # same CCC, shares the window
  0->15 start 0  size 1000000 ccc storage   # CCC (storage, 0, 15)
  2->15 start 0  size 2000000               # no group: its own CCC
  ```
- **A flow without a group gets its own CCC**, exactly like today's
  one-window-per-flow behaviour (see §7).
- **Choosing an algorithm per group.** `-pcm default.yaml` applies to flows without
  a group. `-pcm_ccc <group>=<file.yaml>` sets the algorithm for a group. Several
  groups can use the same algorithm with different parameters.
- **What is shared and what isn't.** The CCC holds the window (and later the rate),
  the group's bytes in flight, and all accumulators, variables, triggers and
  counters. Each `UecSrc` keeps its own reliability state: sequence numbers, SACK,
  retransmit queue and timeout timer. `uet_tx_inflight_bytes` and
  `uet_tx_backlog_bytes` are totals over the group.
- **Sharing the window.** A flow in a group may send only if the group's window has
  room. When room frees up, the flows with data waiting take turns (round-robin),
  much as `UecNIC` already takes turns between senders. Only PCM flows in shared
  groups use this path. A flow with its own CCC uses the existing per-flow check.
- **Global variables** (§3.6.18.6.2) will be shared by all CCCs on the same source
  NIC. They come after shared CCCs.

### 4.6 Changes to `UecSrc`

- Add `PCM` to `Sender_CC`, and `-sender_cc_algo pcm` (implied by `-pcm FILE`).
- At each point in the §3 table, collect that packet's signal values and pass them
  to the flow's CCC. NSCC's functions stay untouched.
- All new code runs only when `_sender_cc_algo == PCM`, so non-PCM runs execute
  the same code as before.
- `initNscc()` still runs for PCM flows, to set the base RTT, BDP and max window.
  The initial window comes from the YAML (`control.initial_value`). The paper's
  incast study (Fig. 2a) shows why the initial window matters.

## 5. DCTCP for verification

| Algorithm | Purpose | Notes |
|---|---|---|
| `dctcp_legacy` | Shows the PCM runtime changes nothing | Triggers on `uet_rx_ack` and `uet_rx_nack` with `op: wc` and count 1 (every event). Integer arithmetic copied from `uec.cpp:1105-1123`. Native units. |
| `dctcp` | The real algorithm [Alizadeh 2010] | Once per window: α ← (1−g)·α + g·F, where F is the fraction of ECN-marked ACKs in that window; cut once per window to cwnd·(1−α/2); grow by 1 MTU per RTT. Parameters `g`, `init_cwnd`, `ai_bytes`. Switch ECN marking set with `-ecn kmin kmax`. |

**Exact-match test:** run `-sender_cc_algo dctcp` and `-pcm dctcp_legacy.yaml
-pcm_units native` on `one.cm`, `incast.cm` and a permutation matrix, and compare
the outputs. The existing DCTCP prints a `DCTCP start` line on every ACK. Those
lines are filtered out of the comparison, so the existing code and its regression
outputs stay unchanged.

## 6. Testing

1. **Unit tests (googletest),** run without a simulated network:
   - YAML loading: valid files load; unsupported signals and mismatched names are
     rejected.
   - Each accumulator.
   - Triggers: every comparison; count-only / time-only / combined / no-evaluation;
     ONCE vs. CONTINUOUS; RECORD vs. INVOKE; raw/delta/magnitude; a trigger being
     checked only when its own signal updates; `invoke_deadline`.
   - Grouping flows into CCCs, and sharing a window between flows.
   - Frozen values during a handler call.
   - Asynchronous mode combining triggers.
   - ERROR state.
   - DCTCP's α on a hand-written sequence of signals.
2. **Exact-match test** (§5), as a script or a ctest.
3. **Regression suite:** `datacenter/commit_check.sh`, about 11 minutes, run in the
   background.
4. **`datacenter/validate_pcm.txt`:** single flow, 16-node incast and permutation,
   each with a `!tailFCT` bound. Added to `commit_check.sh` once its baseline
   output is committed.

## 7. Where we differ from the spec

Each item below also gets a comment at the code that implements it. When an
algorithm's YAML uses one of our additions (items 4–6), the loader prints a line
saying so, so experiment logs show whether a run used only spec features.

1. **Triggers always compare against `op_val`.** The spec is inconsistent here.
   Its trigger definition (§3.6.18.5.1) compares the accumulator against the same
   `mode_ref` numbers that also set the count and time thresholds. Its YAML format
   (§3.6.18.8.5) has no separate comparison value. But its own example
   (§3.6.18.5.5) uses `op_val`. Taken literally, "fire as soon as RTT exceeds
   15 µs" can't be expressed, so this is most likely a typo. We always compare
   `accumulator (op) op_val`. `mode_refval: <count> <time>` then only sets how many
   true comparisons, or how much time, fires the trigger. `op_val` is required for
   every comparison except the wildcard `wc`.

2. **A trigger is checked only when its own signal was updated.** The spec
   (§3.6.18.5.3) says every received packet checks all triggers on `uet_rx_*`
   signals. An "every 8 ACKs" trigger would then also count NACKs, and the
   handler's list of fired triggers would be misleading. Checking only the
   triggers whose signal the packet updated matches the paper's htsim version
   (paper Fig. 4). An update with value 0 still counts: an unmarked ACK updates
   `uet_rx_ack_ecn` with 0.

3. **Accumulators never reset by themselves.** The spec doesn't say whether
   accumulators reset after each handler call. We keep them as running totals,
   which matches the paper's handlers (they subtract a saved previous value) and
   the spec's note that algorithms should handle counter wraparound. A handler that
   wants "since the last call" values resets a signal itself with
   `uet_signal_set()`. We support `uet_signal_set()` and `uet_signal_update()`,
   which are optional in the spec.

4. **Added: DELTA and MAGNITUDE triggers.** The spec has only one kind of trigger,
   comparing the accumulator's value. Following the paper (§5.1), an optional
   `trigger_type` field adds `delta`, which compares the change since the
   trigger's last true comparison, and `magnitude`, the absolute value of that
   change. The default `raw` is the spec's behaviour.

5. **Added: a NACKed-bytes signal**, `uet_rx_nack_vend_htsim_bytes` (§3 above). It
   uses the spec's naming rule for vendor signals.

6. **Added: native units** (`-pcm_units native`), which pass picoseconds and bytes
   to handlers instead of the spec's 128 ns / 256 B units. It's only for the
   exact-match test; spec units are the default.

7. **Flow grouping differs slightly.** The spec requires all RUD connections
   between the same two NICs in the same traffic class to share one CCC
   (§3.5.8.1). We group by (group name, source NIC, destination NIC), so two
   different group names on the same NIC pair give two CCCs. This is what makes
   per-workload experiments possible. If all flows on a pair use the same group
   name, the result is the spec's. Traffic class is left out because htsim doesn't
   model traffic classes.

8. **Flows without a group get their own CCC.** By the same spec rule they would
   share one with other ungrouped flows on the same NIC pair. Keeping them
   separate preserves today's behaviour and the exact-match test.

9. **Asynchronous mode uses a fixed handler time `Th`.** The spec leaves call
   timing to the implementation, but says programs may assume they are called
   immediately (§3.6.18.2). Asynchronous mode deliberately breaks that assumption
   to study reaction delay, as in the paper (§5.3, §7.3). Synchronous mode keeps
   it.

10. **Only part of the mandatory signal set is supported.** The spec requires
    implementations to support all mandatory signals (§3.6.18.4). We support the
    sender-side ones listed in §3. The receiver-side, credit, load-balancing,
    telegram and service-time signals are out of scope or not modelled in htsim.
    A YAML file using an unsupported signal fails to load, which is what the spec
    requires.

11. **Not yet supported, and rejected at load:** global variables (§3.6.18.6.2;
    planned after shared CCCs), telegrams (§3.6.18.6.3), control packets
    (§3.6.18.6.4), load-balancing algorithms (§3.6.18.3), and the optional controls
    other than `uet_set_cwnd`. Each gets its own note here once it's added.

## 8. Commit plan

Each commit builds, passes `ctest`, and leaves non-PCM behaviour unchanged.
Commits that touch `UecSrc` or `main_uec.cpp` also run `commit_check.sh` (about
11 min, in the background). A commit that introduces a difference from the spec
adds its §7 entry and code comment in the same commit.

**Done**
- [x] Build, unit tests, smoke run, `commit_check.sh` baseline (all 8 pass on `65ea315`)
- [x] `pcm` branch; read spec §3.6.18 and the paper; agreed scope and §7 decisions

**Commits**
1. [ ] **docs:** this plan
2. [ ] **build:** yaml-cpp via FetchContent (pinned); empty `pcm/` library target
3. [ ] **pcm: YAML loader.** Reads and checks the §3.6.18.8.5 format plus our
   `op_val` and `trigger_type` additions; rejects unsupported signals, controls and
   features. Unit tests.
4. [ ] **pcm: accumulators and triggers.** All accumulators (including several per
   signal) and the full trigger behaviour from §3.6.18.5 plus §7 items 1, 2 and 4.
   Unit tests.
5. [ ] **pcm: calling handlers.** The `uet_pcm.h` API; tracking which CCC is being
   served; matching YAML names; frozen values and applying controls; synchronous
   calls; ERROR state; counters. Unit tests that feed events directly, with no
   network.
6. [ ] **pcm: time triggers and `invoke_deadline`.** Timers on the event list;
   unit tests.
7. [ ] **uec: connect PCM to the sender.** `PCM` enum; ACK/NACK/send/timeout calls
   into per-flow CCCs; options `-pcm`, `-pcm_param`, `-pcm_units`; counters on the
   flow finish line and in the summary; `commit_check.sh`.
8. [ ] **pcm: `dctcp_legacy` and the exact-match test**, as ctest or a script.
9. [ ] **pcm: `dctcp`.** Textbook DCTCP, a unit test of α, and incast/permutation
   sanity runs.
10. [ ] **datacenter: flow groups.** `ccc <group>` in the connection matrix,
    grouping by (group, src, dst), `-pcm_ccc group=file`; unit tests.
11. [ ] **uec: shared group window.** Group bytes-in-flight, taking turns between
    flows, group totals for signals; tests that two grouped flows share one window
    fairly; `commit_check.sh`.
12. [ ] **pcm: asynchronous mode.** `-pcm_exec async -pcm_handler_time_ns`; tests
    that triggers arriving during a call are combined.
13. [ ] **datacenter: `validate_pcm.txt` and its baseline**, added to `commit_check.sh`

**Later (design only now)**
- [ ] Global variables shared per NIC
- [ ] Rate limiting in the sender for `uet_set_send_rate` (DCQCN, TIMELY; Swift at
      windows below one packet)
- [ ] NSCC and Swift; compare NSCC with the built-in version and Swift with paper Fig. 6
- [ ] Set `_residency_time` in ACKs → `uet_rx_ack_svc`
- [ ] Event-drop model for overloaded NICs (§4.4)
