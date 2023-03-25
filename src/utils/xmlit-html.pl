#!/usr/bin/perl

$cnt = 0;
$titles = 1;

# The complete XML should have a header of <?xml version="1.0" encoding="ISO-8859-1"?>.

print "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n",
      "<!DOCTYPE bodgything\n",
      "    [\n";

open(IN, "htmlcodes.txt") || die "Cannot open htmlcodes.txt";
    $backup = $/;
    undef $/;
    $data = <IN>;
    close IN;
    $/ = $backup;

print $data;

print "    ]>\n",
      "<docseq>\n";

while (<>)
{
    ($line) = /^(.*)$/;
    if ( !/\.gz$/ )
    {
	open(IN, $line) || die "Cannot open \"" . $line . "\"";
    }
    else
    {
	open(IN, "gzip -d < '" . $line . "'|") || die "Cannot run " . "gzip -d <" . $line;
    }

    $backup = $/;
    undef $/;
    $data = <IN>;
    close IN;

    $data =~ s/[^ -~\240-\376]/ /gs; # Remove non-iso8859-1
    $data =~ s/&&/&amp;&/gs; # Double-ampersands. 
    $data =~ s/&([0-9a-zA-Z]+[^;0-9a-zA-Z])/&amp;$1/gs; # &rubbish and stuff.
    $data =~ s/&([^#0-9a-zA-Z])/&amp;$1/gs; # Lonely ampersands. 
    $data =~ s/&#([0-9]*)([^0-9;])/&#$1;$2/gs; # Badly terminated decimal numbers.
    $data =~ s/&#x([0-9a-fA-F]*)([^0-9a-fA-F;])/&#x$1;$2/gs; # Badly terminated hex numbers.
    # $data =~ s/[^ -~]/ /gs; # Remove non ascii

    # Remove <SCRIPT...> ... </SCRIPT> stuff.
    $data =~ s/<SCRIPT.*?<\/SCRIPT>/ /gsi;

    if ($titles)
    {
	# Change title elements to a single line containing grok endgrok.
	$data =~ s/<title>(.{0,256}?)<\/title>/\ngrok $1 endgrok\n/gs;
    }

    # Remove tags -- some are deleted, the rest are replaced by a space.
    $data =~ s/<b\b[^>]*>|<\/b\s*>|<i\b[^>]*>|<\/i\s*>|<font\b[^>]*>|<\/font\s*>|<strong\b[^>]*>|<\/strong\s*>|<em\b[^>]*>|<\/em\s*>|<tt\b[^>]*>|<\/tt\s*>//gsi; # Delete character attribute related tags.
    $data =~ s/<[^>]*?>/ /gs; # Replace other tags by a space.

    if ($titles)
    {
	$data =~ s/\ngrok\s*endgrok\n//gs;
	$data =~ s/\ngrok\s(.*?)\sendgrok\n/<title>$1<\/title>/gs;
    }
    $data =~ s/\s\s*/ /gs; # remove runs of white space
    $data =~ s/([^\n]{50}[^\s]*?\s)/$1\n/gs; # Nice line lengths.

    ($dev,$ino,$mode,$nlink,$uid,$gid,$rdev,$size,
      $atime,$mtime,$ctime,$blksize,$blocks) = stat($line);

    $ino = sprintf("%06d", $cnt);
    $cnt = $cnt+1;

    $line =~ s/&/&amp;/g;
    $line =~ s/</&lt;/g;
    $line =~ s/>/&gt;/g;

    print "<document>\n",
          "<attributes>\n",
	  "<docid>", $ino, "</docid>\n",
	  "<filename>", $line, "</filename>\n",
	  "<mtime>", $mtime, "</mtime>\n",
	  "</attributes>\n",
	  "<text>", $data, "</text>\n",
	  "</document>\n";
    $/ = $backup;
}

print "</docseq>\n";

exit 0;

