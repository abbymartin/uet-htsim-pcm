# AGENTS.md

Guidance for people and coding agents (Claude Code, Codex) working in this repo.
`CLAUDE.md` imports this file.

## What this is

A fork of the Ultra Ethernet Consortium's htsim network simulator, extended
with **Programmable Congestion Management (PCM)** from the UE specification
v1.1 (§3.6.18). A PCM congestion-control algorithm is a YAML file plus a small
C/C++ handler; the simulator delivers network signals to the handler and
applies the window it sets.

- [docs/pcm-plan.md](docs/pcm-plan.md): how PCM works here; **§7 lists every
  place we differ from the spec**.
- [docs/pcm-triggers.md](docs/pcm-triggers.md): how triggers decide when a
  handler runs.
- [docs/roadmap.md](docs/roadmap.md): phases 2–4 (more algorithms, AI-workload
  scenarios, data collection).
- [htsim/README.md](htsim/README.md): upstream htsim usage.

The spec and the PCM paper (Khalilov et al., arXiv:2610.13130) are not in the
repo; the spec's license doesn't allow redistributing it. Ask a teammate for
local copies, and never commit them.

## Layout

```
htsim/sim/                 simulator core; UEC transport in uec.{h,cpp}
htsim/sim/pcm/             PCM: YAML loader, triggers, runtime, handler API (uet_pcm.h)
htsim/sim/pcm/algorithms/  PCM algorithms: <name>.yaml + <name>.cpp
htsim/sim/pcm/tests/       PCM unit tests (googletest)
htsim/sim/datacenter/      main_uec.cpp (the htsim_uec binary), topologies,
                           connection matrices, validation scripts
```

## Build, run, test

Needs Linux or macOS (on Windows, use WSL), a C++17 compiler (g++ or clang),
CMake ≥ 3.16, bash and Python 3. The first configure needs `git` and network
access to fetch googletest and yaml-cpp.

From `htsim/sim/`:

```bash
cmake -S . -B build -DENABLE_TESTS=ON      # first time; fetches googletest, yaml-cpp
cmake --build build --parallel
(cd build && ctest)                        # unit tests, a few seconds
cd datacenter
./htsim_uec -tm connection_matrices/one.cm                           # one flow, default NSCC
./htsim_uec -tm connection_matrices/one.cm -sender_cc_only \
            -pcm ../pcm/algorithms/constant.yaml                    # the same with a PCM algorithm
```

- **Regression suite:** `datacenter/commit_check.sh` compares runs against
  baselines on `origin/main`. It takes about 11 minutes, so run it in the
  background, and **don't rebuild while it runs**: it uses the binary in
  `build/`. It regenerates `datacenter/validate_outputs/*.out`; those files
  should show no changes afterwards unless a change is meant to alter results.
  PCM runs are in `validate_pcm.txt`.
- **PCM checks:** `datacenter/pcm_exact_match.sh` (PCM ports of htsim's built-in
  algorithms send exactly like the originals) and `datacenter/pcm_groups_check.sh`
  (flow groups share one window). Both are part of `ctest`; a few seconds each.
- **PCM options:** `-pcm FILE`, `-pcm_ccc GROUP=FILE`, `-pcm_param NAME=VALUE` or
  `GROUP:NAME=VALUE` (both repeatable), `-pcm_units spec|native`, and
  `-pcm_handler_time_ns T` (each handler call takes T; 0 = immediate). Flows are
  put in a group with `ccc GROUP` on their connection-matrix line. See
  `./htsim_uec -help` and `docs/pcm-plan.md` §4.5.
- **Runs stop at `-end`, 1000 µs by default.** Flows still running then have no
  finish line; give long runs a larger `-end` (in µs).

## Working rules

- **Small commits**, each of which builds and passes `ctest`. Commits touching
  `uec.{h,cpp}` or `main_uec.cpp` also pass `commit_check.sh`.
- **Non-PCM behaviour must not change.** New code runs only for PCM flows.
- **The spec is a guide, not law.** Follow it by default, but don't implement
  spec features nothing here would use. When the spec is unclear or
  inconsistent, pick one behaviour (no flags for alternative readings).
- **Document every difference from the spec**, both as an item in
  `docs/pcm-plan.md` §7 and as a plain comment at the code: what the spec says,
  what we do, and why.
- **Plain wording.** Use terms from the spec, the paper or the code, define
  acronyms, and don't coin new jargon in docs, comments or commit messages.
- **Code style:** 4-space indent, `// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-`
  at the top of C++ files, no namespaces, `Pcm` prefix for PCM classes.

## Adding a PCM algorithm

1. Write `htsim/sim/pcm/algorithms/<name>.yaml` in the spec's format
   (§3.6.18.8.5). `constant.yaml` is the smallest example; the loader's error
   messages say what is missing or unsupported.
2. Write `<name>.cpp`:
   ```cpp
   #include "uet_pcm.h"
   PCM_ALGORITHM(<name>);        // must equal cc_algorithm.name
   PCM_SIGNAL(T_ACK);            // every YAML macro the handler uses
   PCM_VARIABLE(V_X);
   PCM_PARAMETER_UINT(P_Y);
   PCM_HANDLER(<name>) {
       uet_trigmask_t fired;
       uet_trigger_mask(&fired);
       if (fired & T_ACK) { /* uet_signal_get(...), uet_set_cwnd(...) */ }
   }
   ```
3. Add the `.cpp` to `htsim_pcm_algorithms` in `htsim/sim/pcm/CMakeLists.txt`.
   It must stay an object library: handlers register themselves at start-up,
   and a static library would let the linker drop them.
4. Add unit tests in `pcm/tests/` for the handler's logic;
   `pcm_dctcp_test.cpp` shows how to drive an algorithm with scripted ACKs and
   NACKs. If htsim has a built-in version of the algorithm, compare the two on
   the same matrices; an exact port can be added to `pcm_exact_match.sh`.
5. Run it on incast and permutation matrices, including a large incast
   (`incast_128.cm`): heavy trimming exposes mistakes that small runs hide.
   Then add a run to `datacenter/validate_pcm.txt`, with a `!tailFCT` bound
   about 5% above its result, and regenerate the baseline with
   `python3 validate.py validate_pcm.txt > validate_outputs/validate_pcm.out`.

Things that catch people out:
- Signal values are in the spec's units by default (time in 128 ns, ACK bytes in
  256 B, truncated); `-pcm_units native` gives picoseconds and bytes.
- Macros (`T_ACK`, `P_Y`, ...) only work inside a running handler.
- Signals accumulate forever (a SUM is a running total): keep the previous value
  in a variable and subtract it, or reset with `uet_signal_set()`.
- Triggers come only from the YAML file; handlers can't change them.
- A `uet_tx_cwnd` control with `initial_value: 0` keeps the simulator's starting
  window.
- Invalid API use (wrong variable type, a window of 0, ...) puts the flow's CCC
  into the ERROR state: it stops sending and a message is printed.
