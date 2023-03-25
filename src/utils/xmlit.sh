#!/bin/sh

# Convert a sequence of files to XML suitable for indexing.

if [ ! -e xmlit.pl ]
then
    echo File xmlit.pl is not in the current directory.
    exit 1
fi

if [ "$1" != "-nh" ]
then
    echo '<docseq>'
fi

# Filenames are presented on stdin, one per line.
while read input
do
    xmlit.pl $input
done

if [ "$1" != "-nh" ]
then
    echo '</docseq>'
fi
