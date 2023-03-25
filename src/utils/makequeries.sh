#!/bin/sh

sw=/tmp/test/sw.txt

if [ ! -s "$sw" ]
then
    sw=""
fi

if [ "$sw" = "" ]
then
    tr -cs 'a-zA-Z0-9' '\012' < $1| awk -f makequeries.awk
else
    tr -cs 'a-zA-Z0-9' '\012' < $1 | fgrep -v -i -w -f $sw | awk -f makequeries.awk
fi
