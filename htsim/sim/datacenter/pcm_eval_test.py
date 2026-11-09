#!/usr/bin/env python3
"""Tests for pcm_eval.py and the PCM evaluation scenarios (docs/pcm-eval.md).

Usage: ./pcm_eval_test.py   (part of ctest; the last tests run htsim_uec)
"""

import csv
import filecmp
import os
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pcm_eval  # noqa: E402

GENERATOR = os.path.join(HERE, "connection_matrices", "pcm", "gen_eval_scenarios.py")


class Metrics(unittest.TestCase):
    def test_percentile_nearest_rank(self):
        v = list(range(1, 101))
        self.assertEqual(pcm_eval.percentile(v, 50), 50)
        self.assertEqual(pcm_eval.percentile(v, 99), 99)
        self.assertEqual(pcm_eval.percentile(v, 100), 100)
        self.assertEqual(pcm_eval.percentile([3, 1, 2], 50), 2)
        self.assertEqual(pcm_eval.percentile([7], 99), 7)

    def test_jain(self):
        self.assertAlmostEqual(pcm_eval.jain([5, 5, 5, 5]), 1.0)
        self.assertAlmostEqual(pcm_eval.jain([1, 0, 0, 0]), 0.25)
        self.assertAlmostEqual(pcm_eval.jain([1, 2]), 0.9)

    def test_job_metrics(self):
        # id: (src, dst, size, job)
        flows = {1: (0, 9, 1000, "a"), 2: (1, 9, 3000, "a"), 3: (2, 9, 1000, "a")}
        starts = {1: 0.0, 2: 10.0, 3: 0.0}
        finishes = {1: 20.0, 2: 40.0, 3: 30.0}
        r = pcm_eval.job_metrics([1, 2, 3], flows, starts, finishes)
        self.assertEqual(r["unfinished"], 0)
        self.assertEqual(r["fct_max_us"], 30.0)
        self.assertEqual(r["fct_p50_us"], 30.0)  # FCTs 20, 30, 30
        self.assertEqual(r["jct_us"], 40.0)
        self.assertAlmostEqual(r["goodput_gbps"], 5000 * 8 / 40e3)
        # A flow that did not finish: FCT statistics of the others, no JCT.
        del finishes[2]
        r = pcm_eval.job_metrics([1, 2, 3], flows, starts, finishes)
        self.assertEqual(r["unfinished"], 1)
        self.assertEqual(r["fct_max_us"], 30.0)
        self.assertNotIn("jct_us", r)
        self.assertNotIn("goodput_gbps", r)

    def test_absolute_paths(self):
        args = pcm_eval.absolute_paths(
            ["-pcm", "a.yaml", "-pcm_ccc", "g=b.yaml", "-pcm_param", "g:P=1", "-cwnd", "10"])
        self.assertEqual(args, ["-pcm", os.path.join(HERE, "a.yaml"),
                                "-pcm_ccc", "g=" + os.path.join(HERE, "b.yaml"),
                                "-pcm_param", "g:P=1", "-cwnd", "10"])


class Scenarios(unittest.TestCase):
    def test_generator_reproduces_matrices(self):
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([sys.executable, GENERATOR, tmp], check=True)
            names = sorted(os.listdir(tmp))
            self.assertEqual(names, sorted(s + ".cm" for s in pcm_eval.SCENARIOS))
            for n in names:
                self.assertTrue(filecmp.cmp(os.path.join(tmp, n),
                                            os.path.join(pcm_eval.MATRICES, n), shallow=False),
                                f"{n} differs; run gen_eval_scenarios.py")

    def test_matrices(self):
        for s in pcm_eval.SCENARIOS:
            path = os.path.join(pcm_eval.MATRICES, s + ".cm")
            flows = pcm_eval.read_matrix(path)
            with open(path) as f:
                declared = next(int(l.split()[1]) for l in f if l.startswith("Connections"))
            self.assertEqual(len(flows), declared, s)
            self.assertEqual(sorted(flows), list(range(1, declared + 1)), s)
            # Flows of one PCM flow group between the same two hosts would share
            # a CCC; the scenarios avoid that, so built-in algorithms compare.
            keys = [(src, dst, job) for (src, dst, _, job) in flows.values()]
            self.assertEqual(len(keys), len(set(keys)), s)


class Inputs(unittest.TestCase):
    def test_rejects_inputs_that_share_a_run_directory(self):
        for args in (["--seeds", "1,1"], ["--scenarios", "incast_8,incast_8"],
                     ["--algo", "..=-pcm x.yaml"], ["--seeds", "x"], ["--jobs", "0"]):
            r = subprocess.run([sys.executable, os.path.join(HERE, "pcm_eval.py"), *args],
                               capture_output=True)
            self.assertEqual(r.returncode, 2, args)  # argparse error, before any run


class Runs(unittest.TestCase):
    def run_eval(self, *args):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        out = os.path.join(tmp.name, "out")
        subprocess.run([sys.executable, os.path.join(HERE, "pcm_eval.py"), "--seeds", "1",
                        "--out", out, *args], check=True, stdout=subprocess.DEVNULL)
        with open(os.path.join(out, "summary.csv")) as f:
            return out, list(csv.DictReader(f))

    def test_incast(self):
        _, rows = self.run_eval("--scenarios", "incast_8",
                                "--algo", "pcm_dctcp=-pcm ../pcm/algorithms/dctcp.yaml")
        self.assertEqual(len(rows), 1)
        r = rows[0]
        self.assertEqual((r["job"], r["status"], r["flows"], r["unfinished"]),
                         ("all", "ok", "8", "0"))
        self.assertGreater(float(r["jct_us"]), 0)
        self.assertGreater(int(r["q_max_bytes"]), 0)
        self.assertGreater(int(r["pcm_calls"]), 0)
        self.assertEqual(r["pcm_errors"], "0")

    def test_mixed_with_one_algorithm_per_group(self):
        out, rows = self.run_eval(
            "--scenarios", "mixed", "--no-queues", "--algo",
            "mix=-pcm ../pcm/algorithms/dctcp.yaml -pcm_ccc bg=../pcm/algorithms/constant.yaml")
        self.assertEqual([r["job"] for r in rows], ["all", "ai", "bg", "storage"])
        self.assertEqual([r["unfinished"] for r in rows], ["0"] * 4)
        self.assertEqual(rows[0]["q_max_bytes"], "")
        with open(os.path.join(out, "runs", "mixed", "mix", "seed1", "stdout.txt")) as f:
            self.assertIn("PCM: group bg: algorithm constant", f.read())


if __name__ == "__main__":
    unittest.main(verbosity=2)
