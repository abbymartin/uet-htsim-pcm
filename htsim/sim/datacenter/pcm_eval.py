#!/usr/bin/env python3
"""Evaluate congestion-control algorithms on the PCM evaluation scenarios.

Runs htsim_uec for each scenario x algorithm x seed, in parallel, and writes:
  OUT/flows.csv     one row for each flow of each run
  OUT/summary.csv   one row for each job of each run (metrics: docs/pcm-eval.md)
  OUT/summary.md    the mean over the seeds, as a Markdown table
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
RE_QUEUE = re.compile(r"^(\S+) Type QUEUE_APPROX ID (\d+) Ev RANGE LastQ (-?\d+) "
                      r"MinQ -?\d+ MaxQ (-?\d+)")

SUMMARY_FIELDS = [
    "scenario", "algorithm", "seed", "job", "status", "flows", "unfinished",
    "fct_mean_us", "fct_p50_us", "fct_p99_us", "fct_max_us", "jct_us",
    "goodput_gbps", "jain", "new_pkts", "rtx_pkts", "rtx_pct", "trims",
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


def parse_stdout(path):
    """Return starts and finishes ({id: us}) and the run totals (None if missing)."""
    starts, finishes, totals, pcm = {}, {}, None, None
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
    return starts, finishes, totals, pcm


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


def job_metrics(ids, flows, starts, finishes):
    """Metrics for one job. FCT statistics use the finished flows; JCT and
    goodput need all flows finished."""
    done = [i for i in ids if i in finishes and i in starts]
    row = {"flows": len(ids), "unfinished": len(ids) - len(done)}
    if done:
        fct = [finishes[i] - starts[i] for i in done]
        row.update(fct_mean_us=sum(fct) / len(fct), fct_p50_us=percentile(fct, 50),
                   fct_p99_us=percentile(fct, 99), fct_max_us=max(fct))
        row["jain"] = jain([flows[i][2] / t for i, t in zip(done, fct)])
    if done and len(done) == len(ids):
        jct = max(finishes[i] for i in ids) - min(starts[i] for i in ids)
        row["jct_us"] = jct
        row["goodput_gbps"] = sum(flows[i][2] for i in ids) * 8 / (jct * 1e3)
    return row


def run_one(task):
    """Run one simulation and return (summary rows, flow rows)."""
    scenario, algo, opts, seed, out, htsim, parse_output, queues = task
    end_us, sample_us = SCENARIOS[scenario]
    matrix = os.path.join(MATRICES, scenario + ".cm")
    run_dir = os.path.join(out, "runs", scenario, algo, f"seed{seed}")
    os.makedirs(run_dir, exist_ok=True)
    cmd = [htsim] + COMMON + absolute_paths(shlex.split(opts)) + [
        "-tm", matrix, "-end", str(end_us), "-seed", str(seed), "-o", "logout.dat"]
    if queues:
        cmd += ["-log", "tor_downqueue", "-logtime_us", str(sample_us)]
    stdout_path = os.path.join(run_dir, "stdout.txt")
    with open(stdout_path, "w") as f:
        f.write("# " + shlex.join(cmd) + "\n")
        f.flush()
        rc = subprocess.run(cmd, cwd=run_dir, stdout=f, stderr=subprocess.STDOUT).returncode

    flows = read_matrix(matrix)
    starts, finishes, totals, pcm = parse_stdout(stdout_path)
    key = {"scenario": scenario, "algorithm": algo, "seed": seed}

    flow_rows = []
    for i, (src, dst, size, job) in sorted(flows.items()):
        s, e = starts.get(i), finishes.get(i)
        flow_rows.append(dict(key, job=job, id=i, src=src, dst=dst, size=size,
                              start_us=s, finish_us=e,
                              fct_us=e - s if s is not None and e is not None else None))

    run = dict(key, job="all", status="ok" if rc == 0 else f"exit {rc}")
    run.update(job_metrics(list(flows), flows, starts, finishes))
    if totals:
        new, rtx, nacks = totals
        run.update(new_pkts=new, rtx_pkts=rtx, trims=nacks,
                   rtx_pct=100 * rtx / new if new else 0.0)
    if pcm:
        calls, errors = pcm
        done_bytes = sum(flows[i][2] for i in finishes if i in flows)
        run.update(pcm_calls=calls, pcm_errors=errors,
                   pcm_calls_per_mb=calls / (done_bytes / MB) if done_bytes else None)
        if errors and rc == 0:
            run["status"] = f"{errors} CCC(s) in the ERROR state"
    logfile = os.path.join(run_dir, "logout.dat")
    if queues and rc == 0:
        # Until the last finish; until -end if a flow did not finish.
        busy_until = max(finishes.values()) if run["unfinished"] == 0 else end_us
        try:
            q_max, q_mean, q_at = parse_queues(run_dir, parse_output, busy_until)
            run.update(q_max_bytes=q_max, q_mean_bytes=q_mean, q_max_at=q_at)
        except (OSError, RuntimeError) as e:
            run["status"] = f"queue log error: {e}"
    if os.path.exists(logfile):
        os.remove(logfile)  # large; the summary keeps what we use

    rows = [run]
    jobs = sorted({job for (_, _, _, job) in flows.values()})
    if jobs != ["-"]:
        for job in jobs:
            ids = [i for i, f in flows.items() if f[3] == job]
            rows.append(dict(key, job=job, status=run["status"],
                             **job_metrics(ids, flows, starts, finishes)))
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
TABLE_COLUMNS = [
    ("unfin", "unfinished", 1), ("fct_p50", "fct_p50_us", 1),
    ("fct_p99", "fct_p99_us", 1), ("fct_max", "fct_max_us", 1),
    ("jct", "jct_us", 1), ("Gbit/s", "goodput_gbps", 1), ("jain", "jain", 3),
    ("rtx%", "rtx_pct", 2), ("trims", "trims", 0),
    ("qmax_KB", "q_max_bytes", 1), ("qmean_KB", "q_mean_bytes", 1),
    ("calls/MB", "pcm_calls_per_mb", 1)]


def summary_table(rows, scenarios, algos):
    """Return the table rows: scenario, job, algorithm, then the mean over the
    seeds of each column in TABLE_COLUMNS ('-' if a seed has no value)."""
    groups = {}
    for r in rows:
        groups.setdefault((r["scenario"], r["job"], r["algorithm"]), []).append(r)
    order = lambda k: (scenarios.index(k[0]), k[1] != "all", k[1], list(algos).index(k[2]))
    table = []
    for key, rs in sorted(groups.items(), key=lambda g: order(g[0])):
        cells = list(key)
        for _, field, digits in TABLE_COLUMNS:
            vals = [r.get(field) for r in rs]
            if any(v is None for v in vals):
                cells.append("-")
                continue
            mean = sum(vals) / len(vals)
            if field.startswith("q_"):
                mean /= 1024
            cells.append(f"{mean:.{digits}f}")
        table.append(cells)
    return table


def table_note(seeds):
    return (f"Mean over seeds {','.join(map(str, seeds))}. Times in us; KB = 1024 bytes; "
            "'-' means no value for at least one seed.")


def print_table(table, seeds):
    print(table_note(seeds))
    header = f"{'scenario':12} {'job':8} {'algorithm':16}" + "".join(
        f" {c[0]:>9}" for c in TABLE_COLUMNS)
    print(header)
    print("-" * len(header))
    for cells in table:
        print(f"{cells[0]:12} {cells[1]:8} {cells[2]:16}" + "".join(f" {c:>9}" for c in cells[3:]))


def write_markdown(path, table, seeds):
    headings = ["scenario", "job", "algorithm"] + [c[0] for c in TABLE_COLUMNS]
    with open(path, "w") as f:
        f.write(table_note(seeds) + "\n\n")
        f.write("| " + " | ".join(headings) + " |\n")
        f.write("|" + "---|" * 3 + "--:|" * len(TABLE_COLUMNS) + "\n")
        for cells in table:
            f.write("| " + " | ".join(cells) + " |\n")


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

    table = summary_table(summary, scenarios, algos)
    write_markdown(os.path.join(args.out, "summary.md"), table, seeds)
    print_table(table, seeds)
    failed = [r for r in summary if r["job"] == "all" and r["status"] != "ok"]
    for r in failed:
        print(f"FAILED: {r['scenario']} {r['algorithm']} seed {r['seed']}: {r['status']}"
              f" (see {os.path.join(args.out, 'runs', r['scenario'], r['algorithm'])})")
    print(f"Results in {args.out}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
