#!/bin/sh

if [ "$1" = "" ]
then
    echo usage: $0 inputfile.pod '"title"'
    exit 1
fi

input="$1"
basename=`basename "$input" .pod`
shift

# Complete list of understood things.
[ "$basename" = "ntvcachecfgxml" ] && sect=5
[ "$basename" = "ntvcached" ] && sect=1
[ "$basename" = "ntvcheck" ] && sect=1
[ "$basename" = "ntvdtds" ] && sect=5
[ "$basename" = "ntvhitlistxml" ] && sect=5
[ "$basename" = "ntvindex" ] && sect=1
[ "$basename" = "ntvindexerxml" ] && sect=5
[ "$basename" = "ntvopt" ] && sect=1
[ "$basename" = "ntvquerygen" ] && sect=1
[ "$basename" = "ntvqueryxml" ] && sect=5
[ "$basename" = "ntvresourcefile" ] && sect=5
[ "$basename" = "ntvsearch" ] && sect=1
[ "$basename" = "ntvsearchd" ] && sect=1
[ "$basename" = "ntvultralite" ] && sect=1
[ "$basename" = "ntvtxtfilter" ] && sect=1
[ "$basename" = "ntvpdffilter" ] && sect=1
[ "$basename" = "ntvdocfilter" ] && sect=1
[ "$basename" = "ntvhtmlfilter" ] && sect=1
[ "$basename" = "ntvmailfilter" ] && sect=1

# Their sections.
sed="$sed -e 's%###ntvcachecfgxml###%in ntvcachecfgxml(5)%'"
sed="$sed -e 's%###ntvcached###%in ntvcached(1)%'"
sed="$sed -e 's%###ntvcheck###%in ntvcheck(1)%'"
sed="$sed -e 's%###ntvdtds###%in ntvdtds(5)%'"
sed="$sed -e 's%###ntvhitlistxml###%in ntvhitlistxml(5)%'"
sed="$sed -e 's%###ntvindex###%in ntvindex(1)%'"
sed="$sed -e 's%###ntvindexerxml###%in ntvindexerxml(5)%'"
sed="$sed -e 's%###ntvopt###%in ntvopt(1)%'"
sed="$sed -e 's%###ntvquerygen###%in ntvquerygen(1)%'"
sed="$sed -e 's%###ntvqueryxml###%in ntvqueryxml(5)%'"
sed="$sed -e 's%###ntvresourcefile###%in ntvresourcefile(5)%'"
sed="$sed -e 's%###ntvsearch###%in ntvsearch(1)%'"
sed="$sed -e 's%###ntvsearchd###%in ntvsearchd(1)%'"
sed="$sed -e 's%###ntvultralite###%in ntvultralite(1)%'"
sed="$sed -e 's%###ntvtxtfilter###%in ntvtxtfilter(1)%'"
sed="$sed -e 's%###ntvpdffilter###%in ntvpdffilter(1)%'"
sed="$sed -e 's%###ntvdocfilter###%in ntvdocfilter(1)%'"
sed="$sed -e 's%###ntvhtmlfilter###%in ntvhtmlfilter(1)%'"
sed="$sed -e 's%###ntvmailfilter###%in ntvmailfilter(1)%'"

[ -z "$sect" ] \
    && echo "$basename doesn't have a section. Update script." \
    && exit 1

echo pod2man $basename.pod...
sh -c "sed $sed" < "$input" > "$basename"
pod2man --section="$sect" \
	--center="NexTrieve" \
	--release="2.0.0" "$basename" > "$basename.$sect" 2> /tmp/eout$$.txt
if [ -s "/tmp/eout$$.txt" ]; then
    fgrep -v head3 < /tmp/eout$$.txt > /tmp/eout2$$.txt
    if [ -s "/tmp/eout2$$.txt" ]; then
	echo Unrecoverable errors:
	cat /tmp/eout$$.txt
	exit 1
    fi
    echo Changing head3 to head2...
    sed -e 's/=head3/=head2/' < "$basename" > x
    mv x "$basename"
    pod2man --section="$sect" \
	    --center="NexTrieve" \
	    --release="2.0.0" "$basename" > "$basename.$sect"
fi
rm "$basename"
