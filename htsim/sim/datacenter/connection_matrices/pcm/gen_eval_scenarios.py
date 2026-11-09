#!/usr/bin/env python3
"""Generate the connection matrices for the PCM evaluation (docs/pcm-eval.md).

All scenarios use 128 hosts: the default 3-tier fat tree of htsim_uec at
100 Gbit/s, with 4 hosts per ToR switch. Each scenario has its own fixed random
seed, so this script always writes the same files.

Usage: ./gen_eval_scenarios.py [OUTPUT_DIR]   (default: eval/ next to this script)
"""

import os
import random
import sys

NODES = 128
KB = 1024
MB = 1024 * 1024


def incast(rng, senders, size):
    """`senders` hosts send `size` bytes each to one host, all at time 0."""
    hosts = rng.sample(range(NODES), senders + 1)
    dst = hosts[0]
    return [(src, dst, 0, size, None) for src in hosts[1:]]


def permutation(rng, size):
    """Each host sends `size` bytes to one other host, and receives one flow."""
    dsts = list(range(NODES))
    while True:
        rng.shuffle(dsts)
        if all(src != dst for src, dst in enumerate(dsts)):
            break
    return [(src, dst, 0, size, None) for src, dst in enumerate(dsts)]


def alltoall(hosts, group_size, size, job=None):
    """Split `hosts` into groups of `group_size`. In each group, every host
    sends `size` bytes to every other host, all at time 0."""
    flows = []
    for g in range(0, len(hosts), group_size):
        group = hosts[g:g + group_size]
        flows += [(s, d, 0, size, job) for s in group for d in group if s != d]
    return flows


def mixed(rng):
    """Three jobs, each its own PCM flow group (`ccc`), on shared links:
    - ai:      64 hosts, all-to-all in 4 groups of 16, 512 KB per pair;
    - storage: 16 of the other hosts send 1 MB each to one ai host;
    - bg:      each of the last 48 hosts sends one 4 MB flow to a different
               ai host (not the storage receiver).
    """
    hosts = list(range(NODES))
    rng.shuffle(hosts)
    ai, storage, bg = hosts[:64], hosts[64:80], hosts[80:]
    receiver = ai[0]
    flows = alltoall(ai, 16, 512 * KB, "ai")
    flows += [(src, receiver, 0, 1 * MB, "storage") for src in storage]
    flows += [(src, dst, 0, 4 * MB, "bg")
              for src, dst in zip(bg, rng.sample(ai[1:], len(bg)))]
    return flows


# name: (seed, description, flow generator)
SCENARIOS = {
    "incast_8": (1, "8-to-1 incast, 1 MB per sender",
                 lambda rng: incast(rng, 8, 1 * MB)),
    "incast_32": (2, "32-to-1 incast, 1 MB per sender",
                  lambda rng: incast(rng, 32, 1 * MB)),
    "incast_127": (3, "127-to-1 incast, 1 MB per sender",
                   lambda rng: incast(rng, 127, 1 * MB)),
    "permutation": (4, "permutation of all 128 hosts, 2 MB per flow",
                    lambda rng: permutation(rng, 2 * MB)),
    "alltoall": (5, "all-to-all in 8 groups of 16 hosts, 512 KB per pair",
                 lambda rng: alltoall(rng.sample(range(NODES), NODES), 16, 512 * KB)),
    "mixed": (6, "jobs ai (all-to-all), storage (16-to-1 incast) and bg (4 MB flows)",
              mixed),
}


def write(path, name, seed, description, flows):
    with open(path, "w") as f:
        f.write(f"# PCM evaluation scenario {name}: {description}.\n")
        f.write(f"# Written by gen_eval_scenarios.py (seed {seed}). Do not edit.\n")
        f.write(f"Nodes {NODES}\n")
        f.write(f"Connections {len(flows)}\n")
        for i, (src, dst, start, size, job) in enumerate(flows, 1):
            assert src != dst
            line = f"{src}->{dst} id {i} start {start} size {size}"
            if job:
                line += f" ccc {job}"
            f.write(line + "\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "eval")
    os.makedirs(out, exist_ok=True)
    for name, (seed, description, gen) in SCENARIOS.items():
        write(os.path.join(out, name + ".cm"), name, seed, description,
              gen(random.Random(seed)))


if __name__ == "__main__":
    main()
