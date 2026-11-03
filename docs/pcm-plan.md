# PCM in htsim (phase 1)

How Programmable Congestion Management (PCM) is added to `htsim_uec`, and where
it differs from the spec. Phases 2–4 are in [roadmap.md](roadmap.md); trigger
details are in [pcm-triggers.md](pcm-triggers.md).

References:
- **Spec:** UE Specification v1.1, §3.6.18 *Programmable Congestion Management*
  (pp. 516–547). "§" refers to the spec unless it says "paper".
- **Paper:** Khalilov et al., *PCM: Programmable Congestion Management for Terabit
  RDMA NICs*, arXiv:2610.13130 (Oct 2026). The authors built PCM into UET-htsim
  (paper §6.1), but their code isn't available.

| Term | Meaning |
|---|---|
| FEP | Fabric endpoint, i.e. a NIC |
| PDC | Packet delivery context, i.e. a connection. One htsim `UecSrc` is one PDC. |
| RUD / ROD | Reliable unordered / ordered delivery (PDC modes) |
| CCC | Congestion control context: the congestion state (window, signals, algorithm state) that one or more PDCs share |
| TC | Traffic class |
| NSCC / RCCC | UET's built-in sender-based / receiver-credit-based congestion control |

## 1. Scope

A PCM algorithm is a YAML file declaring what it uses (signals, triggers,
controls, variables, parameters; §3.6.18.8.5) plus a C handler written against
the spec's `uet_*` API. The simulator delivers signals, checks triggers, calls
the handler, and applies the window it sets. Phase 1 adds this runtime, flow
groups that share one CCC, an asynchronous handler mode, and DCTCP to verify it
all.

- **Sender-side congestion control only:** no receiver-side or credit PCM, load
  balancing, telegrams or CSIG. Evaluation runs use `-sender_cc_only`.
- **The spec is a guide, not law.** We follow it by default, leave out features
  nothing here would use, and pick one behaviour where it's unclear. Every
  difference is listed in §7 and commented at the code.
- **Non-PCM behaviour is unchanged.** `commit_check.sh` passes with identical
  outputs, and `pcm_exact_match.sh` shows PCM ports of built-in algorithms send
  exactly like the originals.

## 2. The PCM model, from the spec

| Concept | Spec | Summary |
|---|---|---|
| YAML file | §3.6.18.8.5 | Keys `cc_algorithm`, `signal`, `control`, `variable`, `parameter` (and `global_variable`, `tgm`, not supported). `cc_algorithm.name` names the handler. |
| Signals | §3.6.18.4, Table 3-109 | Named `uet_<rx\|tx\|time>_<pkt>_<name>`, e.g. `uet_rx_ack_bytes`. A file using an unsupported signal must fail to load. |
| Accumulators | Tables 3-106, 3-107 | How updates combine: `uet_op_sum/max/min/last`, optional `uet_op_ewma`. One signal may have several. |
| Snapshot | §3.6.18.2 | Signal values are frozen while the handler runs. |
| Triggers | §3.6.18.5 | Decide when the handler runs: a comparison, a `(count, time)` threshold, `arm_mode`, `action` (INVOKE or RECORD) and `invoke_deadline`. The handler reads which fired with `uet_trigger_mask()`. See [pcm-triggers.md](pcm-triggers.md). |
| Controls | §3.6.18.6.1 | What the handler sets: `uet_set_cwnd` (mandatory); rate and ACK controls are optional. |
| Variables, parameters | §3.6.18.6.2, §3.6.18.8.5 | Private per-CCC state; constants per algorithm instance (its "personality"). |
| Runtime model | §3.6.18.9 | At most one handler call at a time per CCC. RX/TX events may be skipped; time events may be late. |
| Errors | §3.6.18.7 | `uet_ccc_fatal()` or invalid API use puts the CCC into the ERROR state. |
| Units | Tables 3-109, 3-113 | Time in 128 ns, ACK bytes in 256 B. |

From the paper: synchronous and asynchronous execution (§5.3), the
reaction-delay experiment (§7.3, Fig. 6: Swift, 8:1 incast, handler delay
`N·Th` with `Th ≈ 1.1 µs`), and its htsim test setup, which matches htsim's
defaults (1024-host fat tree, 100 Gbit/s, 4150 B packets).

## 3. Signals and where they come from

| Signal | Source in `UecSrc` | Notes |
|---|---|---|
| `uet_rx_ack`, `_ack_bytes`, `_ack_ecn`, `_ack_probe` | `processAck` | `_ack_bytes` = newly acknowledged bytes |
| `uet_rx_ack_count` | `processAck` | packets newly acknowledged (cumulative + SACK) |
| `uet_rx_ack_rtt` | `processAck` | only when the packet's send record is valid |
| `uet_rx_nack`, `_nack_ecn`, `_nack_rtt`, `_nack_last_hop_trim` | `processNack` | |
| `uet_rx_nack_trim` | `processNack` | always 1: the sink only sends NACKs for trimmed packets |
| `uet_rx_nack_vend_htsim_bytes` | `processNack` | our vendor signal (§7 item 12) |
| `uet_tx_req`, `_req_bytes` | `sendNewPacket`, `sendRtxPacket` | |
| `uet_tx_rto` | `rtxTimerExpired` | |
| `uet_tx_cwnd`, `_inflight_bytes`, `_backlog_bytes` | every event | current values (§7 item 11); group totals for a flow group |
| `uet_time_current`, `_last_update` | timer events, and before each call | §7 item 10 |

Not supported: `uet_rx_ack_svc` (the ACK field is never set), `uet_tx_send_rate`
(needs rate limiting, phase 2), and receiver-side, credit, telegram and
load-balancing signals. An ACK carries one ECN bit; since marked packets are
ACKed at once, counting marked ACKs approximates the fraction of marked bytes.

## 4. Design

### 4.1 Files

```
htsim/sim/pcm/
  uet_pcm.h              the handler API (spec names and signatures) and the
                         PCM_ALGORITHM / PCM_SIGNAL / ... / PCM_HANDLER macros
  pcm_signals.{h,cpp}    every spec signal with its direction, unit and support
  pcm_spec.{h,cpp}       YAML loader and checks; -pcm_param overrides
  pcm_accumulator.{h,cpp}, pcm_trigger.{h,cpp}
                         one accumulated value; one trigger's state machine
  pcm_signal_state.{h,cpp}  all of a CCC's accumulators and triggers
  pcm_runtime.{h,cpp}    PcmAlgorithm (a YAML file matched to its handler) and
                         PcmCcc (one CCC: calls the handler, timers, ERROR state)
  pcm_registry.{h,cpp}   compiled handlers by name, and the macros each uses
  algorithms/            constant, dctcp_legacy, dctcp (.yaml + .cpp)
  tests/                 googletest unit tests
htsim/sim/uec.{h,cpp}    PCM in the sender: events, timers, flow groups
htsim/sim/datacenter/    options in main_uec.cpp; pcm_exact_match.sh,
                         pcm_groups_check.sh, validate_pcm.txt
```

The `pcm/` library doesn't depend on htsim. When a CCC needs to act at a given
time, it asks through a scheduling callback, and the sender calls
`PcmCcc::onTimer()` then.

### 4.2 Handlers

```cpp
#include "uet_pcm.h"
PCM_ALGORITHM(dctcp);         // same name as cc_algorithm.name in the YAML
PCM_SIGNAL(T_ACK);            // the YAML macros this handler uses
PCM_VARIABLE(V_ALPHA);
PCM_PARAMETER_FLOAT(P_G);
PCM_HANDLER(dctcp) {
    uet_trigmask_t fired;
    uet_trigger_mask(&fired);
    if (fired & T_ACK) { /* uet_signal_get(...), uet_set_cwnd(...) */ }
}
```

- **Handlers are compiled in** and register under their name at start-up; a
  YAML file's `cc_algorithm.name` selects one. They're built as an object
  library (`htsim_pcm_algorithms`) so the linker keeps them.
- **The `uet_*` functions act on the CCC being served.** The simulator is
  single-threaded, so the runtime records which CCC that is before each call.
- **Macros are matched to the YAML file at load time**, by name. Loading fails if
  the handler uses a name the file doesn't define, or defines with a different
  kind or type. One handler can be loaded with several YAML files.
- **A signal macro is both an index and a mask bit**, as the spec uses it both
  ways (`uet_signal_get(S)`, `fired & T`). Macros only work inside a handler.
- **Parameters are read at run time**, so `-pcm_param` and per-group parameters
  need no recompile.

### 4.3 Calling the handler

1. All of an event's signal updates are applied together, then the triggers of
   the updated signals are checked.
2. If an INVOKE trigger fires (immediately, or at its `invoke_deadline`), the
   handler gets every trigger fired since its last call, and the signal values
   frozen.
3. Its signal writes and window take effect in the order it made them, when it
   returns.

`uet_ccc_fatal()` or invalid API use (§7 item 22) cancels that call's changes and
stops the CCC. The runtime doesn't clamp the window: bounds are the algorithm's
job.

### 4.4 Runtime behaviour

- **Timers.** A trigger on a `uet_time_*` signal, a pending `invoke_deadline` or
  a busy asynchronous call makes the CCC ask for a timer event when it's due.
  PCM timer events don't count as traffic, so they can't keep a finished
  simulation running.
- **Asynchronous mode** (`-pcm_handler_time_ns T`, default 0 = synchronous). A
  call runs the handler on the values at its start; its changes take effect T
  later. Meanwhile events still update signals, and the calls they ask for are
  combined into one that starts when the busy call finishes. A first sweep with
  PCM `dctcp` on the 16-to-1 incast, T from 0 to 145.5 µs: retransmissions grow
  from 1,283 to 56,115, the slowest flow from 2480 to 2733 µs, and handler calls
  drop from 6,677 to 271.
- **Units** (`-pcm_units spec|native`). Spec units (the default) truncate values
  to 128 ns and 256 B; native units pass picoseconds and bytes, for exact ports.
- **No skipped events**, which the spec allows. All events pass through one
  function (`PcmSignalState::applyEvent`) where a drop model could be added.
- **Counters** per CCC (events, calls, deadline calls, timer events, window
  updates, requests while busy, error) are printed on each PCM flow's finish
  line, with totals on a `PCM:` summary line.

### 4.5 Flow groups

In the spec a CCC is shared by a group of PDCs (§3.6, §3.5.8.1). Here, flows
with the same group name, source NIC and destination NIC share one CCC, held by
a `UecPcmGroup` in `uec.cpp`:

```
0->15 start 0  size 2000000 ccc ai        # CCC (ai, 0, 15)
0->15 start 50 size 2000000 ccc ai        # same CCC, shares the window
0->15 start 0  size 1000000 ccc storage   # CCC (storage, 0, 15)
2->15 start 0  size 2000000               # no group: its own CCC
```

- **Algorithms.** `-pcm FILE` applies to ungrouped flows and to groups without
  their own; `-pcm_ccc GROUP=FILE` gives a group its own;
  `-pcm_param GROUP:NAME=VALUE` gives it its own parameters. The `ccc` token is
  ignored without PCM, so a matrix also runs with the built-in algorithms.
- **Sharing.** The group has one window, set of signals, variables, triggers and
  timer. Each flow keeps its own reliability state (sequence numbers, SACK,
  retransmissions, timeout). The window signals are the group's: its window and
  totals over its flows. The CCC starts with the first flow, using that flow's
  window unless the YAML sets one.
- **Sending.** A flow sends only if the group's bytes in flight plus the packet
  fit the group's window (`UecSrc::can_send_NSCC`). When room frees up, the
  flows take turns, one packet each (`UecPcmGroup::kick`).
- **Ungrouped flows** keep their own CCC and run the same code as before.
- **Checked by** `pcm_groups_check.sh`: two 2 MB flows from NIC 0 to NIC 15 with
  10-packet windows finish at 728 / 745 µs ungrouped and at 1569 / 1570 µs in one
  group, i.e. half the throughput each, finishing together.

### 4.6 The sender

- **Options:** `-pcm FILE` (implies `-sender_cc_algo pcm`), `-pcm_ccc`,
  `-pcm_param`, `-pcm_units`, `-pcm_handler_time_ns`. The loaded algorithms,
  non-spec features they use, and warnings are printed at start.
- **A flow's CCC is created when it starts** (`startConnection`), after
  `initNscc()` has set its base RTT, BDP and window.
- **Events** go to the CCC where the built-in algorithms update their window
  (ACK, NACK), after sending a packet, and on a retransmit timeout. A window set
  by the handler becomes the flow's (or group's) window.
- **ERROR state:** the flow stops sending (§7 item 23).

## 5. DCTCP and verification

| Algorithm | What it is |
|---|---|
| `constant` | A fixed window, like `-sender_cc_algo constant`. The smallest example. |
| `dctcp_legacy` | A line-for-line port of htsim's built-in DCTCP (no α; cuts a third of the ACKed bytes per marked ACK). Native units. |
| `dctcp` | DCTCP as Linux implements it. Once per window of data, α ← (1−g)·α + g·F, with F the fraction of packets ECN-marked or NACKed. The first mark in a window cuts it to cwnd·(1−α/2); unmarked ACKs add about one packet per RTT; a NACK halves it, at most once per window. Counts packets, so spec and native units behave the same. |

NACKs count in `dctcp`'s F and towards the end of its window because an early
version, where they didn't, never ended windows in a 128-to-1 incast (almost
nothing is ACKed there), and stalled with millions of trims.

**Exact match** (`pcm_exact_match.sh`): `constant` and `dctcp_legacy` give output
identical to `-sender_cc_algo constant` and `dctcp` on `one.cm`, `incast.cm` and
`perm_32n_32c_2MB.cm`, ignoring only PCM lines and fields. The DCTCP runs include
about 2,300 ECN cuts and 600–800 NACKs each.

**Sanity runs** (`-sender_cc_only`; mean / max flow completion time):

| Scenario | NSCC | built-in DCTCP | PCM `dctcp` |
|---|---|---|---|
| 16-to-1 incast, 2 MB each | 2179 / 2479 µs | 2273 / 2491 µs | 2251 / 2480 µs |
| 32-node permutation, 2 MB each | 498 / 733 µs | 508 / 723 µs | 549 / 778 µs |
| 127-to-1 incast, 2 MB each | 20170 / 21029 µs | 20171 / 21038 µs | 20220 / 21072 µs |

The 127-to-1 incast is limited by the receiver's link (127 × 2 MB at 100 Gbit/s
≈ 20.3 ms).

## 6. Testing

| Check | What | Time |
|---|---|---|
| `ctest` (unit) | YAML loading, accumulators, triggers, signal state, handler API, timers and asynchronous mode (fake clock), DCTCP | seconds |
| `pcm_exact_match.sh` (in `ctest`) | PCM ports match built-in algorithms exactly (§5) | ~2 s |
| `pcm_groups_check.sh` (in `ctest`) | flow groups share one window and take turns (§4.5) | <1 s |
| `validate_pcm.txt` (in `commit_check.sh`) | PCM `dctcp` on single flow, 16- and 128-to-1 incast, 32-node permutation; a flow group; asynchronous mode; each with a tail-FCT bound | ~1 s |
| `commit_check.sh` | all validation suites against the baselines on `origin/main` | ~11 min |

`commit_check.sh` skips the comparison for a suite with no baseline on
`origin/main` yet (as `validate_pcm` until this branch is merged) and says so.

## 7. Where we differ from the spec

Each item is also commented at the code. The loader records when a file uses
an addition (items 3, 12) and the simulator prints it, so a run's log shows
whether it used only spec features. The loader is also stricter than the spec
in ways that only catch mistakes: unknown or repeated keys are errors, and a
file with no `invoke` trigger gets a warning.

### Triggers

1. **Triggers compare against `op_val`.** The spec's trigger definition
   (§3.6.18.5.1) compares the accumulator against the `mode_ref` numbers that also
   set the count and time thresholds, its YAML format (§3.6.18.8.5) has no
   separate value, but its own example (§3.6.18.5.5) uses `op_val`. Read
   literally, "fire as soon as RTT exceeds 15 µs" can't be expressed, so it's
   most likely a typo. We compare `accumulator (op) op_val`; `mode_refval` only
   sets the count and time. `op_val` is required except for the wildcard `wc`.
2. **A trigger is checked only when its own signal is updated.** The spec
   (§3.6.18.5.3) checks all `uet_rx_*` triggers on every received packet, so an
   "every 8 ACKs" trigger would also count NACKs. An update with value 0 still
   counts. This matches the paper's htsim version (paper Fig. 4).
3. **Added: DELTA and MAGNITUDE triggers**, from the paper (§5.1), which its own
   handlers use (App. B). `trigger_type: delta` compares the change since the
   trigger's last true comparison, `magnitude` its absolute value; `raw` (the
   default) is the spec's.
4. **`invoke_deadline` starts when a trigger fires.** The field is "an upper bound
   on how long an invoke action may be deferred after trigger fires"
   (§3.6.18.5.1), though §3.6.18.5.3 updates the deadline "for every evaluated
   trigger". A trigger that didn't fire has nothing to defer.
5. **A deferred call happens at its deadline**, the latest time allowed, so
   triggers firing meanwhile are combined into it. An immediate trigger before
   then delivers it earlier.
6. **Triggers can't be changed at runtime.** `uet_modify_trigger()` and
   `uet_query_trigger()` aren't implemented: nothing planned needs them, and the
   spec doesn't define their `struct uet_trigger_attr`. So a `once` trigger fires
   one time only, and `arm_mode: disarmed`, which could never fire, is rejected.
7. **At most 64 signal entries per algorithm**, one bit each in the 64-bit
   trigger mask. The spec gives no limit.

### Signal values

8. **Accumulators never reset by themselves.** The spec doesn't say. Running
   totals match the paper's handlers (which subtract a saved previous value) and
   the spec's note about counter wraparound. Handlers can reset a signal with
   `uet_signal_set()`.
9. **Starting values.** The spec mentions "initial values for new CCCs"
   (§3.6.18.8.1) but has no YAML field for them. SUM, LAST and MAX start at 0
   (spec signals are unsigned); MIN starts at the largest 64-bit integer
   (`UET_SIGNAL_MIN_EMPTY`, meaning "no sample yet"; setting a MIN to it restarts
   it); EWMA starts from its first sample.
10. **Handlers always read the current time.** The spec updates `uet_time_*` only
    on timer events, so an ACK handler would read the last timer event's time.
    Before each call, `uet_time_current` gets the current time and
    `uet_time_last_update` the time of the last signal update. Their triggers are
    still checked only on timer events.
11. **The window, bytes in flight and backlog come with every event**, not only
    with sent packets as for other `uet_tx_*` signals, so a handler called for an
    ACK sees them after the ACK.
12. **Added: `uet_rx_nack_vend_htsim_bytes`**, the bytes in a NACKed packet, which
    the spec lacks and `dctcp_legacy` needs. Named by the spec's vendor rule
    (§3.6.18.4.6).
13. **Added: native units** (`-pcm_units native`), for exact ports. Spec units are
    the default; values are truncated to whole units (a 4150 B ACK is 16 × 256 B),
    as hardware would count; the spec doesn't say how to round.
14. **Only the sender-side signals in §3 are supported.** The spec requires all
    mandatory signals (§3.6.18.4); the others are out of scope or not modelled.
    Using one fails to load, as the spec requires for unsupported signals.

### YAML format

15. **`accumulator` is required.** The format calls it optional with a default
    from a table that doesn't mark any default. Both spellings the spec uses
    (`uet_op_sum`, `uet_sum`) are accepted.
16. **`scope` may be left out** (default `CCC`, the only value we accept; `EV` is
    for load balancing), and a section may be a single entry instead of a list,
    because both of the spec's examples do so. `invoke_deadline` is required as
    the format says; the one example without it ends in `...`.
17. **Control names are the matching signal names**, e.g. `uet_tx_cwnd` for
    `uet_set_cwnd()`. The format refers to a table of control names ("Table
    ZZZ") that the spec doesn't include.
18. **Fields we have no use for are optional, checked and ignored:**
    `description` (algorithm and parameter), `max_ack_coalescing` (the spec
    doesn't say what to do with it), a control's `macro` (control functions take
    no index), a trigger's `mode` (`count_or_time` is its only value), and the
    `*_min_bitwidth` fields (we always meet 64 bits).
19. **`initial_value: 0` keeps the simulator's starting window** (from the BDP, or
    `-cwnd`), since 0 is never a valid window. Other values replace it.

### Handler API and errors

20. **`uet_trigger_mask()` takes a pointer.** The spec gives it no signature; its
    text says it "returns" the mask, but its example passes a pointer
    (§3.6.18.5.5). The pointer form matches the other getters.
21. **When the handler's changes take effect.** The spec freezes signals during a
    call (§3.6.18.2) and executes control operations "in issued order"
    (§3.6.18.9), but doesn't say when a handler's own writes apply. Its signal
    writes show in its frozen copy at once and reach the CCC, with its window, in
    order when it returns. Local variables change at once.
22. **Invalid operations** (values in `PcmImplementationError`): an index out of
    range; a variable function of the wrong `dtype`; `uet_set_cwnd` without a
    `uet_tx_cwnd` control, or with 0; a null pointer. The spec leaves the list
    open (§3.6.18.7). Using the API outside a handler is a simulator bug and
    aborts.
23. **The ERROR state only stops new packets**, including retransmissions. The
    spec also drops in-flight packets and puts the PDCs in error
    (§3.6.18.7.2.1). Unless all its data was sent, the flow never finishes; the
    error is printed and counted on the `PCM:` line.
24. **Asynchronous mode uses a fixed handler time**, deliberately breaking the
    spec's note that programs may assume immediate calls (§3.6.18.2), to study
    reaction delay as the paper does. A `uet_signal_set` takes effect when the
    call finishes, replacing updates that arrived meanwhile; handlers should keep
    previous values in variables instead.

### Flow groups

25. **Grouping is by (group, source NIC, destination NIC).** The spec puts all RUD
    PDCs between two NICs in one traffic class into one CCC (§3.5.8.1). Group
    names can split them, for per-workload experiments; with one name per pair
    the result is the spec's. Traffic class is left out: htsim has none.
26. **Ungrouped flows get their own CCC**, as today, instead of sharing one per
    NIC pair.
27. **Flows sharing a CCC take turns, one packet each.** The spec doesn't say how
    a CCC's PDCs share its window (§3.6).

### Not supported

28. Global variables (§3.6.18.6.2; and the `uet_update_variable_*` functions,
    which only update them), telegrams (§3.6.18.6.3), control packets
    (§3.6.18.6.4), load-balancing algorithms (§3.6.18.3), and the optional
    controls other than `uet_set_cwnd`. Files using them fail to load.

## 8. Commits

Each commit builds and passes `ctest`; those touching the sender also pass
`commit_check.sh`. A commit that adds a difference from the spec adds its §7 item
and code comment.

| # | Commit | Content |
|---|---|---|
| 1 | `df319bd` | this plan |
| 2 | `53a9916` | yaml-cpp 0.8.0 via FetchContent |
| 3 | `c9cb9ca` | YAML loader |
| 4 | `00a1624` | accumulators and the trigger state machine |
| 5 | `c11a757` | handler API and calling handlers |
| — | `8cdfe00` | removing spec features with no use here |
| 6 | `26a6cfd` | timers: time triggers and `invoke_deadline` |
| 7 | `addf703` | PCM in the sender; options; `constant` |
| — | `fae5bae` | AGENTS.md, CLAUDE.md, roadmap |
| 8 | `bb67925` | `dctcp_legacy`, `dctcp`, exact-match test |
| 9 | `6533e96` | flow groups sharing one CCC and window |
| 10 | `d7ad526` | asynchronous handler calls |
| 11 | this commit | `validate_pcm.txt` in `commit_check.sh`; docs consolidated |

**Later:** per-NIC global variables; rate limiting in the sender
(`uet_set_send_rate`, for DCQCN, TIMELY and Swift); NSCC and Swift on PCM;
`uet_rx_ack_svc`; an event-drop model. See [roadmap.md](roadmap.md).
