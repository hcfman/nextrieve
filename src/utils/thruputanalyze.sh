#!/bin/sh

# Analyze a thruput file, creating graphs of queries/second
# and number of seconds.

[ "$1" = "" ] && echo 'usage: thruputfilename' && exit 1

thruputfile="$1"
graphfile="$1".txt
giffile="$1".gif

shift

awk '/^[^+]/ {next;}
	 {
	    gap = substr($1, 2) + 0;
	    print ($2+0.0) / gap;
	 }' < "$thruputfile" \
    | sed -e 's/$/ /' \
    | sort -n \
    | awk '{if ($0 != lastline)
		    {
			if (lastline != "") print lastline, nlines;
			nlines=0;lastline=$0;
		    }
		 nlines++;
		}
	    END {print lastline, nlines;}' \
    > "$graphfile"

graph -T X --top-label="$*" --x-label="queries/second" --y-label="# seconds" \
    < "$graphfile"
graph -T gif --top-label="$*" --x-label="queries/second" --y-label="# seconds" \
    < "$graphfile" \
    > "$giffile"
