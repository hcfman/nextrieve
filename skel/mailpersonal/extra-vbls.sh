
# Included by ntvmakenewindex to define extra variables.

NTVMAILADM=
while [ -z "$NTVMAILADM" ]; do
    echo ""
    echo 'Who should receive email when a daily index fails?'
    read NTVMAILADM
    NTVMAILADM="`echo $NTVMAILADM|sed -e 's/^[   ]*//' -e 's/[   ]*$//'`"
done

sendmaildflt=""
sendmaildfltmsg=""
if [ -f "/usr/sbin/sendmail" ]; then
    sendmaildflt="/usr/sbin/sendmail"
    sendmaildfltmsg="(/usr/sbin/sendmail)"
fi

NTVSENDMAIL=
while [ -z "$NTVSENDMAIL" ]; do
    echo ""
    echo "What sendmail should be used to send these emails? $sendmaildfltmsg"
    read NTVSENDMAIL
    NTVSENDMAIL="`echo $NTVSENDMAIL|sed -e 's/^[   ]*//' -e 's/[   ]*$//'`"
    if [ -z "$NTVSENDMAIL" ]; then NTVSENDMAIL="$sendmaildflt"; fi
    if [ ! -f "$NTVSENDMAIL" ]; then
	echo $NTVSENDMAIL does not exist.
	NTVSENDMAIL=
    fi
done

NTVHOSTNAME="`hostname`"

echo ''
echo Enter the http prefix to the cgi-bin directory that corresponds to the
echo cgi directory you entered for the search script. This may be something
echo like http://www.somesite.com/cgi-bin for example.
echo ''
echo Enter http prefix for accessable documents, or a blank line:

read NTVHTTPPREFIX

if [ -z "$NTVHTTPPREFIX" ]; then
    # Use a prefix that invokes ${NTVNAME}show.
    NTVHTTPPREFIX="/cgi-bin/"
elif [ "`echo ${NTVHTTPPREFIX%/}`" = "$NTVHTTPPREFIX" ]; then
    # Force to end with a '/'.
    NTVHTTPPREFIX="$NTVHTTPPREFIX"/
fi
