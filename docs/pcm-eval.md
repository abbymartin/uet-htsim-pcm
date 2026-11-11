# PCM evaluation

This page tells you how to compare congestion-control algorithms on fixed
workloads. Use it for PCM algorithms and for htsim's built-in algorithms.
It covers phase 3 (scenarios) and the first part of phase 4 (metrics) of
[roadmap.md](roadmap.md). The current results are in
[pcm-results.md](pcm-results.md).

## 1. Run it

Build first (see [AGENTS.md](../AGENTS.md)). Then, from `htsim/sim/datacenter/`:

```bash
./pcm_eval.py                                   # all scenarios, 3 algorithms, seeds 1,2,3
./pcm_eval.py --scenarios incast_8,mixed --seeds 1
./pcm_eval.py --algo dctcp='-sender_cc_algo dctcp' \
              --algo pcm_dctcp='-pcm ../pcm/algorithms/dctcp.yaml'
./pcm_eval.py --algo sync='-pcm ../pcm/algorithms/dctcp.yaml' \
              --algo async='-pcm ../pcm/algorithms/dctcp.yaml -pcm_handler_time_ns 1100'
./pcm_eval.py --scenarios mixed --algo \
    split='-pcm ../pcm/algorithms/dctcp.yaml -pcm_ccc bg=../pcm/algorithms/constant.yaml'
```

- Each `--algo NAME=OPTIONS` gives `htsim_uec` options. The first `--algo`
  replaces the default set: `nscc`, `dctcp` (built-in) and `pcm_dctcp`.
- Paths in the options are relative to `datacenter/`.
- The script adds `-sender_cc_only`, `-tm`, `-end`, `-seed`, `-o`,
  `-rtt_log`, `-link_stats` and the queue-logging options. Do not give them in
  `--algo`.
- The full default set (54 runs) takes about one minute on many cores.
  `--jobs N` sets the number of parallel runs.
- `--no-queues` skips queue logging. Then the `q_*` metrics are empty.

The script writes to `pcm_eval_out/` (or `--out DIR`). It replaces a previous
output there, but it does not delete a directory that it did not make.

| File | Content |
|---|---|
| `summary.csv` | One row for each job of each run: the metrics in §2. |
| `summary.md` | The mean over the seeds: one Markdown table for each scenario and job, with one column for each algorithm. With two or more seeds, `±N` after a value is the standard deviation over the seeds. The best value is in bold. Copy it into [pcm-results.md](pcm-results.md). |
| `flows.csv` | One row for each flow of each run: hosts, size, start, finish, FCT. |
| `runs/<scenario>/<algorithm>/seed<N>/stdout.txt` | The `htsim_uec` output. Line 1 is the command. |

The script also prints `summary.md`. A `-` in a table means that at least one
seed has no value. A run fails if `htsim_uec`
fails, or if a CCC goes into the ERROR state. The script prints a line for each
failed run, and then exits with 1.

## 2. Metrics

KB = 1024 bytes and MB = 1024 KB. Times are in µs.

For each row of the results tables, [pcm-results.md](pcm-results.md) §2
tells if larger or smaller is better, and which values to expect.

Each run has a row with job `all`. When the matrix puts flows in PCM flow groups
(`ccc GROUP`), each group is a job and has its own row too. Run totals
(retransmissions to PCM errors in the table) are only on the `all` row.

| Metric (column) | Definition | Why it is important |
|---|---|---|
| Unfinished flows (`unfinished`) | Flows without a finish line at `-end`. | An incorrect handler or the ERROR state can stop a flow. A value above 0 makes the other metrics incomplete. |
| Flow completion time (`fct_mean_us`, `fct_p50_us`, `fct_p95_us`, `fct_p99_us`, `fct_max_us`) | Finish time minus start time of each finished flow. Percentiles use the nearest rank. | This is the result that a user sees. The tail shows the worst flows. |
| Job completion time (`jct_us`) | Last finish minus first start in the job. Empty if a flow did not finish. | A collective operation ends only when its slowest flow ends. |
| Goodput (`goodput_gbps`) | Bytes of the job × 8 / JCT, in Gbit/s. This is the total for the job. | It shows how well the job uses the network. For an incast, 100 is the receiver's link rate. |
| Throughput (`throughput_gbps`) | Goodput × (new + retransmitted packets) / new packets: the data that the senders send, with retransmissions. Only on the `all` row. | The difference from goodput is bandwidth spent on retransmissions. It can be more than a link's rate: a trimmed packet uses the links before the switch that trims it. |
| Fairness (`jain`) | Jain's index over the throughput (size / FCT) of each finished flow. 1 is equal; 1/n is the worst. | It shows if some flows get less bandwidth. Compare it only within a job: different flow sizes make it lower. |
| Latency (`rtt_samples`, `rtt_mean_us`, `rtt_p50_us`, `rtt_p99_us`) | Round-trip time (RTT) samples: from sending a data packet to receiving its ACK, measured by the sender (`-rtt_log`). One sample for each ACK whose send record is valid, as for the `uet_rx_ack_rtt` signal. Statistics over all samples of the job's flows. | Queueing adds delay to each packet. Above the base RTT of 14 µs, most of the time is spent in queues. |
| Retransmissions (`rtx_pkts`, `rtx_pct`) | Packets sent again, and their percentage of new packets. | Each retransmission uses bandwidth again. |
| Trims (`trims`) | NACKs received. The sink sends a NACK only for a trimmed packet. | It shows how often queues were full. |
| Drops (`drops`) | Packets that switch queues dropped instead of trimming (`-link_stats`). With trimming, a queue drops a packet only when its queue for headers is also full. Only on the `all` row. | A dropped packet is resent only after a timeout, which is much slower than after a NACK. |
| Link utilization (`util_max_pct`, `util_max_at`, `util_mean_pct`) | For each link that leaves a switch: the bytes that entered it (`-link_stats`), divided by what its rate can carry from time 0 until the last flow finishes (until `-end` if a flow did not finish). `util_max_pct` is the busiest link, named in `util_max_at`; `util_mean_pct` is the mean over all of these links. Includes headers, trimmed packets, ACKs and NACKs. Only on the `all` row. | The busiest link is usually the bottleneck: near 100% means that the algorithm keeps it busy. The mean depends mostly on the scenario (an incast uses few links); between algorithms, a higher mean can also come from retransmissions. |
| Peak queue (`q_max_bytes`, `q_max_at`) | The highest occupancy of a switch queue, and the name of that queue. | Congestion control must keep queues short. |
| Mean queue (`q_mean_bytes`) | The mean of the samples of the queue with the highest peak, until the last flow finishes (until `-end` if a flow did not finish). | A standing queue adds delay to all flows that use it. |
| Handler calls (`pcm_calls`, `pcm_calls_per_mb`) | PCM handler calls in the run, and calls per MB of data in finished flows. Empty for built-in algorithms. | A NIC must run each call. The count shows the load, not the time for each call. |
| PCM errors (`pcm_errors`) | CCCs in the ERROR state. | An error stops the flows of that CCC. |

Notes:
- `±N` in `summary.md` shows how much a result changes with the seed. A
  difference between algorithms smaller than their `±N` may be chance.
- `htsim_uec` prints times with 6 significant digits. Above 1000 µs, the
  resolution is 0.1 µs.
- The queue metrics come from `-log tor_downqueue`. This option adds a sampling
  logger to every queue, not only to ToR downlinks. Host NIC queues (names
  starting with `SRC`) are not switch queues, and the script ignores them.
  Logging does not change the results of a run.
- Each sample keeps the highest occupancy since the previous sample. When all
  flows finish before `-end`, a sample comes after the last packet, so the peak
  is exact. The mean uses the occupancy at each sample time.
- The occupancy includes the priority queue (trimmed headers and control
  packets). Thus the peak can be more than the data buffer of the queue
  (178,450 bytes in these scenarios).

## 3. Scenarios

All scenarios use the default fat tree of `htsim_uec` for 128 hosts: 3 tiers,
4 hosts for each ToR switch, 100 Gbit/s links, no oversubscription. All flows
start at time 0. The base RTT is 14 µs. The starting window is 257 KB
(1.5 times the bandwidth-delay product).

| Scenario | Flows | What it models |
|---|---|---|
| `incast_8` | 8 senders, 1 MB each, to one host | Small incast, as in the paper's reaction-delay test (paper §7.3). |
| `incast_32` | 32 senders, 1 MB each, to one host | Medium incast. |
| `incast_127` | All other 127 hosts, 1 MB each, to one host | Very large incast: heavy trimming shows handler errors that small runs do not show. |
| `permutation` | Each host sends 2 MB to one host and receives one flow | Many-to-many with no shared receiver. The load is in the fabric. |
| `alltoall` | 8 groups of 16 hosts. In each group, each host sends 512 KB to each other host. | Many-to-many with a 15-to-1 incast at each receiver, as in an all-to-all collective. |
| `mixed` | Job `ai`: all-to-all in 4 groups of 16 hosts, 512 KB for each pair. Job `storage`: 16 hosts send 1 MB each to one `ai` host. Job `bg`: 48 hosts each send 4 MB to a different `ai` host. | An AI job that shares links with storage and background traffic. Each job is a PCM flow group, so you can give each one its own algorithm with `-pcm_ccc`. |

All flows are larger than the starting window, so the congestion control can
change how they send. In each scenario, the three default algorithms give
different results. (With 128 KB flows, the all-to-all finished in the first
window, and all algorithms gave the same result.)

In each PCM flow group, each pair of hosts has only one flow. Thus each flow
has its own CCC, and the built-in algorithms (which ignore `ccc`) see the same
flows. `pcm_eval_test.py` checks this.

`connection_matrices/pcm/gen_eval_scenarios.py` writes the matrices to
`connection_matrices/pcm/eval/`. Each scenario has a fixed random seed, so the
script always writes the same files. To add a scenario:

1. Add it to `SCENARIOS` in `gen_eval_scenarios.py`, and run the script.
2. Add it to `SCENARIOS` in `pcm_eval.py`, with an end time (about three times
   the slowest run) and a queue sampling period (100 or more samples).
3. Run `ctest`. `pcm_eval_check` makes sure that the matrices agree with the
   generator.

## 4. Tests

`pcm_eval_test.py` is part of `ctest` (`pcm_eval_check`, about 3 s). It tests
the metric functions, the scenario matrices, and two short runs: `incast_8`,
and `mixed` with one algorithm for each group.
