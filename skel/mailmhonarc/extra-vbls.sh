
# Included by ntvmakenewindex to define extra variables.

NTVBASEDIR=
while [ -z "$NTVBASEDIR" ] ; do
    cat <<EOF
What is the BASEDIR? i.e. this is the directory under the document root
from which all documents that relate the mhonarc collection exist. i.e.
there should be directories named by date with mhonarc files in them
of the form yyyymm.
EOF
    read NTVBASEDIR
    NTVBASEDIR=`echo $NTVBASEDIR|sed -e 's/^[   ]*//' -e 's/[   ]*$//'`

    if [ "$NTVBASEDIR" = "`echo ${NTVBASEDIR#/}`" ] ; then
	echo ""
	echo "You must choose an absolute path to the basedir"
	NTVBASEDIR=
	continue
    fi

    if [ ! -d "$NTVBASEDIR" ] ; then
	echo ""
	echo "$NTVBASEDIR does not exist; are you sure you want to use this value?"
	read answer
	answer=`echo $answer|sed -e 's/^[   ]*//' -e 's/[   ]*$//'`

	if [ "$answer" != y -a "$answer" != Y ] ; then
	    NTVBASEDIR=
	fi
    fi
done

echo ''
echo If these files are immediately accessable from a site, enter the
echo HTTP prefix that should be used.
echo This should contain a prefix that would take you to the directory where
echo that corresponds to the directory entered as a BASEDIR earlier
echo ''
echo If the indexed documents are not immediately accessable, use a blank
echo line.  The script ${NTVNAME}show will be called but will not actually
echo display the document unless you edit it.
echo ''
echo Enter http prefix for accessable documents, or a blank line:

read NTVHTTPPREFIX

if [ -z "$NTVHTTPPREFIX" ]; then
    # Use a prefix that invokes ${NTVNAME}show.
    NTVHTTPPREFIX="/cgi-bin/${NTVNAME}show?filename="
elif [ "`echo ${NTVHTTPPREFIX%/}`" = "$NTVHTTPPREFIX" ]; then
    # Force to end with a '/'.
    NTVHTTPPREFIX="$NTVHTTPPREFIX"/
fi

NTVMHONARC=
while [ -z "$NTVMHONARC" ] ; do
    echo ""
    echo "What is the full pathname to the mhonarc program ?"
    read NTVMHONARC

    if [ ! -x "$NTVMHONARC" ]; then
	echo "$NTVMHONARC" is not executable.
	NTVMHONARC=
    fi
done

NTVAUTOINDEX=1
