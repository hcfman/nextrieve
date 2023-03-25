#!/bin/sh

#
# Basic test of system stability...
#
# Nextrieve executables should be in the current PATH.
#

tmp=/tmp/out$$

filesneeded="testhtml.res simple-search.sh testhtml1.html testhtml2.html"
bad=
for i in $filesneeded; do
    [ ! -s "$i" ] && bad="$bad $i"
done

if [ ! -z "$bad" ]; then
    echo "Cannot find required files in current directory ($bad)."
    exit 1
fi


echo "Testing basic XML indexing..."
rm -rf testidx
mkdir testidx

# TWO documents...
ntvindex -R testhtml.res -v - << EOXML
<ntv:docseq xmlns:ntv="http://www.nextrieve.com/1.0">
<document>
    <attributes>
	<number>0</number>
	<title>simple title: doc 1</title>
	<filename>testxml1.html</filename>
    </attributes>
    <text>
	<title>simple title: doc 1</title>
	some simple text. document number 1.
    </text>
</document>
<document>
    <attributes>
	<flag></flag>
	<number>1</number>
	<title>simple title: doc 2</title>
	<filename>testxml2.html</filename>
    </attributes>
    <text>
	<title>simple title: doc 2</title>
	some simple text in document number 2.
    </text>
</document>
</ntv:docseq>
EOXML

if [ "$?" != 0 ]; then
    echo Basic indexing failed!
    exit 1
fi

NTV_RESOURCE=testhtml.res export NTV_RESOURCE
OUT=$tmp export OUT

# Raw searches...
NDOCS=2 ./simple-search.sh -x simple || exit 1
NDOCS=2 ./simple-search.sh simple || exit 1
NDOCS=1 ./simple-search.sh -c flag simple || exit 1
NDOCS=1 ./simple-search.sh -c !flag simple || exit 1
NDOCS=1 ./simple-search.sh -c number=0 simple || exit 1
NDOCS=1 ./simple-search.sh -c number!=0 simple || exit 1

ntvopt testidx
if [ "$?" != 0 ]; then
    echo Optimizing failed...
    exit 1
fi
ntvidx-useopt.sh testidx

# Raw searches after optimizing...
NDOCS=2 ./simple-search.sh -x simple || exit 1
NDOCS=2 ./simple-search.sh simple || exit 1
NDOCS=1 ./simple-search.sh -c flag simple || exit 1
NDOCS=1 ./simple-search.sh -c !flag simple || exit 1
NDOCS=1 ./simple-search.sh -c number=0 simple || exit 1
NDOCS=1 ./simple-search.sh -c number!=0 simple || exit 1

# Raw searches through daemon...
SCHARGS="-A localhost -P 9000" export SCHARGS
ntvsearchd $SCHARGS; sleep 5
if kill -0 `cat testidx/pid*.ntv`; then
    : ;
else
    echo Search daemon apparently did not start.
    echo Look at the end of testidx/log.txt.
    exit 1
fi

NDOCS=2 ./simple-search.sh -x simple || exit 1
NDOCS=2 ./simple-search.sh simple || exit 1
NDOCS=1 ./simple-search.sh -c flag simple || exit 1
NDOCS=1 ./simple-search.sh -c !flag simple || exit 1
NDOCS=1 ./simple-search.sh -c number=0 simple || exit 1
NDOCS=1 ./simple-search.sh -c number!=0 simple || exit 1

kill `cat testidx/pid*ntv`; rm -f testidx/pid*.ntv; sleep 2
unset SCHARGS

# Add a couple of html docs...
(echo testhtml1.html; echo testhtml2.html) \
    | ntvhtmlfilter -t title -T title -f filename \
    | ntvdocseq \
    | ntvindex -v -
if [ "$?" != 0 ]; then
    echo Incremental indexing failed!
    exit 1
fi

ntvopt testidx
if [ $? != 0 ]; then
    echo Optimizing failed...
    exit 1
fi
ntvidx-useopt.sh testidx

NDOCS=3 ./simple-search.sh -x simple 
NDOCS=4 ./simple-search.sh simple
NDOCS=1 ./simple-search.sh -c flag simple
NDOCS=3 ./simple-search.sh -c !flag simple
NDOCS=3 ./simple-search.sh -c number=0 simple
NDOCS=1 ./simple-search.sh -c number!=0 simple

rm -rf testidx

echo Simple tests worked out OK.
exit 0
