#!/bin/sh

# Analyze a log file of queries, creating graphs of queries/second
# and number of seconds.

[ "$1" = "" ] && echo 'usage: logfilename' && exit 1

logfile="$1"
graphfile="$1".txt
giffile="$1".gif

shift

awk '/</ {next;}
	 {if ($0 != lastline)
		{
		    if (lastline != "")
		    {
			if (gap == 1)
			    print nlines;
			else
			    print (nlines+0.0) / gap;
		    }
		    time = $4;
		    split(time, fields, ":");
		    seconds = fields[1]*60*60+fields[2]*60+fields[3];
		    if (lastline != "")
		    {
			gap = seconds - lastseconds;
			if (gap < 0)
			    gap += 24*60*60;
		    }
		    else
			gap = 1;
		    lastseconds = seconds;
		    nlines=0;lastline=$0;
		}
	     nlines++;
	 }
     END {print nlines;}' < "$logfile" \
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
