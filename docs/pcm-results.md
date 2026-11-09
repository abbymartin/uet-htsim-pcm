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

## 2. How to read the table

Each row is the mean over the seeds. Rows with job `all` are the full run. In
`mixed`, there is also a row for each job (`ai`, `storage`, `bg`).
[pcm-eval.md](pcm-eval.md) §2 gives the exact definitions.

Scale: a 100 Gbit/s link sends 1 MB of data in about 85 µs (with headers),
and carries at most about 98 Gbit/s of data. A switch queue holds 174 KB of
data (43 packets of 4150 bytes). A full queue adds about 14 µs of delay, the
same as the base RTT (round-trip time).

| Column | What it means | Better | Scale |
|---|---|---|---|
| `unfin` | Flows that did not finish before the end of the run. | 0 | Must be 0. Above 0, the other numbers are not complete. |
| `fct_p50` | Flow completion time (FCT) in µs of the median flow: half of the flows finish faster. | Smaller | A flow alone sends 1 MB in about 90 µs. When flows share a link fairly, they all finish near the best `jct` (below). |
| `fct_p99` | FCT of the flow at 99%: only 1% of flows are slower. The "tail". | Smaller | Between `fct_p50` and `jct`. A large gap to `fct_p50` means that some flows got less bandwidth. |
| `fct_max` | FCT of the slowest flow. | Smaller | Equal to `fct_p99`, or a little more. |
| `jct` | Job completion time in µs: from the first start to the last finish. All flows start at 0, so it equals `fct_max`. | Smaller | At least the best possible job time (below). |
| `Gbit/s` | Goodput: data of the job divided by `jct`. This is the total of all flows of the job, not the rate of one flow. | Larger | One receiver (incast, `storage`): at most about 98. `permutation`, `alltoall`: at most about 12,600 (128 links × 98). |
| `jain` | Jain's fairness index over the rate of each flow. | Larger | 1 = all flows get the same rate. 0.99 or more is fair. Compare only rows of the same job. In `mixed`/`all` it is low because the flow sizes are different. |
| `rtx%` | Packets sent again, as a percentage of new packets. | Smaller | 0 is best. 20% = 1 packet in 5 is sent again. Above 100%, there are more resent packets than new packets. |
| `trims` | Packets that a full switch queue cut to a header (each causes a NACK and a resend). | Smaller | 0 is best. A 1 MB flow has about 257 packets: an 8-to-1 incast of 1 MB has about 2,060 packets, so 400 trims is about 1 in 5. |
| `qmax_KB` | Highest occupancy of a switch queue, in KB. | Smaller | At most about 174 for data. 174 means that the queue was full and trimming started. More than 174 is possible: trimmed headers and control packets are in a separate queue that is counted too. |
| `qmean_KB` | Mean occupancy of the queue with the highest peak. | Smaller | 0 to about 174. Each 12.5 KB adds 1 µs of delay to each packet that goes through the queue. |
| `calls/MB` | PCM handler calls for each MB of data. Only for PCM algorithms. | Smaller, if the other results stay the same | Each call is work for the NIC (network interface card). About 257 means one call for each data packet. |

Best possible job time (`jct`): the time to send all data over the busiest
link, at 100 Gbit/s, with no losses. Compare `jct` with it: 10% more means 10%
slower than the best.

| Scenario | Busiest link | Best `jct` (µs) |
|---|---|--:|
| `incast_8` | 8 MB into the receiver | about 680 |
| `incast_32` | 32 MB into the receiver | about 2,730 |
| `incast_127` | 127 MB into the receiver | about 10,830 |
| `permutation` | 2 MB on each host link | about 180 |
| `alltoall` | 7.5 MB into each receiver | about 640 |
| `mixed` (all, `storage`) | 23.5 MB into the `storage` receiver | about 2,000 |
| `mixed` (`ai`, `bg`) | up to 11.5 MB into an `ai` host that also gets a `bg` flow | about 980 |

## 3. Table

Mean over seeds 1,2,3. Times in us; KB = 1024 bytes; '-' means no value for at least one seed.

| scenario | job | algorithm | unfin | fct_p50 | fct_p99 | fct_max | jct | Gbit/s | jain | rtx% | trims | qmax_KB | qmean_KB | calls/MB |
|---|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| incast_8 | all | nscc | 0.0 | 664.9 | 723.2 | 723.2 | 723.2 | 92.8 | 0.997 | 20.61 | 424 | 174.9 | 127.0 | - |
| incast_8 | all | dctcp | 0.0 | 665.4 | 726.5 | 726.5 | 726.5 | 92.4 | 0.995 | 19.41 | 399 | 174.9 | 63.0 | - |
| incast_8 | all | pcm_dctcp | 0.0 | 673.8 | 723.1 | 723.1 | 723.1 | 92.8 | 0.996 | 28.13 | 578 | 174.8 | 57.0 | 242.3 |
| incast_32 | all | nscc | 0.0 | 2515.0 | 2769.7 | 2769.7 | 2769.7 | 96.9 | 0.992 | 25.13 | 2066 | 176.2 | 143.3 | - |
| incast_32 | all | dctcp | 0.0 | 2604.9 | 2771.9 | 2771.9 | 2771.9 | 96.8 | 0.995 | 23.19 | 1907 | 176.6 | 91.4 | - |
| incast_32 | all | pcm_dctcp | 0.0 | 2634.5 | 2788.0 | 2788.0 | 2788.0 | 96.3 | 0.990 | 40.82 | 3357 | 176.2 | 89.3 | 326.6 |
| incast_127 | all | nscc | 0.0 | 10893.4 | 11051.3 | 11068.9 | 11068.9 | 96.2 | 0.979 | 126.82 | 41393 | 325.7 | 169.9 | - |
| incast_127 | all | dctcp | 0.0 | 10873.6 | 11057.2 | 11065.0 | 11065.0 | 96.3 | 0.979 | 127.37 | 41572 | 346.5 | 169.6 | - |
| incast_127 | all | pcm_dctcp | 0.0 | 10910.6 | 11097.1 | 11109.8 | 11109.8 | 95.9 | 0.981 | 148.35 | 48420 | 306.0 | 169.6 | 637.7 |
| permutation | all | nscc | 0.0 | 201.3 | 211.3 | 211.9 | 211.9 | 10136.0 | 0.999 | 0.00 | 0 | 145.9 | 43.6 | - |
| permutation | all | dctcp | 0.0 | 205.6 | 233.3 | 236.8 | 236.8 | 9069.7 | 0.997 | 0.00 | 0 | 140.6 | 29.9 | - |
| permutation | all | pcm_dctcp | 0.0 | 296.9 | 441.8 | 448.7 | 448.7 | 4786.8 | 0.948 | 0.00 | 0 | 120.3 | 8.1 | 87.6 |
| alltoall | all | nscc | 0.0 | 663.0 | 713.4 | 753.2 | 753.2 | 10693.7 | 0.999 | 0.65 | 1620 | 176.3 | 50.1 | - |
| alltoall | all | dctcp | 0.0 | 673.3 | 705.6 | 709.0 | 709.0 | 11358.3 | 1.000 | 1.63 | 4028 | 177.1 | 59.4 | - |
| alltoall | all | pcm_dctcp | 0.0 | 670.5 | 731.5 | 814.4 | 814.4 | 9892.3 | 0.999 | 0.90 | 2218 | 174.7 | 62.7 | 122.4 |
| mixed | all | nscc | 0.0 | 672.1 | 1883.9 | 2050.0 | 2050.0 | 2815.4 | 0.658 | 5.08 | 9002 | 175.4 | 140.2 | - |
| mixed | all | dctcp | 0.0 | 700.9 | 1905.5 | 2057.2 | 2057.2 | 2805.5 | 0.742 | 4.86 | 8607 | 177.1 | 54.2 | - |
| mixed | all | pcm_dctcp | 0.0 | 697.3 | 1915.3 | 2058.2 | 2058.2 | 2804.2 | 0.726 | 4.64 | 8161 | 177.1 | 13.0 | 188.7 |
| mixed | ai | nscc | 0.0 | 666.6 | 1147.6 | 1355.9 | 1355.9 | 2971.6 | 0.952 | - | - | - | - | - |
| mixed | ai | dctcp | 0.0 | 699.7 | 857.4 | 1163.8 | 1163.8 | 3464.7 | 0.998 | - | - | - | - | - |
| mixed | ai | pcm_dctcp | 0.0 | 695.1 | 1089.7 | 1204.5 | 1204.5 | 3343.8 | 0.996 | - | - | - | - | - |
| mixed | bg | nscc | 0.0 | 1061.1 | 1097.3 | 1097.3 | 1097.3 | 1467.8 | 1.000 | - | - | - | - | - |
| mixed | bg | dctcp | 0.0 | 1370.6 | 1408.1 | 1408.1 | 1408.1 | 1143.8 | 0.995 | - | - | - | - | - |
| mixed | bg | pcm_dctcp | 0.0 | 1327.2 | 1390.3 | 1390.3 | 1390.3 | 1158.9 | 0.995 | - | - | - | - | - |
| mixed | storage | nscc | 0.0 | 1908.9 | 2050.0 | 2050.0 | 2050.0 | 65.5 | 0.996 | - | - | - | - | - |
| mixed | storage | dctcp | 0.0 | 1936.9 | 2057.2 | 2057.2 | 2057.2 | 65.2 | 0.995 | - | - | - | - | - |
| mixed | storage | pcm_dctcp | 0.0 | 1954.5 | 2058.2 | 2058.2 | 2058.2 | 65.2 | 0.996 | - | - | - | - | - |
