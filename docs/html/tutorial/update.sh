#!/bin/sh

echo ntvpod2html tutgettingstarted.pod...

tit="Getting Started Quickly"
../ntvpod2html ../../manpages/tutgettingstarted.pod \
	--title="$tit" \
    | sed -e 's%HREF="#%HREF="tutgettingstarted.html#%' \
	  -e '/quot;<A HREF/s/<A HREF=.*">//' \
	  -e '/quot<\/A>/s/<\/A>//' \
          -e 's%##IMAGE-tutgs-html-simple.gif##%<A HREF="tutgs-html-simple.gif" target="_blank">simple</A>%' \
          -e 's%##IMAGE-tutgs-html-advanced.gif##%<A HREF="tutgs-html-advanced.gif" target="_blank">advanced</A>%' \
          -e 's%##IMAGE-tutgs-mst-simple.gif##%<A HREF="tutgs-mst-simple.gif" target="_blank">simple</A>%' \
          -e 's%##IMAGE-tutgs-mst-advanced.gif##%<A HREF="tutgs-mst-advanced.gif" target="_blank">advanced</A>%' \
          -e 's%##IMAGE-tutgs-mpt-simple.gif##%<A HREF="tutgs-mpt-simple.gif" target="_blank">simple</A>%' \
          -e 's%##IMAGE-tutgs-mpt-advanced.gif##%<A HREF="tutgs-mpt-advanced.gif" target="_blank">advanced</A>%' \
          -e 's%##IMAGE-tutgs-mhonarc-simple.gif##%<A HREF="tutgs-mhonarc-simple.gif" target="_blank">simple</A>%' \
          -e 's%##IMAGE-tutgs-mhonarc-advanced.gif##%<A HREF="tutgs-mhonarc-advanced.gif" target="_blank">advanced</A>%' \
	  -e "s/<BODY>/<BODY BACKGROUND=\"bgblend.gif\"><H1 ALIGN=\"CENTER\">$tit<\/H1>/" \
    > tutgettingstarted.html


tit="Building a Web Interface From First Principles"
../ntvpod2html ../../manpages/tutweb.pod  \
	--title="$tit" \
    | sed -e 's%##IMAGE-tutweb-s_.gif##%<A HREF="tutweb-s_.gif" target="_blank">here</A>%' \
          -e 's%##IMAGE-tutweb-sq.gif##%<A HREF="tutweb-sq.gif" target="_blank">here</A>%' \
          -e 's%##IMAGE-tutweb-sa.gif##%<A HREF="tutweb-sa.gif" target="_blank">here</A>%' \
	  -e 's%HREF="#%HREF="tutweb.html#%' \
	  -e "s/<BODY>/<BODY BACKGROUND=\"bgblend.gif\"><H1 ALIGN=\"CENTER\">$tit<\/H1>/" \
    > tutweb.html

for i in ../../manpages/tut*.gif ../../manpages/bg*.gif; do
    echo cp "$i" .
    cp "$i" .
done
