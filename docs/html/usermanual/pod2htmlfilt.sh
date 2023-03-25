#!/bin/sh

if [ "$1" = "" ]
then
    echo usage: $0 inputfile.pod '"title"'
    exit 1
fi

input="$1"
basename=`basename "$input" .pod`
shift
title="$@"

echo ../ntvpod2html $basename.pod...
    sed -e '1,5d' \
        -e '/=head1 *DIAGNOSTICS/,$d' \
        -e '/=head1 *SEE ALSO/,$d' \
        -e 's/=head1/=head2/' \
	< "$input" \
    | ../ntvpod2html --title="$title" \
    | sed -e 's%HREF="#%HREF="'$basename'.html#%' \
          -e 's%###\(ntv.*\)###%<A HREF="\1.html" target="_blank">here</A>%' \
	  -e 's%^ *TABLESTART$%<TABLE CELLSPACING=0 CELLPADDING=0 BORDER=0> <TR> <TD BGCOLOR="#A6C2ED"><FONT SIZE=2><PRE>%' \
	  -e 's%TABLEEND%</PRE></FONT></TD></TR></TABLE>%' \
	  -e "s/<BODY>/<BODY BACKGROUND=\"bgblend.gif\"><H1 ALIGN=\"CENTER\">$title<\/H1>/" \
    > $basename.html
