#!/bin/sh

# Called from simple-search.sh.

rm -f "$OUT"
ntvquerygen $@ | ntvsearch $SCHARGS > $OUT
if [ "$NDOCS" != "" ]; then
    ndocs=`grep '^<hit' < $OUT | wc -l`
    if [ $ndocs != $NDOCS ]; then
	echo "Expected $NDOCS docs from $@; got $ndocs."
	exit 1
    fi
fi

exit 0
