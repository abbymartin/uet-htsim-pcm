#!/usr/bin/env bash
#
# Checks PCM flow groups (`ccc <group>` in a connection matrix,
# docs/pcm-plan.md §4.5) with two 2 MB flows from the same NIC to the same NIC,
# each limited to a 10-packet window, well below the bandwidth-delay product:
#
#   - In different groups, they behave exactly like flows without a group:
#     each has its own CCC and window.
#   - In the same group, they share one CCC and one window, so each takes
#     about twice as long, and they take turns, so they finish together.
#   - -pcm_ccc and GROUP:NAME=VALUE give a group its own algorithm and
#     parameters.
#
# Usage: ./pcm_groups_check.sh   (exits non-zero if a check fails)

cd "$(dirname "${BASH_SOURCE[0]}")"

if [ ! -x ./htsim_uec ]; then
    echo "htsim_uec not found; build it first (see AGENTS.md)"
    exit 1
fi

algos=../pcm/algorithms
cms=connection_matrices/pcm
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

run() {  # run <matrix> <output> [extra options]
    local cm=$1 out=$2
    shift 2
    ./htsim_uec -tm "$cms/$cm" -sender_cc_only -cwnd 10 -end 5000 \
        -pcm "$algos/constant.yaml" "$@" > "$tmp/$out" 2>&1
}

check() {  # check <description> <condition result: 0 = pass>
    if [ "$2" -eq 0 ]; then
        echo "PASS  $1"
    else
        echo "FAIL  $1"
        failures=$((failures + 1))
    fi
}

fcts() {  # flow completion times in µs, one per line
    grep "finished at" "$1" | sed 's/.* finished at \([0-9.]*\) .*/\1/'
}

# Remove what differs between grouped and ungrouped runs: PCM fields and lines,
# and the lines naming the connection matrix file.
normalize() {
    grep -v -e '^PCM:' -e "$cms/" "$1" | sed 's/ pcm_group .*$//; s/ pcm_events .*$//'
}

run pair.cm private.txt
run pair_two_groups.cm two_groups.txt
run pair_group.cm group.txt

diff <(normalize "$tmp/private.txt") <(normalize "$tmp/two_groups.txt") > /dev/null
check "flows in different groups behave like flows without a group" $?

grep -q "^PCM: CCCs: 1 " "$tmp/group.txt"
check "flows in the same group share one CCC" $?

read -r p1 p2 <<< "$(fcts "$tmp/private.txt" | tr '\n' ' ')"
read -r g1 g2 <<< "$(fcts "$tmp/group.txt" | tr '\n' ' ')"
if [ -z "$g2" ]; then
    check "both grouped flows finish" 1
else
    echo "      without a group: $p1 $p2 us; in one group: $g1 $g2 us"
    awk -v p1="$p1" -v p2="$p2" -v g1="$g1" -v g2="$g2" 'BEGIN {
        p = (p1 > p2 ? p1 : p2); g = (g1 < g2 ? g1 : g2)
        exit !(g > 1.8 * p) }'
    check "a shared window takes about twice as long" $?
    awk -v g1="$g1" -v g2="$g2" 'BEGIN {
        d = g1 - g2; if (d < 0) d = -d
        exit !(d < 0.02 * g1) }'
    check "grouped flows take turns and finish together" $?
fi

run pair_group.cm per_group.txt -pcm_ccc "g=$algos/dctcp.yaml" -pcm_param g:P_G=0.125
grep -q "^PCM: group g: algorithm dctcp " "$tmp/per_group.txt" &&
    grep -q "^PCM: group g: parameter P_G=0.125" "$tmp/per_group.txt" &&
    [ "$(grep -c 'pcm_group g:0->15' "$tmp/per_group.txt")" -eq 2 ]
check "-pcm_ccc and GROUP:NAME=VALUE configure a group" $?

if [ $failures -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "All flow group checks passed"
