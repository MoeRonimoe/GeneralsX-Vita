#!/bin/bash
# Compares two versions of the Direct3D 8 layer that run.sh has built: both get the same
# Direct3D calls, for several seeds, without and with the render thread, and the four traces
# of each seed have to be the same.
#
#   compare.sh <name of the first> <name of the second> [frames] [seeds...]
OUT=${D3D8TEST_OUT:-$(cd "$(dirname "$0")" && pwd)/../../../../build/hosttest/d3d8}
FIRST=$1
SECOND=$2
FRAMES=${3:-120}
shift 3 2> /dev/null
SEEDS=${*:-1 2 3 4 5 6 7 8}

if [ -z "$FIRST" ] || [ -z "$SECOND" ]; then
	echo "compare.sh <name of the first> <name of the second> [frames] [seeds...]"
	exit 2
fi

FAILED=0
for SEED in $SEEDS; do
	for NAME in "$FIRST" "$SECOND"; do
		for THREAD in 0 1; do
			if ! VITA_RENDER_THREAD=$THREAD "$OUT/test-$NAME" "$OUT/compare-$NAME-$THREAD.txt" "$FRAMES" "$SEED" 2> "$OUT/compare-$NAME-$THREAD.log"; then
				echo "seed $SEED: $NAME with VITA_RENDER_THREAD=$THREAD reports errors"
				tail -n 12 "$OUT/compare-$NAME-$THREAD.log"
				FAILED=1
			fi
		done
	done

	SAME=1
	for OTHER in "$FIRST-1" "$SECOND-0" "$SECOND-1"; do
		if ! cmp -s "$OUT/compare-$FIRST-0.txt" "$OUT/compare-$OTHER.txt"; then
			echo "seed $SEED: $FIRST-0 and $OTHER differ"
			diff "$OUT/compare-$FIRST-0.txt" "$OUT/compare-$OTHER.txt" | head -n 12
			SAME=0
			FAILED=1
		fi
	done

	if [ $SAME = 1 ]; then
		echo "seed $SEED: the same, $(grep -c ' draw ' "$OUT/compare-$FIRST-0.txt") draw calls"
	else
		# Kept for a look at what differs.
		for NAME in "$FIRST" "$SECOND"; do
			for THREAD in 0 1; do
				cp "$OUT/compare-$NAME-$THREAD.txt" "$OUT/differs-$SEED-$NAME-$THREAD.txt"
			done
		done
	fi
done

exit $FAILED
