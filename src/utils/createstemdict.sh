#!/bin/sh

# Given an ntcheck-produced dictionary dump, produce all stemmed
# variants...

stemmer=/home/kim/someuser/nextrieve-accents/utils/stemmer

if [ ! -e "$stemmer" ]
then
    echo Where is the stemmer -- "$stemmer" does not exist.
    exit 1
fi


sed -e '/^[0-9]*:/!d' -e '/\\/d' -e 's/^[0-9]*: //' -e 's/ .*//' < $1 \
    | sort -u \
    | $stemmer \
    | sort \
    | awk -f createstemdict.awk  > allstems.txt
