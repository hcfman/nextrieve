sub ntverror {
    my ( $format, @list ) = @_;

    my $message = sprintf( $format, @list );
    my $message = "$!" ? "$message: $!\n" : "$message\n";
    print STDERR $message;

    # Set global "run failed" flag
    open( MAILOUT, "> %%NTVBASE%%/%%NTVNAME%%/runblocked" );
    close MAILOUT;

    my $sec, $min, $hour, $mday, $mon, $year, $wday, $yday, $isdst;
    ( $sec, $min, $hour, $mday, $mon, $year, $wday, $yday, $isdst ) =
        localtime( time );
    $year += 1900; $mon++;

    # Log all the error messages
    open( LOGOUT, ">> %%NTVBASE%%/%%NTVNAME%%/logs/scripterror.log" ) ||
	die "Can't create log: $!\n";
    printf( LOGOUT "%04d-%02d-%02d %02d:%02d:%02d - $message\n",
	$year, $mon, $mday, $hour, $min, $sec );
    close LOGOUT;

    if ( !open( MAILOUT, "|-" ) ) {
        exec '%%NTVSENDMAIL%%', '-t';
        exit 1;
    }

    $TO = '%%NTVMAILADM%%';
    print MAILOUT <<perlEOF;
From: $TO
To: $TO
Subject: %%NTVNAME%% - Failed indexing

$message
perlEOF
close MAILOUT;

    exit 1;
}

1;
