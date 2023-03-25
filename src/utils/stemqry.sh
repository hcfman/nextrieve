#!/bin/sh

# Given a file containing query words, one per line, produce a new
# query containing stemmed variants...

stemmer=/home/kim/someuser/nextrieve-accents/utils/stemmer
dictstems=/data1/indexes/someuser/dict-allstems.txt

if [ ! -e "$stemmer" ]
then
    echo Where is the stemmer -- "$stemmer" does not exist.
    exit 1
fi

if [ ! -e "$dictstems" ]
then
    echo Where are the dictionary stems -- "$dictstems" does not exist.
    exit 1
fi

sort -u < $1 \
    | $stemmer \
    | sort -u -k 1,1 \
    | while read stem orig
      do
	grep "^$stem " < $dictstems | sed -e 's/^[^ ]* / /' > stems$$
	if [ -s stems$$ ]
	then
	    # Put out orig wd, followed by stem-equivalents prefixed with '*'.
	    grep " $orig " < stems$$ > x$$
	    if [ -s x$$ ]
	    then
		sed -e "s/ $orig / /" -e 's/ \(.\)/ *\1/g' -e "s/^/ $orig /" \
		    < stems$$ > x$$
	    else
		sed -e 's/^ /x/' -e 's/ \(.\)/ *\1/g' -e "s/^x/ /" \
		    < stems$$ > x$$
	    fi

	    cat x$$
	else
	    # Stem doesn't exist, word doesn't exist either.
	    :; 
	fi
      done

rm -f x$$ stems$$
