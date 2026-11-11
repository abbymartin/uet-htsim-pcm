#!/usr/bin/env python3
"""Evaluate congestion-control algorithms on the PCM evaluation scenarios.

Runs htsim_uec for each scenario x algorithm x seed, in parallel, and writes:
  OUT/flows.csv     one row for each flow of each run
  OUT/summary.csv   one row for each job of each run (metrics: docs/pcm-eval.md)
  OUT/summary.md    the mean (and spread) over the seeds, as Markdown tables
  OUT/runs/...      the output of each run
It also prints the table in summary.md.

Examples (from htsim/sim/datacenter):
  ./pcm_eval.py
  ./pcm_eval.py --scenarios incast_8,mixed --seeds 1
  ./pcm_eval.py --algo pcm_dctcp='-pcm ../pcm/algorithms/dctcp.yaml' \\
                --algo pcm_dctcp_slow='-pcm ../pcm/algorithms/dctcp.yaml -pcm_handler_time_ns 1100'

Paths in algorithm options are relative to this directory.
"""

import argparse
import concurrent.futures
import csv
import os
import re
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MATRICES = os.path.join(HERE, "connection_matrices", "pcm", "eval")
MB = 1024 * 1024

# name: (end time in us, queue sampling period in us). The end time is about
# three times the slowest run seen; the period gives 100 or more samples.
SCENARIOS = {
    "incast_8": (3000, 5),
    "incast_32": (8000, 10),
    "incast_127": (30000, 50),
    "permutation": (3000, 2),
    "alltoall": (3000, 5),
    "mixed": (8000, 10),
}

ALGORITHMS = {
    "nscc": "-sender_cc_algo nscc",
    "dctcp": "-sender_cc_algo dctcp",
    "pcm_dctcp": "-pcm ../pcm/algorithms/dctcp.yaml",
}

COMMON = ["-sender_cc_only"]

RE_START = re.compile(r"^Flow \S+ flowId (\d+) .* starting at (\S+)$")
RE_FINISH = re.compile(r"^Flow \S+ flowId (\d+) .* finished at (\S+) ")
RE_TOTALS = re.compile(r"^New: (\d+) Rtx: (\d+) .* NACKs: (\d+) ")
RE_PCM = re.compile(r"^PCM: CCCs: \d+ Events: \d+ Calls: (\d+) .* Errors: (\d+)$")
RE_LINK = re.compile(r"^Link (\S+) Bytes (\d+) Gbps (\S+) Drops (\d+|-)$")
RE_QUEUE = re.compile(r"^(\S+) Type QUEUE_APPROX ID (\d+) Ev RANGE LastQ (-?\d+) "
                      r"MinQ -?\d+ MaxQ (-?\d+)")

SUMMARY_FIELDS = [
    "scenario", "algorithm", "seed", "job", "status", "flows", "unfinished",
    "fct_mean_us", "fct_p50_us", "fct_p95_us", "fct_p99_us", "fct_max_us", "jct_us",
    "goodput_gbps", "throughput_gbps", "jain", "rtt_samples", "rtt_mean_us",
    "rtt_p50_us", "rtt_p99_us", "new_pkts", "rtx_pkts", "rtx_pct", "trims",
    "drops", "util_max_pct", "util_max_at", "util_mean_pct",
    "q_max_bytes", "q_mean_bytes", "q_max_at", "pcm_calls", "pcm_calls_per_mb",
    "pcm_errors",
]
FLOW_FIELDS = ["scenario", "algorithm", "seed", "job", "id", "src", "dst",
               "size", "start_us", "finish_us", "fct_us"]


def read_matrix(path):
    """Return {id: (src, dst, size, job)}. A flow without `ccc` is in job "-"."""
    flows = {}
    with open(path) as f:
        for line in f:
            tokens = line.split()
            if not tokens or "->" not in tokens[0]:
                continue
            src, dst = (int(x) for x in tokens[0].split("->"))
            opts = dict(zip(tokens[1::2], tokens[2::2]))
            flows[int(opts["id"])] = (src, dst, int(opts["size"]), opts.get("ccc", "-"))
    return flows


def percentile(values, p):
    """Nearest-rank percentile: the smallest value with p% of values at or below it."""
    v = sorted(values)
    return v[max(1, (p * len(v) + 99) // 100) - 1]  # rank ceil(p * n / 100)


def jain(values):
    """Jain's fairness index: 1 when all values are equal, 1/n at worst."""
    return sum(values) ** 2 / (len(values) * sum(x * x for x in values))


def mean_sd(values):
    """Mean and sample standard deviation (None for fewer than 2 values)."""
    mean = sum(values) / len(values)
    if len(values) < 2:
        return mean, None
    return mean, (sum((x - mean) ** 2 for x in values) / (len(values) - 1)) ** 0.5


def parse_stdout(path):
    """Return starts and finishes ({id: us}), the run totals and PCM totals
    (None if missing), and the links [(name, bytes, Gbit/s, drops)] of
    -link_stats (drops None if the queue does not count them)."""
    starts, finishes, totals, pcm, links = {}, {}, None, None, []
    with open(path) as f:
        for line in f:
            if m := RE_START.match(line):
                starts[int(m[1])] = float(m[2])
            elif m := RE_FINISH.match(line):
                finishes[int(m[1])] = float(m[2])
            elif m := RE_TOTALS.match(line):
                totals = tuple(int(x) for x in m.groups())
            elif m := RE_PCM.match(line):
                pcm = tuple(int(x) for x in m.groups())
            elif m := RE_LINK.match(line):
                links.append((m[1], int(m[2]), float(m[3]),
                              None if m[4] == "-" else int(m[4])))
    return starts, finishes, totals, pcm, links


def read_rtts(path):
    """Return {flow id: [RTT in us]} from a -rtt_log file."""
    rtts = {}
    with open(path) as f:
        for line in f:
            flow, ps = line.split()
            rtts.setdefault(int(flow), []).append(int(ps) / 1e6)
    return rtts


def link_utilization(links, busy_us):
    """Return (highest %, its link, mean % over all links). The utilization of
    a link is the bytes that entered it divided by what its rate can carry in
    busy_us."""
    if not links or not busy_us:
        return None, None, None
    util = [(b * 8 / (gbps * 1e3 * busy_us) * 100, name) for name, b, gbps, _ in links]
    top = max(util, key=lambda u: u[0])
    return top[0], top[1], sum(u for u, _ in util) / len(util)


def switch_drops(links):
    """Return the packets that switch queues dropped (not trimmed), or None
    if a queue does not count them or there are no links."""
    if not links or any(d is None for *_, d in links):
        return None
    return sum(d for *_, d in links)


def parse_queues(run_dir, parse_output, busy_until_us):
    """Return (peak bytes, mean bytes, name) for the switch queue with the
    highest peak. The mean is over the samples up to busy_until_us. Host NIC
    queues (names starting with SRC) are not switch queues."""
    names = {}
    with open(os.path.join(run_dir, "idmap.txt")) as f:
        for line in f:
            parts = line.split(maxsplit=1)
            if len(parts) == 2:
                names[int(parts[0])] = parts[1].strip()
    peak, total, count = {}, {}, {}
    proc = subprocess.Popen([parse_output, "logout.dat", "-ascii"], cwd=run_dir,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    for line in proc.stdout:
        m = RE_QUEUE.match(line)
        if not m:
            continue
        qid = int(m[2])
        if names.get(qid, "").startswith("SRC"):
            continue
        peak[qid] = max(peak.get(qid, 0), int(m[4]))
        if float(m[1]) * 1e6 <= busy_until_us:
            total[qid] = total.get(qid, 0) + int(m[3])
            count[qid] = count.get(qid, 0) + 1
    if proc.wait() != 0:
        raise RuntimeError(f"parse_output failed in {run_dir}")
    if not peak:
        return None, None, None
    hot = max(peak, key=lambda q: (peak[q], -q))  # lowest id on a tie
    mean = total[hot] / count[hot] if count.get(hot) else None
    return peak[hot], mean, names.get(hot, str(hot))


def job_metrics(ids, flows, starts, finishes, rtts=None):
    """Metrics for one job. FCT statistics use the finished flows; JCT and
    goodput need all flows finished. RTT statistics use all samples of the
    job's flows ({id: [us]})."""
    done = [i for i in ids if i in finishes and i in starts]
    row = {"flows": len(ids), "unfinished": len(ids) - len(done)}
    if done:
        fct = [finishes[i] - starts[i] for i in done]
        row.update(fct_mean_us=sum(fct) / len(fct), fct_p50_us=percentile(fct, 50),
                   fct_p95_us=percentile(fct, 95), fct_p99_us=percentile(fct, 99),
                   fct_max_us=max(fct))
        row["jain"] = jain([flows[i][2] / t for i, t in zip(done, fct)])
    if done and len(done) == len(ids):
        jct = max(finishes[i] for i in ids) - min(starts[i] for i in ids)
        row["jct_us"] = jct
        row["goodput_gbps"] = sum(flows[i][2] for i in ids) * 8 / (jct * 1e3)
    samples = [x for i in ids for x in (rtts or {}).get(i, [])]
    if samples:
        row.update(rtt_samples=len(samples), rtt_mean_us=sum(samples) / len(samples),
                   rtt_p50_us=percentile(samples, 50), rtt_p99_us=percentile(samples, 99))
    return row


def run_one(task):
    """Run one simulation and return (summary rows, flow rows)."""
    scenario, algo, opts, seed, out, htsim, parse_output, queues = task
    end_us, sample_us = SCENARIOS[scenario]
    matrix = os.path.join(MATRICES, scenario + ".cm")
    run_dir = os.path.join(out, "runs", scenario, algo, f"seed{seed}")
    os.makedirs(run_dir, exist_ok=True)
    cmd = [htsim] + COMMON + absolute_paths(shlex.split(opts)) + [
        "-tm", matrix, "-end", str(end_us), "-seed", str(seed), "-o", "logout.dat",
        "-rtt_log", "rtt.txt", "-link_stats"]
    if queues:
        cmd += ["-log", "tor_downqueue", "-logtime_us", str(sample_us)]
    stdout_path = os.path.join(run_dir, "stdout.txt")
    with open(stdout_path, "w") as f:
        f.write("# " + shlex.join(cmd) + "\n")
        f.flush()
        rc = subprocess.run(cmd, cwd=run_dir, stdout=f, stderr=subprocess.STDOUT).returncode

    flows = read_matrix(matrix)
    starts, finishes, totals, pcm, links = parse_stdout(stdout_path)
    rtt_path = os.path.join(run_dir, "rtt.txt")
    rtts = read_rtts(rtt_path) if os.path.exists(rtt_path) else {}
    key = {"scenario": scenario, "algorithm": algo, "seed": seed}

    flow_rows = []
    for i, (src, dst, size, job) in sorted(flows.items()):
        s, e = starts.get(i), finishes.get(i)
        flow_rows.append(dict(key, job=job, id=i, src=src, dst=dst, size=size,
                              start_us=s, finish_us=e,
                              fct_us=e - s if s is not None and e is not None else None))

    run = dict(key, job="all", status="ok" if rc == 0 else f"exit {rc}")
    run.update(job_metrics(list(flows), flows, starts, finishes, rtts))
    if totals:
        new, rtx, nacks = totals
        run.update(new_pkts=new, rtx_pkts=rtx, trims=nacks,
                   rtx_pct=100 * rtx / new if new else 0.0)
        # Data sent, with retransmissions: goodput x (new + resent) / new. All
        # data packets but the last of each flow have the same size.
        if new and "goodput_gbps" in run:
            run["throughput_gbps"] = run["goodput_gbps"] * (new + rtx) / new
    # Until the last finish; until -end if a flow did not finish.
    busy_until = max(finishes.values()) if run["unfinished"] == 0 else end_us
    run["util_max_pct"], run["util_max_at"], run["util_mean_pct"] = \
        link_utilization(links, busy_until)
    run["drops"] = switch_drops(links)
    if pcm:
        calls, errors = pcm
        done_bytes = sum(flows[i][2] for i in finishes if i in flows)
        run.update(pcm_calls=calls, pcm_errors=errors,
                   pcm_calls_per_mb=calls / (done_bytes / MB) if done_bytes else None)
        if errors and rc == 0:
            run["status"] = f"{errors} CCC(s) in the ERROR state"
    logfile = os.path.join(run_dir, "logout.dat")
    if queues and rc == 0:
        try:
            q_max, q_mean, q_at = parse_queues(run_dir, parse_output, busy_until)
            run.update(q_max_bytes=q_max, q_mean_bytes=q_mean, q_max_at=q_at)
        except (OSError, RuntimeError) as e:
            run["status"] = f"queue log error: {e}"
    for path in (logfile, rtt_path):
        if os.path.exists(path):
            os.remove(path)  # large; the summary keeps what we use

    rows = [run]
    jobs = sorted({job for (_, _, _, job) in flows.values()})
    if jobs != ["-"]:
        for job in jobs:
            ids = [i for i, f in flows.items() if f[3] == job]
            rows.append(dict(key, job=job, status=run["status"],
                             **job_metrics(ids, flows, starts, finishes, rtts)))
    return rows, flow_rows


def absolute_paths(args):
    """Make the file names in -pcm FILE, -pcm_ccc GROUP=FILE and -topo FILE
    absolute (relative to this directory), as each run has its own directory."""
    out = []
    for i, a in enumerate(args):
        prev = args[i - 1] if i else ""
        if prev in ("-pcm", "-topo"):
            a = os.path.join(HERE, a)
        elif prev == "-pcm_ccc" and "=" in a:
            group, path = a.split("=", 1)
            a = group + "=" + os.path.join(HERE, path)
        out.append(a)
    return out


# (heading, summary.csv column, decimals) for the printed and Markdown tables.
# The rows of each summary table: label, summary field, digits, and which value
# is best ("min", "max", or None if no value is best).
TABLE_ROWS = [
    ("unfinished flows", "unfinished", 1, None),
    ("FCT mean (us)", "fct_mean_us", 1, "min"),
    ("FCT p50 (us)", "fct_p50_us", 1, "min"),
    ("FCT p95 (us)", "fct_p95_us", 1, "min"),
    ("FCT p99 (us)", "fct_p99_us", 1, "min"),
    ("JCT (us)", "jct_us", 1, "min"),
    ("goodput (Gbit/s)", "goodput_gbps", 1, "max"),
    ("throughput (Gbit/s)", "throughput_gbps", 1, None),
    ("Jain fairness", "jain", 3, "max"),
    ("RTT p50 (us)", "rtt_p50_us", 1, "min"),
    ("RTT p99 (us)", "rtt_p99_us", 1, "min"),
    ("retransmitted (%)", "rtx_pct", 2, "min"),
    ("trims", "trims", 0, "min"),
    ("drops", "drops", 0, "min"),
    ("busiest link (%)", "util_max_pct", 1, "max"),
    ("mean link (%)", "util_mean_pct", 2, None),
    ("peak queue (KB)", "q_max_bytes", 1, "min"),
    ("mean queue (KB)", "q_mean_bytes", 1, "min"),
    ("handler calls / MB", "pcm_calls_per_mb", 1, None),
]


def summary_tables(rows, scenarios, algos):
    """Return one table for each scenario and job: (scenario, job, lines).
    Each line is (label, digits, best, means, sds), with the mean and the
    sample standard deviation over the seeds for each algorithm (None if a
    seed has no value; sd None for one seed). Lines without values are left
    out."""
    groups = {}
    for r in rows:
        groups.setdefault((r["scenario"], r["job"]), {}).setdefault(
            r["algorithm"], []).append(r)
    order = lambda k: (scenarios.index(k[0]), k[1] != "all", k[1])
    tables = []
    for key in sorted(groups, key=order):
        lines = []
        for label, field, digits, best in TABLE_ROWS:
            means, sds = [], []
            for a in algos:
                vals = [r.get(field) for r in groups[key].get(a, [])]
                if not vals or any(v is None for v in vals):
                    means.append(None)
                    sds.append(None)
                    continue
                scale = 1024 if field.startswith("q_") else 1
                mean, sd = mean_sd([v / scale for v in vals])
                means.append(mean)
                sds.append(sd)
            if any(m is not None for m in means):
                lines.append((label, digits, best, means, sds))
        tables.append((*key, lines))
    return tables


def table_cells(digits, best, means, sds=None):
    """Format the means. The best value is in bold; each other value has its
    difference from the best in percent. A standard deviation over the seeds
    follows as "±sd"."""
    cells = [f"{m:.{digits}f}" if m is not None else "-" for m in means]
    known = [m for m in means if m is not None]
    if best is not None and len(known) >= 2:
        top = (min if best == "min" else max)(known)
        for i, m in enumerate(means):
            if m is None:
                continue
            if cells[i] == f"{top:.{digits}f}":
                cells[i] = f"**{cells[i]}**"
            elif top != 0:
                d = (m - top) / top * 100
                cells[i] += f" ({d:+.0f}%)" if abs(d) >= 10 else f" ({d:+.1f}%)"
    for i, sd in enumerate(sds or []):
        if sd is not None and means[i] is not None:
            # After the value, before the difference: "12.3 ±0.4 (+5%)".
            value, sep, diff = cells[i].partition(" (")
            cells[i] = f"{value} ±{sd:.{digits}f}" + (f" ({diff}" if sep else "")
    return cells


def markdown(tables, algos, seeds):
    out = [f"Mean over seeds {','.join(map(str, seeds))}. KB = 1024 bytes. "
           "Bold: the best value. " +
           ("±N: the standard deviation over the seeds. " if len(seeds) > 1 else "") +
           "(+N%): the difference from the best value. "
           "'-': no value for at least one seed.", ""]
    for scenario, job, lines in tables:
        out += [f"### {scenario}" + ("" if job == "all" else f", job {job}"), "",
                "| metric | better | " + " | ".join(algos) + " |",
                "|---|---|" + "--:|" * len(algos)]
        for label, digits, best, means, sds in lines:
            word = {"min": "smaller", "max": "larger", None: ""}[best]
            if label == "unfinished flows":
                word = "0"
                sds = None  # 0 for every seed when the run is valid
            out.append(f"| {label} | {word} | " +
                       " | ".join(table_cells(digits, best, means, sds)) + " |")
        out.append("")
    return "\n".join(out)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--scenarios", default=",".join(SCENARIOS),
                   help="comma-separated (default: all)")
    p.add_argument("--algo", action="append", metavar="NAME=OPTIONS",
                   help="htsim_uec options for an algorithm; repeatable. "
                        "Replaces the default set: " +
                        ", ".join(f"{k}='{v}'" for k, v in ALGORITHMS.items()))
    p.add_argument("--seeds", default="1,2,3", help="comma-separated (default: 1,2,3)")
    p.add_argument("--jobs", type=int, default=os.cpu_count(), help="parallel runs")
    p.add_argument("--out", default=os.path.join(HERE, "pcm_eval_out"),
                   help="output directory; a previous output there is replaced "
                        "(default: pcm_eval_out)")
    p.add_argument("--no-queues", action="store_true",
                   help="do not log queues (no q_* metrics; faster)")
    args = p.parse_args()

    scenarios = args.scenarios.split(",")
    for s in scenarios:
        if s not in SCENARIOS:
            p.error(f"unknown scenario {s}; known: {', '.join(SCENARIOS)}")
    try:
        seeds = [int(s) for s in args.seeds.split(",")]
    except ValueError:
        p.error(f"bad --seeds {args.seeds!r}: use integers, e.g. 1,2,3")
    # Each run has its own directory, so repeated values are not allowed.
    if len(set(scenarios)) != len(scenarios) or len(set(seeds)) != len(seeds):
        p.error("a scenario or seed is repeated")
    if args.jobs < 1:
        p.error("--jobs must be 1 or more")
    algos = ALGORITHMS
    if args.algo:
        algos = {}
        for a in args.algo:
            name, sep, opts = a.partition("=")
            if not sep or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", name) or name in algos:
                p.error(f"bad or repeated --algo {a!r}: use NAME=OPTIONS, NAME made of "
                        "letters, digits, '_', '.', '-' and not starting with '.' or '-'")
            algos[name] = opts

    htsim = os.path.join(HERE, "htsim_uec")
    if not os.access(htsim, os.X_OK):
        sys.exit("htsim_uec not found; build it first (see AGENTS.md)")
    parse_output = os.path.join(os.path.dirname(os.path.realpath(htsim)), "..", "parse_output")
    if not args.no_queues and not os.access(parse_output, os.X_OK):
        sys.exit(f"{parse_output} not found; build it, or use --no-queues")

    marker = os.path.join(args.out, ".pcm_eval")
    if os.path.isdir(args.out) and os.listdir(args.out):
        if not os.path.exists(marker):
            sys.exit(f"{args.out} is not empty and is not a pcm_eval output; not replaced")
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)
    open(marker, "w").close()
    tasks = [(s, a, o, seed, args.out, htsim, parse_output, not args.no_queues)
             for s in scenarios for a, o in algos.items() for seed in seeds]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run_one, tasks))

    summary = [r for rows, _ in results for r in rows]
    for name, fields, data in (("summary.csv", SUMMARY_FIELDS, summary),
                               ("flows.csv", FLOW_FIELDS,
                                [r for _, flows in results for r in flows])):
        with open(os.path.join(args.out, name), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            w.writerows(data)

    text = markdown(summary_tables(summary, scenarios, algos), algos, seeds)
    with open(os.path.join(args.out, "summary.md"), "w") as f:
        f.write(text)
    print(text)
    failed = [r for r in summary if r["job"] == "all" and r["status"] != "ok"]
    for r in failed:
        print(f"FAILED: {r['scenario']} {r['algorithm']} seed {r['seed']}: {r['status']}"
              f" (see {os.path.join(args.out, 'runs', r['scenario'], r['algorithm'])})")
    print(f"Results in {args.out}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
