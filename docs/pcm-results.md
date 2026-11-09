# PCM evaluation results

The current results of the PCM evaluation. [pcm-eval.md](pcm-eval.md) tells
what each scenario and metric is.

- **Made on:** 2026-11-09, with `./pcm_eval.py` (all scenarios; algorithms
  `nscc`, `dctcp` and `pcm_dctcp`; seeds 1, 2 and 3).
- **Code:** the simulator and the algorithms of commit `83dd2e8`.
- **To update:** run `./pcm_eval.py` in `htsim/sim/datacenter/`. Then copy
  `pcm_eval_out/summary.md` into §3, and change the date and the commit above.
  Do this after a change to the simulator, an algorithm or a scenario.

## 1. Observations

- All flows finish in all 54 runs. No CCC goes into the ERROR state.
- **Permutation:** PCM `dctcp` is slow. Its tail FCT is 449 µs; built-in
  DCTCP has 237 µs and NSCC 212 µs. Its mean queue is only 8 KB and it has no
  trims. Thus it probably does not send enough to fill the links.
- **Incasts:** PCM `dctcp` gets more trims than built-in DCTCP: 45% more for
  8-to-1, 76% for 32-to-1, 16% for 127-to-1. Its FCTs are almost the same.
- **Queues:** in the 8- and 32-to-1 incasts, both DCTCPs keep a mean queue of
  about half that of NSCC. In the 127-to-1 incast, the queue is full for all
  three.
- **Mixed:** the algorithms divide the bandwidth differently. NSCC gives more to
  the large `bg` flows (tail FCT 1097 µs, against about 1400 µs for the DCTCPs).
  The DCTCPs give more to the `ai` job (tail FCT 1164 µs and 1205 µs, against
  1356 µs for NSCC).
- **Handler load:** PCM `dctcp` makes 88 to 638 handler calls for each MB. The
  most is in the 127-to-1 incast (about 2.5 calls for each data packet), because
  each NACK also causes a call.

## 2. How to read the tables

Each value is the mean over the seeds. The table for a scenario is for all of
its flows. `mixed` also has a table for each job (`ai`, `storage`, `bg`). In
each row, the best value is in bold. Each other value has its difference from
the best in percent.
[pcm-eval.md](pcm-eval.md) §2 gives the exact definitions.

Scale: a 100 Gbit/s link sends 1 MB of data in about 85 µs (with headers),
and carries at most about 98 Gbit/s of data. A switch queue holds 174 KB of
data (43 packets of 4150 bytes). A full queue adds about 14 µs of delay, the
same as the base RTT (round-trip time).

| Row | What it means | Better | Scale |
|---|---|---|---|
| unfinished flows | Flows that did not finish before the end of the run. | 0 | Must be 0. Above 0, the other numbers are not complete. |
| FCT p50 | Flow completion time (FCT) in µs of the median flow: half of the flows finish faster. | Smaller | A flow alone sends 1 MB in about 90 µs. When flows share a link fairly, they all finish near the best JCT (below). |
| FCT p99 | FCT of the flow at 99%: only 1% of flows are slower. The "tail". | Smaller | Between FCT p50 and JCT. A large gap to FCT p50 means that some flows got less bandwidth. |
| JCT | Job completion time in µs: from the first start to the last finish. All flows start at 0, so it is also the FCT of the slowest flow. | Smaller | At least the best possible job time (below). |
| goodput | Goodput in Gbit/s: data of the job divided by JCT. This is the total of all flows of the job, not the rate of one flow. | Larger | One receiver (incast, `storage`): at most about 98. `permutation`, `alltoall`: at most about 12,600 (128 links × 98). |
| Jain fairness | Jain's fairness index over the rate of each flow. | Larger | 1 = all flows get the same rate. 0.99 or more is fair. Compare it only within one table. In the `mixed` table for all flows, it is low because the flow sizes are different. |
| retransmitted | Packets sent again, as a percentage of new packets. | Smaller | 0 is best. 20% = 1 packet in 5 is sent again. Above 100%, there are more resent packets than new packets. |
| trims | Packets that a full switch queue cut to a header (each causes a NACK and a resend). | Smaller | 0 is best. A 1 MB flow has about 257 packets: an 8-to-1 incast of 1 MB has about 2,060 packets, so 400 trims is about 1 in 5. |
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

Mean over seeds 1,2,3. KB = 1024 bytes. Bold: the best value. (+N%): the difference from the best value. '-': no value for at least one seed.

### incast_8

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **664.9** | 665.4 (+0.1%) | 673.8 (+1.3%) |
| FCT p99 (us) | smaller | 723.2 (+0.0%) | 726.5 (+0.5%) | **723.1** |
| JCT (us) | smaller | 723.2 (+0.0%) | 726.5 (+0.5%) | **723.1** |
| goodput (Gbit/s) | larger | **92.8** | 92.4 (-0.5%) | **92.8** |
| Jain fairness | larger | **0.997** | 0.995 (-0.2%) | 0.996 (-0.1%) |
| retransmitted (%) | smaller | 20.61 (+6.2%) | **19.41** | 28.13 (+45%) |
| trims | smaller | 424 (+6.2%) | **399** | 578 (+45%) |
| peak queue (KB) | smaller | 174.9 (+0.1%) | 174.9 (+0.1%) | **174.8** |
| mean queue (KB) | smaller | 127.0 (+123%) | 63.0 (+11%) | **57.0** |
| handler calls / MB |  | - | - | 242.3 |

### incast_32

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **2515.0** | 2604.9 (+3.6%) | 2634.5 (+4.8%) |
| FCT p99 (us) | smaller | **2769.7** | 2771.9 (+0.1%) | 2788.0 (+0.7%) |
| JCT (us) | smaller | **2769.7** | 2771.9 (+0.1%) | 2788.0 (+0.7%) |
| goodput (Gbit/s) | larger | **96.9** | 96.8 (-0.1%) | 96.3 (-0.7%) |
| Jain fairness | larger | 0.992 (-0.2%) | **0.995** | 0.990 (-0.5%) |
| retransmitted (%) | smaller | 25.13 (+8.4%) | **23.19** | 40.82 (+76%) |
| trims | smaller | 2066 (+8.4%) | **1907** | 3357 (+76%) |
| peak queue (KB) | smaller | **176.2** | 176.6 (+0.2%) | **176.2** |
| mean queue (KB) | smaller | 143.3 (+60%) | 91.4 (+2.3%) | **89.3** |
| handler calls / MB |  | - | - | 326.6 |

### incast_127

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | 10893.4 (+0.2%) | **10873.6** | 10910.6 (+0.3%) |
| FCT p99 (us) | smaller | **11051.3** | 11057.2 (+0.1%) | 11097.1 (+0.4%) |
| JCT (us) | smaller | 11068.9 (+0.0%) | **11065.0** | 11109.8 (+0.4%) |
| goodput (Gbit/s) | larger | 96.2 (-0.0%) | **96.3** | 95.9 (-0.4%) |
| Jain fairness | larger | 0.979 (-0.2%) | 0.979 (-0.2%) | **0.981** |
| retransmitted (%) | smaller | **126.82** | 127.37 (+0.4%) | 148.35 (+17%) |
| trims | smaller | **41393** | 41572 (+0.4%) | 48420 (+17%) |
| peak queue (KB) | smaller | 325.7 (+6.4%) | 346.5 (+13%) | **306.0** |
| mean queue (KB) | smaller | 169.9 (+0.2%) | **169.6** | **169.6** |
| handler calls / MB |  | - | - | 637.7 |

### permutation

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **201.3** | 205.6 (+2.1%) | 296.9 (+47%) |
| FCT p99 (us) | smaller | **211.3** | 233.3 (+10%) | 441.8 (+109%) |
| JCT (us) | smaller | **211.9** | 236.8 (+12%) | 448.7 (+112%) |
| goodput (Gbit/s) | larger | **10136.0** | 9069.7 (-11%) | 4786.8 (-53%) |
| Jain fairness | larger | **0.999** | 0.997 (-0.2%) | 0.948 (-5.2%) |
| retransmitted (%) | smaller | **0.00** | **0.00** | **0.00** |
| trims | smaller | **0** | **0** | **0** |
| peak queue (KB) | smaller | 145.9 (+21%) | 140.6 (+17%) | **120.3** |
| mean queue (KB) | smaller | 43.6 (+441%) | 29.9 (+271%) | **8.1** |
| handler calls / MB |  | - | - | 87.6 |

### alltoall

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **663.0** | 673.3 (+1.6%) | 670.5 (+1.1%) |
| FCT p99 (us) | smaller | 713.4 (+1.1%) | **705.6** | 731.5 (+3.7%) |
| JCT (us) | smaller | 753.2 (+6.2%) | **709.0** | 814.4 (+15%) |
| goodput (Gbit/s) | larger | 10693.7 (-5.9%) | **11358.3** | 9892.3 (-13%) |
| Jain fairness | larger | 0.999 (-0.1%) | **1.000** | 0.999 (-0.1%) |
| retransmitted (%) | smaller | **0.65** | 1.63 (+148%) | 0.90 (+37%) |
| trims | smaller | **1620** | 4028 (+149%) | 2218 (+37%) |
| peak queue (KB) | smaller | 176.3 (+0.9%) | 177.1 (+1.4%) | **174.7** |
| mean queue (KB) | smaller | **50.1** | 59.4 (+19%) | 62.7 (+25%) |
| handler calls / MB |  | - | - | 122.4 |

### mixed

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **672.1** | 700.9 (+4.3%) | 697.3 (+3.8%) |
| FCT p99 (us) | smaller | **1883.9** | 1905.5 (+1.1%) | 1915.3 (+1.7%) |
| JCT (us) | smaller | **2050.0** | 2057.2 (+0.4%) | 2058.2 (+0.4%) |
| goodput (Gbit/s) | larger | **2815.4** | 2805.5 (-0.3%) | 2804.2 (-0.4%) |
| Jain fairness | larger | 0.658 (-11%) | **0.742** | 0.726 (-2.1%) |
| retransmitted (%) | smaller | 5.08 (+9.5%) | 4.86 (+4.7%) | **4.64** |
| trims | smaller | 9002 (+10%) | 8607 (+5.5%) | **8161** |
| peak queue (KB) | smaller | **175.4** | 177.1 (+1.0%) | 177.1 (+1.0%) |
| mean queue (KB) | smaller | 140.2 (+978%) | 54.2 (+317%) | **13.0** |
| handler calls / MB |  | - | - | 188.7 |

### mixed, job ai

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **666.6** | 699.7 (+5.0%) | 695.1 (+4.3%) |
| FCT p99 (us) | smaller | 1147.6 (+34%) | **857.4** | 1089.7 (+27%) |
| JCT (us) | smaller | 1355.9 (+17%) | **1163.8** | 1204.5 (+3.5%) |
| goodput (Gbit/s) | larger | 2971.6 (-14%) | **3464.7** | 3343.8 (-3.5%) |
| Jain fairness | larger | 0.952 (-4.6%) | **0.998** | 0.996 (-0.2%) |

### mixed, job bg

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **1061.1** | 1370.6 (+29%) | 1327.2 (+25%) |
| FCT p99 (us) | smaller | **1097.3** | 1408.1 (+28%) | 1390.3 (+27%) |
| JCT (us) | smaller | **1097.3** | 1408.1 (+28%) | 1390.3 (+27%) |
| goodput (Gbit/s) | larger | **1467.8** | 1143.8 (-22%) | 1158.9 (-21%) |
| Jain fairness | larger | **1.000** | 0.995 (-0.4%) | 0.995 (-0.4%) |

### mixed, job storage

| metric | better | nscc | dctcp | pcm_dctcp |
|---|---|--:|--:|--:|
| unfinished flows | 0 | 0.0 | 0.0 | 0.0 |
| FCT p50 (us) | smaller | **1908.9** | 1936.9 (+1.5%) | 1954.5 (+2.4%) |
| FCT p99 (us) | smaller | **2050.0** | 2057.2 (+0.4%) | 2058.2 (+0.4%) |
| JCT (us) | smaller | **2050.0** | 2057.2 (+0.4%) | 2058.2 (+0.4%) |
| goodput (Gbit/s) | larger | **65.5** | 65.2 (-0.3%) | 65.2 (-0.4%) |
| Jain fairness | larger | **0.996** | 0.995 (-0.1%) | **0.996** |
