# Roadmap

Phases 2–4 of the project proposal. Phase 1 is described in
[pcm-plan.md](pcm-plan.md).

| Phase | Goal | Depends on |
|---|---|---|
| 1 | PCM runtime in htsim, verified with DCTCP (done) | — |
| 2 | More congestion-control algorithms on PCM | 1 |
| 3 | Simulation scenarios modelled on AI workloads | 1 (can start in parallel with 2) |
| 4 | Data collection and analysis | 2, 3 |

## Phase 2: more algorithms

Each algorithm is a YAML file plus a handler in `htsim/sim/pcm/algorithms/` (see
"Adding an algorithm" in [AGENTS.md](../AGENTS.md)).

| Algorithm | Main signals | Output | Check against | Needs first |
|---|---|---|---|---|
| NSCC (UET's default) | ECN, RTT, trims | window | built-in `-sender_cc_algo nscc` | — |
| Swift | RTT | window; pacing below one packet | built-in Swift (`htsim_swift`); paper Fig. 6 | rate limiting, for small windows |
| DCQCN | ECN | rate | published DCQCN behaviour | rate limiting; periodic timers |
| TIMELY | RTT and its gradient | rate | published TIMELY behaviour | rate limiting; RTT EWMA (`uet_op_ewma`) |
| BBR (if time permits) | ACK bytes over time, RTT | rate and window | published BBR behaviour | rate limiting |

Prerequisites, each its own commit:
- **Rate limiting in the sender**, for `uet_set_send_rate()` (spec Table 3-119)
  and the `uet_tx_send_rate` signal. Today the sender only enforces a window
  (`UecSrc::can_send_NSCC`). This is the biggest single piece of phase 2.
- **Remote service time** (`uet_rx_ack_svc`), if an algorithm needs it: the ACK
  field exists but is never set.

An algorithm htsim already has (NSCC, Swift) is first compared with the built-in
version on the same matrices. DCQCN and TIMELY are new here, so they are checked
against the behaviour in their papers (convergence and fairness on an incast,
reaction to ECN or RTT steps). Every algorithm should also run on
`incast_128.cm`: heavy trimming exposes mistakes small runs hide.

## Phase 3: scenarios

The proposal's three traffic types, on a fat tree, at low, moderate and high
load:

| Scenario | What it models | Existing generator (`datacenter/connection_matrices/`) |
|---|---|---|
| Incast | many senders to one receiver: parameter servers, storage reads | `gen_incast.py`, `gen_outcast_incast.py` |
| Many-to-many | all-to-all and allreduce collectives | `gen_serial_alltoall.py`, `gen_serialn_alltoall.py`, `gen_allreduce.py`, `gen_allreduce_butterfly.py`, `gen_permutation.py` |
| Mixed competing | an AI job sharing the network with background or storage traffic | combine the above; give each job its own flow group and algorithm (plan §4.5) |

To decide:
- **Sizes and speeds.** The topologies in `datacenter/topologies/` go from
  128 to 8192 hosts, and the repo has 100 Gbit/s and 800 Gbit/s experiment
  sets. Pick a small, quick configuration for development and one larger one
  for results.
- **Collective structure.** Connection matrices can chain transfers with
  `trigger`, `send_done_trigger` and `barrier`, which can model a collective's
  dependent steps. Choose message sizes and the number of steps per collective.
- **Where scenarios live.** Suggested: `datacenter/connection_matrices/pcm/`,
  with one script that regenerates them all from fixed seeds, so runs are
  reproducible.

## Phase 4: data collection

Metrics from the proposal, and where each comes from:

| Metric | Source |
|---|---|
| Flow completion time: average, p95, p99, and the slowest flow of each collective | flow finish lines (`Flow ... finished at <µs>`) |
| Throughput / goodput, link utilisation | `-log sink`, `-log traffic` (sampled every `-logtime_us`) |
| Queue occupancy | `-log queue_usage`, `-log switch`; `validation/extract_max_qs.sh` |
| Drops, retransmissions, trims | end-of-run line (`New: ... Rtx: ... NACKs: ...`) |
| Fairness | Jain's index over per-flow throughput, computed from the above |
| Handler invocations | `pcm_calls` on PCM flows' finish lines; `PCM:` summary line |

Plan:
- **A runner script** takes an algorithm × scenario × parameter grid, runs
  `htsim_uec` several times with different `-seed` values, and writes one CSV
  row per flow per run. `validation/analysis_and_plotting.py` is a starting
  point for parsing and plots (it needs `pip install -r requirements.txt`).
- **Ablations** from the proposal: trigger count and time thresholds (YAML, or
  `-pcm_param` for parameters), `invoke_deadline` (combining several events
  into one call), handler processing time (`-pcm_handler_time_ns`; paper
  Fig. 6 sweeps this), and spec vs. native units (numeric precision).
- **Handler counts aren't hardware cost.** They show how often a NIC would run
  the handler, not how long it would take, as the proposal notes.
