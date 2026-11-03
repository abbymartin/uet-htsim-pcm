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
| Triggers | §3.6.18.5 | Decide when the handler is called. Each watches one accumulator, with a comparison (`== != > >= < <= *`), a `(count, time)` threshold, `arm_mode` (DISARMED/ONCE/CONTINUOUS; DISARMED isn't supported, see §7 item 20), `action` (INVOKE calls the handler, RECORD only notes that the trigger fired), and `invoke_deadline`. The handler reads which triggers fired with `uet_trigger_mask()`. (The spec's `uet_modify_trigger()` for changing triggers at runtime is not implemented; see §7.) |
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
| `uet_rx_nack`, `_nack_ecn`, `_nack_rtt`, `_nack_trim`, `_nack_last_hop_trim` | `processNack`; the sink only sends NACKs for trimmed packets, so `_nack_trim` is always 1 | ✅ |
| `uet_tx_req`, `uet_tx_req_bytes` | `sendNewPacket` / `sendRtxPacket` | ✅ |
| `uet_tx_rto` | `rtxTimerExpired` | ✅ |
| `uet_tx_cwnd`, `uet_tx_inflight_bytes`, `uet_tx_backlog_bytes` | `_cwnd`, `_in_flight`, `_backlog + _rtx_backlog`; reported with every event (§7 item 25) | ✅ |
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
                      uet_set_cwnd, uet_get/set_variable_*, uet_trigger_mask,
                      uet_trigger_drop_mask, uet_ccc_fatal; and the PCM_ALGORITHM /
                      PCM_SIGNAL / ... / PCM_HANDLER macros for declaring a handler
  pcm_signals.{h,cpp} every spec signal (Tables 3-109, 3-112) with its direction and
                      unit, whether we support it, and our vendor signal
  pcm_spec.{h,cpp}    YAML loader: reads and checks an algorithm's YAML file; rejects
                      anything we don't support
  pcm_accumulator.{h,cpp}  one accumulated signal value (sum/max/min/last/ewma)
  pcm_trigger.{h,cpp}      one trigger's state machine (§3.6.18.5)
  pcm_signal_state.{h,cpp} all accumulators and triggers of one CCC; applies one
                      event's updates together, then checks the triggers of the
                      updated signals
  pcm_runtime.{h,cpp} class PcmCcc, one per CCC (§4.5): signal state, snapshot,
                      calling the handler, applying controls, ERROR state,
                      counters. When it needs to act at a given time (time
                      triggers, invoke_deadline, asynchronous completion), it asks
                      the datapath through a scheduling callback, and the datapath
                      calls PcmCcc::onTimer() then. The pcm/ library therefore
                      doesn't depend on htsim, and UecSrc's timer code stays
                      untouched.
  pcm_registry.{h,cpp} the compiled handlers by name, and the YAML macros each one
                      uses; the objects behind PCM_SIGNAL / PCM_VARIABLE /
                      PCM_PARAMETER_*
  algorithms/
    constant.{yaml,cpp}       fixed window, like -sender_cc_algo constant; the smallest example
    dctcp_legacy.{yaml,cpp}   exact port of the existing DCTCP (for the exact-match test)
    dctcp.{yaml,cpp}          textbook DCTCP
  tests/pcm_*_test.cpp        googletest unit tests
```

### 4.2 Writing handlers like the spec does

Handlers are plain functions with no arguments, as in the spec's examples. A
handler file includes only `uet_pcm.h`:

```cpp
// algorithms/dctcp.cpp
#include "uet_pcm.h"

PCM_ALGORITHM(dctcp);         // same name as cc_algorithm.name in the YAML
PCM_SIGNAL(T_ACK);            // the YAML macros this handler uses
PCM_SIGNAL(S_ACK_BYTES);
PCM_VARIABLE(V_ALPHA);
PCM_PARAMETER_FLOAT(P_G);

PCM_HANDLER(dctcp) {
    uet_trigmask_t fired = uet_trigger_mask();
    if (fired & T_ACK) {
        uet_int_t acked = uet_signal_get(S_ACK_BYTES);
        /* ... */
        uet_set_cwnd(cwnd);
    }
}
```

- **The handler runs for one CCC at a time.** The simulator is single-threaded.
  Before each call, the runtime records which CCC is being served, and the
  `uet_*` functions act on it. Handlers therefore read like the spec's C code,
  and could later be compiled for real hardware.
- **How loading works.** The spec leaves it to the implementation (§3.6.18.8.1).
  Here, handlers are compiled into the simulator. `PCM_ALGORITHM` and
  `PCM_HANDLER` register the handler under its name at program start, and a YAML
  file's `cc_algorithm.name` selects it. One handler file holds one algorithm.
- **Names** such as `T_ACK`, `S_ACK_BYTES` and `V_ALPHA` come from the `macro:`
  fields in the YAML, and the handler declares the ones it uses. When a YAML file
  is loaded, each name is matched to the file's entry. Loading fails if the
  handler uses a name the file doesn't define, or defines as a different kind
  (signal / variable / parameter) or type. Names the file defines but the handler
  doesn't use give a warning. This avoids generating code during the build, and
  lets one handler be loaded with several YAML files whose entries are in
  different orders.
- **A signal name works both as an index and as a mask bit**, because the spec
  uses it both ways: `uet_signal_get(S_RTT)` takes an index, while
  `fired & T_ACK` tests a bit (§3.6.18.5.5). The names are
  small C++ objects that convert to the index and combine with masks as the bit.
  Using one outside a running handler aborts the program.
- **Parameters** are read from the loaded YAML when the program runs, not compiled
  in as constants. One compiled handler can then run with different parameter
  values from different YAML files, or with `-pcm_param NAME=VALUE`.
- **Linking.** Because handlers register themselves at program start, their object
  files must be linked into the simulator even though nothing calls them
  directly. A static library would drop them, so the algorithm files
  (`pcm/algorithms/*.cpp`) are built as an object library,
  `htsim_pcm_algorithms`, linked into `htsim_uec`.

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
- **Timers.** A trigger on a `uet_time_*` signal, or a pending `invoke_deadline`,
  makes the CCC ask for an `onTimer()` call at the time it's due. A timer event
  updates the `uet_time_*` signals and checks their triggers, then calls the
  handler if a deadline has been reached. Triggers on other signals check their
  time condition when their signal is next updated. `invoke_deadline` follows
  §3.6.18.5.3: the CCC's deadline is `min(current deadline, now +
  trigger.invoke_deadline)`.
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

- **Command line.** `PCM` is added to `Sender_CC`. `-pcm FILE` loads an algorithm
  and implies `-sender_cc_algo pcm`. `-pcm_param NAME=VALUE` (repeatable) changes
  a parameter, and `-pcm_units spec|native` chooses the units. The loaded
  algorithm, any non-spec features it uses, and warnings are printed at start.
- **The CCC is created when the flow starts** (`startConnection`), so its timers
  count from the flow's start. `initNscc()` still runs for PCM flows, to set the
  base RTT, BDP and window. A `uet_tx_cwnd` control with a non-zero
  `initial_value` replaces that window; 0 keeps it (§7 item 26). The paper's
  incast study (Fig. 2a) shows why the initial window matters.
- **Events.** At each point in the §3 table, the sender collects that packet's
  signal values, in the simulator's units, and passes them to the flow's CCC:
  ACKs and NACKs where the existing algorithms' window update is called, sent
  packets at the end of `sendNewPacket` / `sendRtxPacket`, and retransmit
  timeouts in `rtxTimerExpired`. Values are converted to the CCC's units, and
  the window, bytes in flight and backlog are added to every event (§7 item
  25). If the handler set a window, it becomes `UecSrc::_cwnd`.
- **Timers.** Each PCM flow has a small event source that delivers the CCC's
  timer requests. It isn't traffic, so PCM timers can't keep a finished
  simulation running. After a timer event that changed the window, the sender
  tries to send.
- **ERROR state.** A CCC in the ERROR state admits no new packets (§7 item 27).
- **Output.** Each PCM flow's finish line adds `pcm_events`, `pcm_calls`,
  `pcm_deadline_calls`, `pcm_timer_events`, `pcm_cwnd_updates` and `pcm_error`.
  A `PCM:` summary line with the totals follows the end-of-run packet counts.
- **Non-PCM runs are unchanged.** All new code runs only for PCM flows: non-PCM
  flows have no CCC, and their output has no PCM fields. `commit_check.sh` passes.

## 5. DCTCP for verification

| Algorithm | Purpose | Notes |
|---|---|---|
| `dctcp_legacy` | Shows the PCM runtime changes nothing | Called on every ACK and NACK. Integer arithmetic copied from `UecSrc::updateCwndOnAck_DCTCP` / `updateCwndOnNack_DCTCP`. Reads the current window from `uet_tx_cwnd`. Needs native units. |
| `dctcp` | The real algorithm [Alizadeh 2010], as Linux implements it | Once per window of data: α ← (1−g)·α + g·F, where F is the fraction of the window's packets that were ECN-marked or NACKed. The first mark in a window cuts it to cwnd·(1−α/2); unmarked ACKs grow it by about one packet per RTT; a NACK halves it, at most once per window. Counts packets (`uet_rx_ack_count`), so it behaves the same in spec and native units. Parameters `P_G` (1/16) and `P_MTU`. Switch ECN marking is on by default (thresholds 20% / 80% of the BDP; `-ecn kmin kmax` to change). |

**Why NACKs count in `dctcp`.** DCTCP has no trimming. In an early version,
NACKs only cut the window and didn't move the window boundary forward. In a
128-to-1 incast almost nothing is ACKed, so windows never ended, the window
stayed at half its starting size, and the run produced millions of trims.
Counting NACKed packets as seen and marked fixes this. A unit test covers it
(`OnlyNacksStillShrinkTheWindow`).

**Exact-match test:** `datacenter/pcm_exact_match.sh`, also run by `ctest` as
`pcm_exact_match`. It runs each PCM port next to its built-in original
(`constant` vs `-pcm constant.yaml`; `dctcp` vs `-pcm dctcp_legacy.yaml
-pcm_units native`) on `one.cm`, `incast.cm` and `perm_32n_32c_2MB.cm` (with
`leaf_spine_tiny.topo`), and diffs the outputs. It ignores only what one side
alone prints: `PCM:` lines, PCM fields on finish lines, the algorithm choice,
and the built-in DCTCP's per-ACK `DCTCP start` line. All six comparisons are
identical. The DCTCP runs include about 2,300 ECN cuts and 600–800 NACKs each.

**Sanity runs** (`-sender_cc_only`; mean / max flow completion time):

| Scenario | NSCC | built-in DCTCP | PCM `dctcp` |
|---|---|---|---|
| 16-to-1 incast, 2 MB each | 2179 / 2479 µs | 2273 / 2491 µs | 2251 / 2480 µs |
| 32-node permutation, 2 MB each | 498 / 733 µs | 508 / 723 µs | 549 / 778 µs |
| 127-to-1 incast, 2 MB each | 20170 / 21029 µs | 20171 / 21038 µs | 20220 / 21072 µs |

The 127-to-1 incast is limited by the receiver's link: 127 × 2 MB at 100 Gbit/s
is about 20.3 ms.

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
algorithm's YAML uses one of our additions (items 4–5), the loader records it,
and the simulator will print it, so experiment logs show whether a run used only
spec features.

The loader is also stricter than the spec in ways that only catch mistakes:
unknown or repeated keys are errors, and a file whose handler can never be called
(no armed `invoke` trigger) gets a warning.

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
   which are optional in the spec. To restart a MIN accumulator, set it back to
   its starting value, the largest 64-bit integer (item 16).

4. **Added: DELTA and MAGNITUDE triggers, taken from the paper.** The spec has
   only one kind of trigger, comparing the accumulator's value. The paper's PCM
   design (§5.1) has three trigger modes: RAW compares the accumulated value,
   DELTA compares "the change since that trigger's last satisfied check", and
   MAGNITUDE compares "the absolute change from the same baseline". The paper's
   handlers use them: the MP-RDMA example's YAML (paper App. B, Listing 4) uses
   `trigger_type: DELTA` with `>= 1` to react to every new ACK, and its EQDS
   receiver uses MAGNITUDE. We include them so handlers written in the paper's
   style can be expressed. An optional `trigger_type` field selects `raw` (the
   default, i.e. the spec's behaviour), `delta` or `magnitude`.

5. **Added: a NACKed-bytes signal**, `uet_rx_nack_vend_htsim_bytes` (§3 above). It
   uses the spec's naming rule for vendor signals.

6. **Added: native units** (`-pcm_units native`), which pass picoseconds and bytes
   to handlers instead of the spec's 128 ns / 256 B units. It's only for the
   exact-match test; spec units are the default. In spec units, values are
   truncated to whole units (e.g. a 4150 B ACK counts as 16 × 256 B), as hardware
   counting in those units would; the spec doesn't say how to round.

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

10. **`accumulator` is required on every signal.** The spec's YAML format marks
    it as optional, with a default "see Table YYY". But the tables it points to
    (3-110, 3-111) only list which accumulators are mandatory or allowed per
    signal, not which is the default. So there's nothing to default to, and we
    require it. Both spellings the spec uses are accepted: `uet_op_sum` (Table
    3-106) and `uet_sum` (the spec's own examples).

11. **A signal's `scope` may be left out, and a section may be a single entry.**
    The YAML format (§3.6.18.8.5) marks optional fields with square brackets,
    and `scope` isn't bracketed, so it's formally required. But both of the
    spec's signal examples (§3.6.18.5.5, and the timer example in §3.6.18.8.5)
    leave it out. We let it be omitted, defaulting to `CCC`. Its other value,
    `EV`, is only for load balancing, which we don't support, so `CCC` is the only
    value we accept anyway. Similarly, the format describes each section as a
    list ("sequence node"), but both examples write a signal as a single entry
    without a list, so both forms are accepted.

    The trigger field `invoke_deadline` is required, as the format says. It's
    missing from the §3.6.18.5.5 example, but that example ends with `...`, i.e.
    it leaves fields out for brevity. The one complete trigger example
    (§3.6.18.8.5) includes it.

12. **Control names are the matching signal names.** The YAML format says
    control names come from a table ("see Table ZZZ") that the spec doesn't
    include. Each control function (Tables 3-118, 3-119) has a signal in
    Table 3-109 for the same quantity, so we use that signal's name. For example,
    the control set by `uet_set_cwnd()` is named `uet_tx_cwnd`.

13. **Fields we have no use for are optional and ignored.** The YAML format
    requires these, but nothing in the simulator would use them, so they may be
    left out. When present they are checked, then ignored:
    - `cc_algorithm.description` and parameter `description`: documentation only.
    - `cc_algorithm.max_ack_coalescing` ("max number of ACKs to be coalesced"):
      the spec doesn't say what an implementation should do with it. Checked to
      be a number.
    - a control's `macro`: the control functions take no index
      (`uet_set_cwnd(value)`), so the macro is never used. Checked to be a C
      identifier.
    - a trigger's `mode`: `count_or_time` is the only mode the spec defines, so
      the field carries no information. Any other value is rejected.
    - `uint_min_bitwidth`, `int_min_bitwidth`, `float_min_bitwidth`: checked to be
      at most 64, which the simulator always meets with its 64-bit integers and
      floats.

14. **Only part of the mandatory signal set is supported.** The spec requires
    implementations to support all mandatory signals (§3.6.18.4). We support the
    sender-side ones listed in §3. The receiver-side, credit, load-balancing,
    telegram and service-time signals are out of scope or not modelled in htsim.
    A YAML file using an unsupported signal fails to load, which is what the spec
    requires.

15. **Not yet supported, and rejected at load:** global variables (§3.6.18.6.2;
    planned after shared CCCs; the `uet_update_variable_*` functions, which only
    update global variables, are left out until then), telegrams (§3.6.18.6.3), control packets
    (§3.6.18.6.4), load-balancing algorithms (§3.6.18.3), and the optional controls
    other than `uet_set_cwnd`. Each gets its own note here once it's added.

16. **Signal values before the first update.** The spec says a loaded algorithm
    has "initial values for new CCCs" for its signals (§3.6.18.8.1), but the YAML
    format has no field for them. Each accumulator starts at the value that has
    no effect on its operation:
    - SUM, LAST and MAX start at 0. Spec signals are unsigned, so 0 is below
      every sample.
    - MIN starts at the largest 64-bit integer. A handler that reads this value
      knows no sample has arrived yet, e.g. no RTT measured before the first ACK.
      It should check for this before doing arithmetic with it.
    - EWMA reads 0 until its first update, which is stored as it is. Table
      3-107's formula needs a previous average, and there is none yet.

17. **`invoke_deadline` applies when a trigger fires.** The trigger definition
    says it is an "upper bound on how long an invoke action may be deferred after
    trigger fires" (§3.6.18.5.1). The evaluation rules say the CCC's deadline is
    updated "for every evaluated trigger" (§3.6.18.5.3). We update it only for
    INVOKE triggers that fired, since a trigger that didn't fire has nothing to
    defer.

18. **At most 64 signal entries per algorithm.** Each entry has one bit in the
    trigger mask that the handler reads with `uet_trigger_mask()`, which is a
    64-bit integer here. The spec gives no limit. A YAML file with more entries
    fails to load.

19. **`uet_trigger_mask()` takes a pointer.** The spec never gives this function
    a signature (every other PCM function has one in a table), and its prose and
    example disagree (§3.6.18.5.5). The text says it "**returns** an unsigned
    integer", but the example calls
    `uet_trigmask_t fired_triggers; uet_trigger_mask(&fired_triggers);` and reads
    the mask from the pointer. We use the pointer form,
    `int uet_trigger_mask(uet_trigmask_t *mask)`, because it matches the spec's
    other getters such as `uet_get_variable_int()`, which fill in a pointer and
    return a status code.

20. **Triggers can't be changed at runtime.** The spec's `uet_modify_trigger()` and
    `uet_query_trigger()` (Table 3-114) aren't implemented. None of the algorithms
    we plan to implement changes its triggers while running, and the spec doesn't
    define their `struct uet_trigger_attr` argument. A trigger's configuration
    comes only from the YAML file. Using either function fails to compile.

    Two consequences for `arm_mode`. A `once` trigger fires a single time and
    then stays disarmed, since nothing can re-arm it; that still has a use, e.g.
    one-time setup on the first ACK. A `disarmed` trigger could never fire, so
    the loader rejects it.

21. **When the handler's changes take effect.** The spec says signal values are
    frozen while the handler runs (§3.6.18.2) and that "control operations are
    executed in issued order" (§3.6.18.9). It doesn't say when a handler's own
    writes take effect. Ours:
    - `uet_signal_set` / `uet_signal_update` change the handler's frozen copy at
      once, so it reads back what it wrote.
    - Those writes and `uet_set_cwnd` reach the CCC, in the order the handler made
      them, when the handler returns.
    - Local variables change at once, since nothing else can see them.

22. **What counts as an invalid operation.** The spec says an invalid operation,
    "including (but not limited to) illegal PCM API usage or out-of-range index
    access", puts the CCC into the ERROR state, and leaves the error values to the
    implementation (§3.6.18.7). Ours, with their values (`PcmImplementationError`
    in `pcm_runtime.h`):
    - a signal or variable index out of range;
    - a variable read or written with the function for a different `dtype`;
    - `uet_set_cwnd` when the YAML has no `uet_tx_cwnd` control;
    - `uet_set_cwnd(0)`: a window of 0 would stop the flow for good;
    - a null pointer argument.

    Using a PCM function or macro outside a running handler is a programming
    error in the simulator, not a CCC error, and aborts the program.

23. **Handlers always read the current time.** The spec updates the `uet_time_*`
    signals only on timer events (§3.6.18.5.3). A handler called for an ACK
    would then read the time of the last timer event, which could be long ago,
    or never if the algorithm has no timer. So just before each handler call,
    `uet_time_current` is set to the current time and `uet_time_last_update` to
    the time of the last event that updated a signal. Their triggers are not
    checked then; those are still only checked on timer events. The time
    signals' own updates don't count as "a signal update" for
    `uet_time_last_update`.

24. **A deferred call happens at its deadline.** `invoke_deadline` is "an upper
    bound on how long an invoke action may be deferred" (§3.6.18.5.1), so any
    time up to the deadline would be allowed. We call the handler at the
    deadline itself, the latest allowed, so that triggers firing in the
    meantime are combined into that one call; this is what makes the deadline a
    coalescing setting. An immediate (deadline 0) trigger before then delivers
    the deferred one with it, earlier.

25. **The window, bytes in flight and backlog come with every event.**
    `uet_tx_cwnd`, `uet_tx_inflight_bytes` and `uet_tx_backlog_bytes` are
    `uet_tx_*` signals, which the spec updates when a packet is sent
    (§3.6.18.5.3). A handler called for an ACK would then see values from the
    last send, before the ACK freed up the window. The sender reports their
    current values with every event (ACK, NACK, send, timeout), so handlers
    always read them as they are now. As with any update, triggers on them are
    checked at those events (item 2).

26. **`initial_value: 0` keeps the simulator's window.** A `uet_tx_cwnd` control's
    `initial_value` is the window "at (re)start of algorithm" (§3.6.18.8.5).
    htsim already gives each flow a starting window from the network's
    bandwidth-delay product (`initNscc`, or `-cwnd`). Since 0 is never a valid
    window (item 22), we use `initial_value: 0` to mean "keep that window". Any
    other value replaces it.

27. **The ERROR state only stops new packets.** The spec says a CCC in the ERROR
    state admits no new packets, its in-flight and queued packets are "logically
    dropped", and its PDCs also go into an error state (§3.6.18.7.2.1). Here the
    flow stops sending, including retransmissions, but packets already in the
    network still arrive and are acknowledged, and nothing else about the flow
    changes. Unless all its data was already sent, the flow then never
    finishes; the error shows as the message printed when it happened, and as
    `Errors` in the `PCM:` summary line.

## 8. Commit plan

Each commit builds, passes `ctest`, and leaves non-PCM behaviour unchanged.
Commits that touch `UecSrc` or `main_uec.cpp` also run `commit_check.sh` (about
11 min, in the background). A commit that introduces a difference from the spec
adds its §7 entry and code comment in the same commit.

**Done**
- [x] Build, unit tests, smoke run, `commit_check.sh` baseline (all 8 pass on `65ea315`)
- [x] `pcm` branch; read spec §3.6.18 and the paper; agreed scope and §7 decisions

**Commits**
1. [x] **docs:** this plan (`df319bd`)
2. [x] **build:** yaml-cpp 0.8.0 via FetchContent (`53a9916`). The `pcm/` library
   target moved to commit 3, since CMake needs at least one source file.
3. [x] **pcm: YAML loader.** The `htsim_pcm` library; reads and checks the
   §3.6.18.8.5 format plus our `op_val` and `trigger_type` additions; rejects
   unsupported signals, controls and features. Unit tests.
4. [x] **pcm: accumulators and triggers.** All accumulators (including several per
   signal) and the full trigger behaviour from §3.6.18.5 plus §7 items 1, 2 and 4.
   Unit tests.
5. [x] **pcm: calling handlers.** The `uet_pcm.h` API; tracking which CCC is being
   served; matching YAML names; frozen values and applying controls; synchronous
   calls; ERROR state; counters. Unit tests that feed events directly, with no
   network.
6. [x] **pcm: time triggers and `invoke_deadline`.** The CCC asks for timer
   events through a scheduling callback; time signals and units; unit tests
   with a fake clock.
7. [x] **uec: connect PCM to the sender.** `PCM` enum; ACK/NACK/send/timeout calls
   into per-flow CCCs; the CCC's timer requests on htsim's event list; options
   `-pcm`, `-pcm_param`, `-pcm_units`; counters on the flow finish line and in
   the summary; `commit_check.sh`. Also the `htsim_pcm_algorithms` object
   library and a first algorithm, `constant` (fixed window), whose output on
   `one.cm` and `incast.cm` is identical to `-sender_cc_algo constant`.
8. [x] **pcm: DCTCP.** `dctcp_legacy` (exact port of the existing DCTCP) and the
   exact-match test, as ctest or a script, covering `constant` too; textbook
   `dctcp`, a unit test of α, and incast/permutation sanity runs.
9. [ ] **pcm: flow groups and shared windows.** `ccc <group>` in the connection
   matrix, grouping by (group, src, dst), `-pcm_ccc group=file`; group
   bytes-in-flight, taking turns between flows, group totals for signals; unit
   tests, tests that two grouped flows share one window fairly, and
   `commit_check.sh`.
10. [ ] **pcm: asynchronous mode.** `-pcm_exec async -pcm_handler_time_ns`; tests
    that triggers arriving during a call are combined.
11. [ ] **datacenter: `validate_pcm.txt` and its baseline**, added to `commit_check.sh`

**Later (design only now)**
- [ ] Global variables shared per NIC
- [ ] Rate limiting in the sender for `uet_set_send_rate` (DCQCN, TIMELY; Swift at
      windows below one packet)
- [ ] NSCC and Swift; compare NSCC with the built-in version and Swift with paper Fig. 6
- [ ] Set `_residency_time` in ACKs → `uet_rx_ack_svc`
- [ ] Event-drop model for overloaded NICs (§4.4)
