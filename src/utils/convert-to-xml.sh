#!/bin/sh

for i in /data1/trec/disk*/WTX???
do
    echo $i...
    for j in $i/*.gz
    do
	gzip -d < $j
    done | awk -f convert-to-xml.awk | gzip -c > $i.xml.gz
done
