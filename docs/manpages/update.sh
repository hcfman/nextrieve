#!/bin/sh

for i in ntv*.pod
do
    ./pod2manfilt.sh $i
done
