#!/usr/bin/env bash
#
# Checks that PCM ports of htsim's built-in sender algorithms send exactly like
# the originals: each pair is run on the same connection matrices and the
# outputs must be identical, apart from lines and fields that only PCM prints.
#
#   constant      vs  -pcm constant.yaml
#   dctcp         vs  -pcm dctcp_legacy.yaml -pcm_units native
#
# Usage: ./pcm_exact_match.sh   (exits non-zero if any pair differs)

cd "$(dirname "${BASH_SOURCE[0]}")"

if [ ! -x ./htsim_uec ]; then
    echo "htsim_uec not found; build it first (see AGENTS.md)"
    exit 1
fi

algos=../pcm/algorithms
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# "<built-in algorithm>|<PCM options>"
pairs=(
    "constant|-pcm $algos/constant.yaml"
    "dctcp|-pcm $algos/dctcp_legacy.yaml -pcm_units native"
)

# Connection matrix and any extra options.
matrices=(
    "connection_matrices/one.cm"
    "connection_matrices/incast.cm"
    "connection_matrices/perm_32n_32c_2MB.cm -topo topologies/leaf_spine_tiny.topo"
)

# Remove what only one side prints: the algorithm choice and PCM lines, PCM
# fields on finish lines, and the built-in DCTCP's per-ACK debug line.
normalize() {
    grep -v -e '^PCM:' -e '^sender based algo' -e ' DCTCP start ' "$1" |
        sed 's/ pcm_events .*$//'
}

failures=0
for pair in "${pairs[@]}"; do
    builtin="${pair%%|*}"
    pcm="${pair#*|}"
    for m in "${matrices[@]}"; do
        ./htsim_uec -tm $m -sender_cc_only -end 3000 -sender_cc_algo "$builtin" \
            > "$tmp/builtin.txt" 2>&1
        ./htsim_uec -tm $m -sender_cc_only -end 3000 $pcm > "$tmp/pcm.txt" 2>&1
        if diff <(normalize "$tmp/builtin.txt") <(normalize "$tmp/pcm.txt") > "$tmp/diff.txt"; then
            flows=$(grep -c "finished at" "$tmp/pcm.txt")
            echo "PASS  $builtin  ${m%% *}  ($flows flows finished)"
        else
            echo "FAIL  $builtin  ${m%% *}"
            head -20 "$tmp/diff.txt"
            failures=$((failures + 1))
        fi
    done
done

if [ $failures -ne 0 ]; then
    echo "$failures comparison(s) differ"
    exit 1
fi
echo "All comparisons identical"
