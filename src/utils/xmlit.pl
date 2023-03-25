#!/usr/bin/perl

$cnt = 0;
$titles = 1;

# The complete XML should have a header of <?xml version="1.0" encoding="ISO-8859-1"?>.

while (<>)
{
    ($line) = /^(.*)$/;
    if ( !/\.gz$/ )
    {
	open(IN, $line) || die;
    }
    else
    {
	open(IN, "gzip -d <" . $line . "|") || die;
    }

    $backup = $/;
    undef $/;
    $data = <IN>;
    close IN;

    $data =~ s/\&nbsp;?/ /gis; # Convert to real spaces
    $data =~ s/\&amp;?/\&/gs; # Convert ampersands
    $data =~ s/[^ -~\240-\376]/ /gs; # Remove non-iso8859-1
    # $data =~ s/[^ -~]/ /gs; # Remove non us-ascii
    if ($titles)
    {
	# Change title elements to a single line containing grok endgrok.
	$data =~ s/<title>(.{1,256}?)<\/title>/\ngrok $1 endgrok\n/gs;
    }

    # Remove tags -- some are deleted, the rest are replaced by a space.
    $data =~ s/<b\b[^>]*>|<\/b\s*>|<i\b[^>]*>|<\/i\s*>|<font\b[^>]*>|<\/font\s*>|<strong\b[^>]*>|<\/strong\s*>|<em\b[^>]*>|<\/em\s*>|<tt\b[^>]*>|<\/tt\s*>//gsi; # Delete character attribute related tags.
    $data =~ s/<[^>]*?>/ /gs; # Replace other tags by a space.
    $data =~ s/]]>/]]]]><![CDATA[/gs; # Transform ]]> sequences

     if ($titles)
     {
	$data =~ s/\ngrok\s(.*)\sendgrok\n/]]><title><![CDATA[$1]]><\/title><![CDATA[/gs;
    }
    $data =~ s/\s\s*/ /gs; # remove runs of white space
    $data =~ s/([^\n]{50}[^\s]*?\s)/$1\n/gs; # Nice line lengths.

    ($dev,$ino,$mode,$nlink,$uid,$gid,$rdev,$size,
      $atime,$mtime,$ctime,$blksize,$blocks) = stat($line);

    $ino = sprintf("%06d", $cnt);
    $cnt = $cnt+1;

    print "<document>\n",
          "<attributes>\n",
	  "<docid>", $ino, "</docid>\n",
	  "<filename>", $line, "</filename>\n",
	  "<mtime>", $mtime, "</mtime>\n",
	  "</attributes>\n",
	  "<text><![CDATA[", $data, "]]></text>\n",
	  "</document>\n";
    $/ = $backup;
}

exit 0;

