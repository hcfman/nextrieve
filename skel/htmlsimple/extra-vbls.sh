
# Included by ntvmakenewindex to define extra variables.

NTVBASEDIR=
while [ -z "$NTVBASEDIR" ] ; do
    cat <<EOF
What is the BASEDIR? i.e. this is the directory from which all documents
to index can be found.  This could possibly be set to the document root
of your webserver for example, for an HTML index.
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
echo 'You can index any of:'
echo '  - HTML files (*.htm, *.html)'
echo '  - PDF files (*.pdf)'
echo '  - DOC files (*.doc)'
echo '  - TXT files (*.txt)'
echo ''
echo 'To index PDF files, you must have pdftotext installed.'
echo 'To index DOC files, you must have antiword installed.'
echo '(Both these applications are freely available.)'

h=" html"
p=""
d=""
t=""

quit=

while [ -z "$quit" ]; do
    echo ''
    if [ ! -z "$h$p$d" ]; then
	echo "You are currently indexing$h$p$d$t files."
    else
	echo "You are currently indexing no files."
    fi
    echo ""
    echo -n "Enter h, p, d or t; q to terminate; ! to clear: "
    read answer
    answer=`echo $answer|sed -e 's/^[   ]*//' -e 's/[   ]*$//'`
    case "$answer" in
    h|H) h=" html"
	 ;;
    p|P) p=" pdf"
	 ;;
    d|D) d=" doc"
	 ;;
    q|Q) quit=yes
	 ;;
    t|T) t=" txt"
	 ;;
    !)   h=; p=; d=;
	 ;;
    esac
done

if [ -z "$h$p$c$t" ]; then
    echo ... indexing html files by default.
    h=" html"
fi

[ ! -z "$h" ] && NTVHTMLFILES=1
[ ! -z "$p" ] && NTVPDFFILES=1
[ ! -z "$d" ] && NTVDOCFILES=1
[ ! -z "$t" ] && NTVTXTFILES=1

echo ''
echo If these files are immediately accessable from a site, enter the
echo HTTP prefix that should be used.
echo Eg, a prefix of http://www.abc.com/exampledocs, with an
echo indexed file of $NTVBASEDIR/file.html, will cause a link to be created
echo of http://www.abc.com/exampledocs/file.html.
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

NTVAUTOINDEX=1
