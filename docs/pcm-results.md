# PCM evaluation results

The current results of the PCM evaluation. [pcm-eval.md](pcm-eval.md) tells
what each scenario and metric is.

- **Made on:** 2026-11-11, with `./pcm_eval.py` (all scenarios; algorithms
  `nscc`, `dctcp` and `pcm_dctcp`; seeds 1, 2 and 3).
- **Code:** the simulator and the algorithms of commit `092440d`.
- **To update:** run `./pcm_eval.py` in `htsim/sim/datacenter/`. Then copy
  `pcm_eval_out/summary.md` into §3, and change the date and the commit above.
  Do this after a change to the simulator, an algorithm or a scenario.

## 1. Observations

- All flows finish in all 54 runs. No CCC goes into the ERROR state. No
  packet is dropped: full queues trim packets instead.
- **Permutation:** PCM `dctcp` is slow. Its tail FCT is 442 µs; built-in
  DCTCP has 233 µs and NSCC 211 µs. It does not send enough to fill the
  links: its busiest link is 50% busy (NSCC: 91%), its median RTT is 15.5 µs,
  near the base RTT of 14 µs, its mean queue is only 8 KB and it has no trims.
- **Incasts:** PCM `dctcp` gets more trims than built-in DCTCP: 45% more for
  8-to-1, 76% for 32-to-1, 17% for 127-to-1. Its FCTs are almost the same.
- **Queues and latency:** in the 8- and 32-to-1 incasts, both DCTCPs keep a
  mean queue 36 to 55% smaller than NSCC's, and a median RTT 5 to 7 µs lower. The
  p99 RTT is the same for all three: some packets still meet a full queue. In
  the 127-to-1 incast, the queue is full for all three, and nearly every
  packet waits in it (RTT 28 µs = 14 µs base + 14 µs).
- **Throughput:** PCM `dctcp` sends 8 to 14% more data than built-in DCTCP in
  the incasts, for the same goodput: its extra trims are resent.
- **Link use:** in the incasts, the receiver's link is 94 to 100% busy for all
  three. In the all-to-all, built-in DCTCP keeps its busiest link most busy
  (96.5%; NSCC 89%, PCM `dctcp` 84%) and has the highest goodput, although it
  has the most trims.
- **Mixed:** the algorithms divide the bandwidth differently. NSCC gives more to
  the large `bg` flows (tail FCT 1097 µs, against about 1400 µs for the DCTCPs).
  The DCTCPs give more to the `ai` job (JCT 1164 µs and 1205 µs, against
  1356 µs for NSCC). The difference between the two DCTCPs there is smaller
  than its spread over the seeds (±25 to ±55 µs).
- **Handler load:** PCM `dctcp` makes 88 to 638 handler calls for each MB. The
  most is in the 127-to-1 incast (about 2.5 calls for each data packet), because
  each NACK also causes a call.

## 2. How to read the tables

Each value is the mean over the seeds. The table for a scenario is for all of
its flows. `mixed` also has a table for each job (`ai`, `storage`, `bg`). In
each row, the best value is in bold. Each other value has its difference from
the best in percent. `±N` is the standard deviation over the seeds: a
difference between algorithms smaller than that may be chance.
[pcm-eval.md](pcm-eval.md) §2 gives the exact definitions.

Scale: a 100 Gbit/s link sends 1 MB of data in about 85 µs (with headers),
and carries at most about 98 Gbit/s of data. A switch queue holds 174 KB of
data (43 packets of 4150 bytes). A full queue adds about 14 µs of delay, the
same as the base RTT (round-trip time).

| Row | What it means | Better | Scale |
|---|---|---|---|
| unfinished flows | Flows that did not finish before the end of the run. | 0 | Must be 0. Above 0, the other numbers are not complete. |
| FCT mean | Mean flow completion time (FCT) in µs. | Smaller | Like FCT p50, but a few very slow or very fast flows move it. |
| FCT p50 | FCT in µs of the median flow: half of the flows finish faster. | Smaller | A flow alone sends 1 MB in about 90 µs. When flows share a link fairly, they all finish near the best JCT (below). |
| FCT p95 | FCT of the flow at 95%: only 5% of flows are slower. | Smaller | Between FCT p50 and FCT p99. With fewer than 20 flows (`incast_8`), it is the slowest flow. |
| FCT p99 | FCT of the flow at 99%: only 1% of flows are slower. The "tail". | Smaller | Between FCT p50 and JCT. A large gap to FCT p50 means that some flows got less bandwidth. |
| JCT | Job completion time in µs: from the first start to the last finish. All flows start at 0, so it is also the FCT of the slowest flow. | Smaller | At least the best possible job time (below). |
| goodput | Goodput in Gbit/s: data of the job divided by JCT. This is the total of all flows of the job, not the rate of one flow. | Larger | One receiver (incast, `storage`): at most about 98. `permutation`, `alltoall`: at most about 12,600 (128 links × 98). |
| throughput | Data that the senders send in Gbit/s, with retransmissions. Only for all flows. | Not marked: compare it with goodput | The difference from goodput is retransmitted data. It can be above the receiver's link rate: a trimmed packet still uses the links before the switch that trims it. |
| Jain fairness | Jain's fairness index over the rate of each flow. | Larger | 1 = all flows get the same rate. 0.99 or more is fair. Compare it only within one table. In the `mixed` table for all flows, it is low because the flow sizes are different. |
| RTT p50 | Round-trip time (RTT) in µs of the median packet: from sending a data packet to receiving its ACK. | Smaller | At least the base RTT, 14 µs. Each full queue on the way adds about 14 µs. |
| RTT p99 | RTT of the packet at 99%. The latency tail. | Smaller | Between RTT p50 and about 14 µs + 14 µs per full queue on the path. |
| retransmitted | Packets sent again, as a percentage of new packets. | Smaller | 0 is best. 20% = 1 packet in 5 is sent again. Above 100%, there are more resent packets than new packets. |
| trims | Packets that a full switch queue cut to a header (each causes a NACK and a resend). | Smaller | 0 is best. A 1 MB flow has about 257 packets: an 8-to-1 incast of 1 MB has about 2,060 packets, so 400 trims is about 1 in 5. |
| drops | Packets that a switch queue dropped instead of trimming. Only for all flows. | Smaller | 0 is best. Above 0, a queue for trimmed headers was full too. |
| busiest link | Utilization in % of the busiest switch link, from time 0 to the last finish. Only for all flows. | Larger | Up to 100. Usually the bottleneck (in an incast, the receiver's link): near 100 means that the algorithm keeps it busy. It includes headers, trimmed packets and ACKs. |
| mean link | Mean utilization in % of all links that leave a switch, over the same time. Only for all flows. | Not marked | Depends mostly on the scenario: an incast uses few of the 640 links. Higher can also mean more retransmissions. |
| peak queue | Highest occupancy of a switch queue, in KB. | Smaller | At most about 174 for data. 174 means that the queue was full and trimming started. More than 174 is possible: trimmed headers and control packets are in a separate queue that is counted too. |
| mean queue | Mean occupancy of the queue with the highest peak. | Smaller | 0 to about 174. Each 12.5 KB adds 1 µs of delay to each packet that goes through the queue. |
| handler calls / MB | PCM handler calls for each MB of data. Only for PCM algorithms. | Smaller, if the other results stay the same (not marked in bold) | Each call is work for the NIC (network interface card). About 257 means one call for each data packet. |

Best possible job time (JCT): the time to send all data over the busiest
link, at 100 Gbit/s, with no losses. Compare JCT with it: 10% more means 10%
slower than the best.

| Scenario | Busiest link | Best JCT (µs) |
|---|---|--:|
| `incast_8` | 8 MB into the receiver | about 680 |
| `incast_32` | 32 MB into the receiver | about 2,730 |
| `incast_127` | 127 MB into the receiver | about 10,830 |
| `permutation` | 2 MB on each host link | about 180 |
| `alltoall` | 7.5 MB into each receiver | about 640 |
| `mixed` (all, `storage`) | 23.5 MB into the `storage` receiver | about 2,000 |
| `mixed` (`ai`, `bg`) | up to 11.5 MB into an `ai` host that also gets a `bg` flow | about 980 |

## 3. Tables

One table for each scenario, and for each job of `mixed`. Each column is an
algorithm.

Mean over seeds 1,2,3. KB = 1024 bytes. Bold: the best value. ±N: the standard deviation over the seeds. (+N%): the difference from the best value. '-': no value for at least one seed.

### incast_8

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | 666.3 ±13.3 (+0.1%) | **665.3** ±6.3 | 674.5 ±5.8 (+1.4%) |
| FCT p50 (us) | smaller | **664.9** ±15.7 | 665.4 ±8.9 (+0.1%) | 673.8 ±12.6 (+1.3%) |
| FCT p95 (us) | smaller | 723.2 ±7.2 (+0.0%) | 726.5 ±11.4 (+0.5%) | **723.1** ±6.6 |
| FCT p99 (us) | smaller | 723.2 ±7.2 (+0.0%) | 726.5 ±11.4 (+0.5%) | **723.1** ±6.6 |
| JCT (us) | smaller | 723.2 ±7.2 (+0.0%) | 726.5 ±11.4 (+0.5%) | **723.1** ±6.6 |
| goodput (Gbit/s) | larger | **92.8** ±0.9 | 92.4 ±1.5 (-0.5%) | **92.8** ±0.9 |
| throughput (Gbit/s) |  | 111.9 ±1.0 | 110.3 ±1.7 | 118.9 ±1.2 |
| Jain fairness | larger | **0.997** ±0.001 | 0.995 ±0.003 (-0.2%) | 0.996 ±0.001 (-0.1%) |
| RTT p50 (us) | smaller | 25.2 ±0.0 (+42%) | 19.0 ±0.1 (+6.6%) | **17.8** ±0.2 |
| RTT p99 (us) | smaller | 42.0 ±0.2 (+0.3%) | **41.9** ±0.2 | 42.2 ±0.1 (+0.7%) |
| retransmitted (%) | smaller | 20.61 ±0.18 (+6.2%) | **19.41** ±0.05 | 28.13 ±0.14 (+45%) |
| trims | smaller | 424 ±4 (+6.2%) | **399** ±1 | 578 ±3 (+45%) |
| drops | smaller | **0** ±0 | **0** ±0 | **0** ±0 |
| busiest link (%) | larger | 94.6 ±0.9 (-0.1%) | 94.1 ±1.5 (-0.6%) | **94.7** ±0.9 |
| mean link (%) |  | 0.86 ±0.01 | 0.85 ±0.01 | 0.91 ±0.01 |
| peak queue (KB) | smaller | 174.9 ±0.0 (+0.1%) | 174.9 ±0.0 (+0.1%) | **174.8** ±0.0 |
| mean queue (KB) | smaller | 127.0 ±3.8 (+123%) | 63.0 ±0.3 (+11%) | **57.0** ±1.7 |
| handler calls / MB |  | - | - | 242.3 ±9.3 |

### incast_32

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | **2494.5** ±16.9 | 2571.1 ±13.7 (+3.1%) | 2575.7 ±32.4 (+3.3%) |
| FCT p50 (us) | smaller | **2515.0** ±28.6 | 2604.9 ±33.6 (+3.6%) | 2634.5 ±29.5 (+4.8%) |
| FCT p95 (us) | smaller | **2757.1** ±2.7 | 2765.8 ±6.1 (+0.3%) | 2776.9 ±14.8 (+0.7%) |
| FCT p99 (us) | smaller | **2769.7** ±3.8 | 2771.9 ±3.9 (+0.1%) | 2788.0 ±19.5 (+0.7%) |
| JCT (us) | smaller | **2769.7** ±3.8 | 2771.9 ±3.9 (+0.1%) | 2788.0 ±19.5 (+0.7%) |
| goodput (Gbit/s) | larger | **96.9** ±0.1 | 96.8 ±0.1 (-0.1%) | 96.3 ±0.7 (-0.7%) |
| throughput (Gbit/s) |  | 121.3 ±0.7 | 119.3 ±0.2 | 135.6 ±1.0 |
| Jain fairness | larger | 0.992 ±0.001 (-0.2%) | **0.995** ±0.002 | 0.990 ±0.004 (-0.5%) |
| RTT p50 (us) | smaller | 25.9 ±0.0 (+25%) | 21.0 ±0.2 (+1.6%) | **20.7** ±0.2 |
| RTT p99 (us) | smaller | **47.1** ±0.6 | 48.1 ±0.8 (+2.1%) | 47.7 ±0.3 (+1.3%) |
| retransmitted (%) | smaller | 25.13 ±0.66 (+8.4%) | **23.19** ±0.01 | 40.82 ±0.03 (+76%) |
| trims | smaller | 2066 ±54 (+8.4%) | **1907** ±1 | 3357 ±2 (+76%) |
| drops | smaller | **0** ±0 | **0** ±0 | **0** ±0 |
| busiest link (%) | larger | **98.8** ±0.1 | 98.7 ±0.1 (-0.1%) | 98.4 ±0.7 (-0.4%) |
| mean link (%) |  | 0.86 ±0.00 | 0.85 ±0.00 | 0.93 ±0.01 |
| peak queue (KB) | smaller | **176.2** ±0.1 | 176.6 ±0.3 (+0.2%) | **176.2** ±0.1 |
| mean queue (KB) | smaller | 143.3 ±1.2 (+60%) | 91.4 ±0.6 (+2.3%) | **89.3** ±1.9 |
| handler calls / MB |  | - | - | 326.6 ±0.6 |

### incast_127

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | 10548.0 ±6.3 (+0.1%) | **10539.6** ±7.1 | 10595.5 ±13.7 (+0.5%) |
| FCT p50 (us) | smaller | 10893.4 ±9.4 (+0.2%) | **10873.6** ±18.0 | 10910.6 ±14.1 (+0.3%) |
| FCT p95 (us) | smaller | **11037.8** ±7.2 | 11038.2 ±0.9 (+0.0%) | 11074.8 ±3.6 (+0.3%) |
| FCT p99 (us) | smaller | **11051.3** ±3.6 | 11057.2 ±4.9 (+0.1%) | 11097.1 ±6.9 (+0.4%) |
| JCT (us) | smaller | 11068.9 ±18.0 (+0.0%) | **11065.0** ±13.4 | 11109.8 ±1.6 (+0.4%) |
| goodput (Gbit/s) | larger | 96.2 ±0.2 (-0.0%) | **96.3** ±0.1 | 95.9 ±0.0 (-0.4%) |
| throughput (Gbit/s) |  | 218.3 ±0.5 | 218.9 ±0.3 | 238.2 ±0.4 |
| Jain fairness | larger | 0.979 ±0.001 (-0.2%) | 0.979 ±0.000 (-0.2%) | **0.981** ±0.001 |
| RTT p50 (us) | smaller | **28.0** ±0.0 | **28.0** ±0.0 | **28.0** ±0.0 |
| RTT p99 (us) | smaller | 28.8 ±0.1 (+0.2%) | **28.7** ±0.0 | 28.8 ±0.0 (+0.0%) |
| retransmitted (%) | smaller | **126.82** ±0.18 | 127.37 ±0.27 (+0.4%) | 148.35 ±0.41 (+17%) |
| trims | smaller | **41393** ±58 | 41572 ±87 (+0.4%) | 48420 ±135 (+17%) |
| drops | smaller | **0** ±0 | **0** ±0 | **0** ±0 |
| busiest link (%) | larger | **99.7** ±0.2 | **99.7** ±0.1 | 99.6 ±0.0 (-0.1%) |
| mean link (%) |  | 1.41 ±0.00 | 1.41 ±0.00 | 1.49 ±0.00 |
| peak queue (KB) | smaller | 325.7 ±2.7 (+6.4%) | 346.5 ±2.5 (+13%) | **306.0** ±1.9 |
| mean queue (KB) | smaller | 169.9 ±0.3 (+0.2%) | **169.6** ±0.3 | **169.6** ±0.5 |
| handler calls / MB |  | - | - | 637.7 ±1.1 |

### permutation

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | **200.8** ±1.1 | 205.6 ±0.8 (+2.4%) | 291.6 ±5.5 (+45%) |
| FCT p50 (us) | smaller | **201.3** ±0.7 | 205.6 ±0.9 (+2.1%) | 296.9 ±3.9 (+47%) |
| FCT p95 (us) | smaller | **208.5** ±1.7 | 223.7 ±1.8 (+7.3%) | 400.7 ±4.0 (+92%) |
| FCT p99 (us) | smaller | **211.3** ±2.5 | 233.3 ±4.0 (+10%) | 441.8 ±14.4 (+109%) |
| JCT (us) | smaller | **211.9** ±2.8 | 236.8 ±3.9 (+12%) | 448.7 ±7.3 (+112%) |
| goodput (Gbit/s) | larger | **10136.0** ±132.0 | 9069.7 ±149.9 (-11%) | 4786.8 ±77.5 (-53%) |
| throughput (Gbit/s) |  | 10136.0 ±132.0 | 9069.7 ±149.9 | 4786.8 ±77.5 |
| Jain fairness | larger | **0.999** ±0.000 | 0.997 ±0.000 (-0.2%) | 0.948 ±0.005 (-5.2%) |
| RTT p50 (us) | smaller | 20.8 ±0.2 (+34%) | 18.7 ±0.0 (+21%) | **15.5** ±0.1 |
| RTT p99 (us) | smaller | 31.5 ±0.9 (+27%) | 29.2 ±0.8 (+17%) | **24.9** ±0.5 |
| retransmitted (%) | smaller | **0.00** ±0.00 | **0.00** ±0.00 | **0.00** ±0.00 |
| trims | smaller | **0** ±0 | **0** ±0 | **0** ±0 |
| drops | smaller | **0** ±0 | **0** ±0 | **0** ±0 |
| busiest link (%) | larger | **91.0** ±0.9 | 85.2 ±2.4 (-6.5%) | 49.6 ±3.7 (-45%) |
| mean link (%) |  | 77.07 ±1.00 | 68.95 ±1.14 | 36.40 ±0.59 |
| peak queue (KB) | smaller | 145.9 ±14.6 (+21%) | 140.6 ±10.2 (+17%) | **120.3** ±12.4 |
| mean queue (KB) | smaller | 43.6 ±8.5 (+441%) | 29.9 ±0.6 (+271%) | **8.1** ±1.3 |
| handler calls / MB |  | - | - | 87.6 ±0.3 |

### alltoall

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | **663.0** ±0.8 | 675.1 ±1.8 (+1.8%) | 673.7 ±1.6 (+1.6%) |
| FCT p50 (us) | smaller | **663.0** ±1.1 | 673.3 ±2.2 (+1.6%) | 670.5 ±1.7 (+1.1%) |
| FCT p95 (us) | smaller | **694.6** ±4.7 | 697.6 ±4.5 (+0.4%) | 710.4 ±4.9 (+2.3%) |
| FCT p99 (us) | smaller | 713.4 ±8.0 (+1.1%) | **705.6** ±4.4 | 731.5 ±7.1 (+3.7%) |
| JCT (us) | smaller | 753.2 ±12.6 (+6.2%) | **709.0** ±3.8 | 814.4 ±18.4 (+15%) |
| goodput (Gbit/s) | larger | 10693.7 ±177.7 (-5.9%) | **11358.3** ±60.3 | 9892.3 ±226.0 (-13%) |
| throughput (Gbit/s) |  | 10763.6 ±170.0 | 11542.9 ±36.2 | 9980.9 ±217.0 |
| Jain fairness | larger | 0.999 ±0.000 (-0.1%) | **1.000** ±0.000 | 0.999 ±0.000 (-0.1%) |
| RTT p50 (us) | smaller | 27.7 ±0.3 (+2.9%) | 27.8 ±0.3 (+3.1%) | **26.9** ±0.2 |
| RTT p99 (us) | smaller | 44.3 ±0.8 (+2.7%) | 44.0 ±0.9 (+2.0%) | **43.1** ±0.9 |
| retransmitted (%) | smaller | **0.65** ±0.08 | 1.63 ±0.25 (+148%) | 0.90 ±0.16 (+37%) |
| trims | smaller | **1620** ±207 | 4028 ±629 (+149%) | 2218 ±395 (+37%) |
| drops | smaller | **0** ±0 | **0** ±0 | **0** ±0 |
| busiest link (%) | larger | 89.0 ±1.7 (-7.7%) | **96.5** ±0.7 | 84.1 ±2.3 (-13%) |
| mean link (%) |  | 81.25 ±1.31 | 86.46 ±0.32 | 75.19 ±1.74 |
| peak queue (KB) | smaller | 176.3 ±1.4 (+0.9%) | 177.1 ±0.1 (+1.4%) | **174.7** ±0.1 |
| mean queue (KB) | smaller | **50.1** ±5.9 | 59.4 ±7.6 (+19%) | 62.7 ±13.8 (+25%) |
| handler calls / MB |  | - | - | 122.4 ±2.3 |

### mixed

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | **702.8** ±1.3 | 746.2 ±0.1 (+6.2%) | 747.3 ±0.4 (+6.3%) |
| FCT p50 (us) | smaller | **672.1** ±2.7 | 700.9 ±0.3 (+4.3%) | 697.3 ±0.3 (+3.8%) |
| FCT p95 (us) | smaller | **1060.9** ±3.7 | 1364.8 ±2.4 (+29%) | 1300.9 ±3.9 (+23%) |
| FCT p99 (us) | smaller | **1883.9** ±13.5 | 1905.5 ±5.4 (+1.1%) | 1915.3 ±20.4 (+1.7%) |
| JCT (us) | smaller | **2050.0** ±10.6 | 2057.2 ±18.7 (+0.4%) | 2058.2 ±14.0 (+0.4%) |
| goodput (Gbit/s) | larger | **2815.4** ±14.5 | 2805.5 ±25.4 (-0.3%) | 2804.2 ±19.0 (-0.4%) |
| throughput (Gbit/s) |  | 2958.4 ±17.6 | 2941.8 ±26.6 | 2934.3 ±20.5 |
| Jain fairness | larger | 0.658 ±0.002 (-11%) | **0.742** ±0.000 | 0.726 ±0.001 (-2.1%) |
| RTT p50 (us) | smaller | **26.9** ±0.0 | 27.9 ±0.0 (+3.7%) | 27.5 ±0.0 (+2.2%) |
| RTT p99 (us) | smaller | 42.9 ±0.1 (+9.8%) | 41.4 ±0.2 (+5.8%) | **39.1** ±0.3 |
| retransmitted (%) | smaller | 5.08 ±0.11 (+9.5%) | 4.86 ±0.01 (+4.7%) | **4.64** ±0.03 |
| trims | smaller | 9002 ±194 (+10%) | 8607 ±16 (+5.5%) | **8161** ±42 |
| drops | smaller | **0** ±0 | **0** ±0 | **0** ±0 |
| busiest link (%) | larger | **98.4** ±0.5 | 98.1 ±0.9 (-0.4%) | 98.2 ±0.6 (-0.2%) |
| mean link (%) |  | 22.00 ±0.13 | 21.88 ±0.20 | 21.83 ±0.15 |
| peak queue (KB) | smaller | **175.4** ±0.1 | 177.1 ±0.0 (+1.0%) | 177.1 ±0.1 (+1.0%) |
| mean queue (KB) | smaller | 140.2 ±0.9 (+978%) | 54.2 ±0.8 (+317%) | **13.0** ±0.5 |
| handler calls / MB |  | - | - | 188.7 ±0.5 |

### mixed, job ai

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | **665.0** ±1.5 | 696.9 ±0.0 (+4.8%) | 700.0 ±0.4 (+5.3%) |
| FCT p50 (us) | smaller | **666.6** ±2.8 | 699.7 ±0.5 (+5.0%) | 695.1 ±0.4 (+4.3%) |
| FCT p95 (us) | smaller | 797.0 ±0.7 (+11%) | **715.9** ±0.7 | 741.2 ±1.7 (+3.5%) |
| FCT p99 (us) | smaller | 1147.6 ±48.4 (+34%) | **857.4** ±29.6 | 1089.7 ±23.0 (+27%) |
| JCT (us) | smaller | 1355.9 ±42.6 (+17%) | **1163.8** ±54.7 | 1204.5 ±25.0 (+3.5%) |
| goodput (Gbit/s) | larger | 2971.6 ±92.4 (-14%) | **3464.7** ±158.6 | 3343.8 ±68.7 (-3.5%) |
| Jain fairness | larger | 0.952 ±0.008 (-4.6%) | **0.998** ±0.000 | 0.996 ±0.000 (-0.2%) |
| RTT p50 (us) | smaller | **27.2** ±0.0 | 28.2 ±0.0 (+3.6%) | 27.9 ±0.0 (+2.5%) |
| RTT p99 (us) | smaller | 42.6 ±0.1 (+10%) | 40.9 ±0.3 (+6.0%) | **38.5** ±0.3 |

### mixed, job bg

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | **1057.6** ±1.7 | 1340.8 ±0.1 (+27%) | 1299.4 ±1.1 (+23%) |
| FCT p50 (us) | smaller | **1061.1** ±3.9 | 1370.6 ±1.2 (+29%) | 1327.2 ±2.6 (+25%) |
| FCT p95 (us) | smaller | **1087.7** ±4.1 | 1384.9 ±4.2 (+27%) | 1357.5 ±5.9 (+25%) |
| FCT p99 (us) | smaller | **1097.3** ±6.5 | 1408.1 ±1.9 (+28%) | 1390.3 ±35.6 (+27%) |
| JCT (us) | smaller | **1097.3** ±6.5 | 1408.1 ±1.9 (+28%) | 1390.3 ±35.6 (+27%) |
| goodput (Gbit/s) | larger | **1467.8** ±8.7 | 1143.8 ±1.6 (-22%) | 1158.9 ±29.3 (-21%) |
| Jain fairness | larger | **1.000** ±0.000 | 0.995 ±0.000 (-0.4%) | 0.995 ±0.000 (-0.4%) |
| RTT p50 (us) | smaller | 20.1 ±0.6 (+37%) | 14.7 ±0.0 (+0.2%) | **14.6** ±0.0 |
| RTT p99 (us) | smaller | 43.6 ±0.0 (+10%) | 42.4 ±0.2 (+7.2%) | **39.5** ±0.3 |

### mixed, job storage

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT mean (us) | smaller | **1909.3** ±4.4 | 1923.1 ±7.0 (+0.7%) | 1933.7 ±28.5 (+1.3%) |
| FCT p50 (us) | smaller | **1908.9** ±16.4 | 1936.9 ±25.6 (+1.5%) | 1954.5 ±26.2 (+2.4%) |
| FCT p95 (us) | smaller | **2050.0** ±10.6 | 2057.2 ±18.7 (+0.4%) | 2058.2 ±14.0 (+0.4%) |
| FCT p99 (us) | smaller | **2050.0** ±10.6 | 2057.2 ±18.7 (+0.4%) | 2058.2 ±14.0 (+0.4%) |
| JCT (us) | smaller | **2050.0** ±10.6 | 2057.2 ±18.7 (+0.4%) | 2058.2 ±14.0 (+0.4%) |
| goodput (Gbit/s) | larger | **65.5** ±0.3 | 65.2 ±0.6 (-0.3%) | 65.2 ±0.4 (-0.4%) |
| Jain fairness | larger | **0.996** ±0.002 | 0.995 ±0.002 (-0.1%) | **0.996** ±0.003 |
| RTT p50 (us) | smaller | 25.6 ±0.1 (+33%) | 20.0 ±0.2 (+3.7%) | **19.3** ±0.1 |
| RTT p99 (us) | smaller | **50.4** ±0.3 | 51.3 ±0.2 (+1.9%) | 51.2 ±0.3 (+1.6%) |
