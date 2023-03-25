function subtags(str) {
			gsub("<b\\>[^>]*>|</b\s*>|<i\\>[^>]*>|</i\s*>|<font\\>[^>]*>|</font\s*>|<strong\\>[^>]*>|</strong\s*>|<em\\>[^>]*>|</em\s*>|<tt\\>[^>]*>|</tt\s*>","", str);
			gsub("<[^<>]*>?", " ", str);
			gsub("<", "\\&lt;", str);
			gsub(">", "\\&gt;", str);

			return str;
                      }
function printtitle(astitle) {
			if (title != "")
			    if (astitle)
				printf "\n<title>%s</title>\n", subtags(title);
			    else
				printf "\n%s\n", subtags(title);
			title = "";
			intitle = 0;
                      }
BEGIN {
legalamps["AElig"] = 1;
legalamps["Aacute"] = 1;
legalamps["Acirc"] = 1;
legalamps["Agrave"] = 1;
legalamps["Alpha"] = 1;
legalamps["Aring"] = 1;
legalamps["Atilde"] = 1;
legalamps["Auml"] = 1;
legalamps["Beta"] = 1;
legalamps["Ccedil"] = 1;
legalamps["Chi"] = 1;
legalamps["Dagger"] = 1;
legalamps["Delta"] = 1;
legalamps["ETH"] = 1;
legalamps["Eacute"] = 1;
legalamps["Ecirc"] = 1;
legalamps["Egrave"] = 1;
legalamps["Epsilon"] = 1;
legalamps["Eta"] = 1;
legalamps["Euml"] = 1;
legalamps["Gamma"] = 1;
legalamps["Iacute"] = 1;
legalamps["Icirc"] = 1;
legalamps["Igrave"] = 1;
legalamps["Iota"] = 1;
legalamps["Iuml"] = 1;
legalamps["Kappa"] = 1;
legalamps["Lambda"] = 1;
legalamps["Mu"] = 1;
legalamps["Ntilde"] = 1;
legalamps["Nu"] = 1;
legalamps["OElig"] = 1;
legalamps["Oacute"] = 1;
legalamps["Ocirc"] = 1;
legalamps["Ograve"] = 1;
legalamps["Omega"] = 1;
legalamps["Omicron"] = 1;
legalamps["Oslash"] = 1;
legalamps["Otilde"] = 1;
legalamps["Ouml"] = 1;
legalamps["Phi"] = 1;
legalamps["Pi"] = 1;
legalamps["Prime"] = 1;
legalamps["Psi"] = 1;
legalamps["Rho"] = 1;
legalamps["Scaron"] = 1;
legalamps["Sigma"] = 1;
legalamps["THORN"] = 1;
legalamps["Tau"] = 1;
legalamps["Theta"] = 1;
legalamps["Uacute"] = 1;
legalamps["Ucirc"] = 1;
legalamps["Ugrave"] = 1;
legalamps["Upsilon"] = 1;
legalamps["Uuml"] = 1;
legalamps["Xi"] = 1;
legalamps["Yacute"] = 1;
legalamps["Yuml"] = 1;
legalamps["Zeta"] = 1;
legalamps["aacute"] = 1;
legalamps["acirc"] = 1;
legalamps["acute"] = 1;
legalamps["aelig"] = 1;
legalamps["agrave"] = 1;
legalamps["alefsym"] = 1;
legalamps["alpha"] = 1;
legalamps["amp"] = 1;
legalamps["and"] = 1;
legalamps["ang"] = 1;
legalamps["aring"] = 1;
legalamps["asymp"] = 1;
legalamps["atilde"] = 1;
legalamps["auml"] = 1;
legalamps["bdquo"] = 1;
legalamps["beta"] = 1;
legalamps["brvbar"] = 1;
legalamps["bull"] = 1;
legalamps["cap"] = 1;
legalamps["ccedil"] = 1;
legalamps["cedil"] = 1;
legalamps["cent"] = 1;
legalamps["chi"] = 1;
legalamps["circ"] = 1;
legalamps["clubs"] = 1;
legalamps["cong"] = 1;
legalamps["copy"] = 1;
legalamps["crarr"] = 1;
legalamps["cup"] = 1;
legalamps["curren"] = 1;
legalamps["dArr"] = 1;
legalamps["dagger"] = 1;
legalamps["darr"] = 1;
legalamps["deg"] = 1;
legalamps["delta"] = 1;
legalamps["diams"] = 1;
legalamps["divide"] = 1;
legalamps["eacute"] = 1;
legalamps["ecirc"] = 1;
legalamps["egrave"] = 1;
legalamps["empty"] = 1;
legalamps["emsp"] = 1;
legalamps["ensp"] = 1;
legalamps["epsilon"] = 1;
legalamps["equiv"] = 1;
legalamps["eta"] = 1;
legalamps["eth"] = 1;
legalamps["euml"] = 1;
legalamps["euro"] = 1;
legalamps["exist"] = 1;
legalamps["fnof"] = 1;
legalamps["forall"] = 1;
legalamps["frac12"] = 1;
legalamps["frac14"] = 1;
legalamps["frac34"] = 1;
legalamps["frasl"] = 1;
legalamps["gamma"] = 1;
legalamps["ge"] = 1;
legalamps["gt"] = 1;
legalamps["hArr"] = 1;
legalamps["harr"] = 1;
legalamps["hearts"] = 1;
legalamps["hellip"] = 1;
legalamps["iacute"] = 1;
legalamps["icirc"] = 1;
legalamps["iexcl"] = 1;
legalamps["igrave"] = 1;
legalamps["image"] = 1;
legalamps["infin"] = 1;
legalamps["int"] = 1;
legalamps["iota"] = 1;
legalamps["iquest"] = 1;
legalamps["isin"] = 1;
legalamps["iuml"] = 1;
legalamps["kappa"] = 1;
legalamps["lArr"] = 1;
legalamps["lambda"] = 1;
legalamps["lang"] = 1;
legalamps["laquo"] = 1;
legalamps["larr"] = 1;
legalamps["lceil"] = 1;
legalamps["ldquo"] = 1;
legalamps["le"] = 1;
legalamps["lfloor"] = 1;
legalamps["lowast"] = 1;
legalamps["loz"] = 1;
legalamps["lrm"] = 1;
legalamps["lsaquo"] = 1;
legalamps["lsquo"] = 1;
legalamps["lt"] = 1;
legalamps["macr"] = 1;
legalamps["mdash"] = 1;
legalamps["micro"] = 1;
legalamps["middot"] = 1;
legalamps["minus"] = 1;
legalamps["mu"] = 1;
legalamps["nabla"] = 1;
legalamps["nbsp"] = 1;
legalamps["ndash"] = 1;
legalamps["ne"] = 1;
legalamps["ni"] = 1;
legalamps["not"] = 1;
legalamps["notin"] = 1;
legalamps["nsub"] = 1;
legalamps["ntilde"] = 1;
legalamps["nu"] = 1;
legalamps["oacute"] = 1;
legalamps["ocirc"] = 1;
legalamps["oelig"] = 1;
legalamps["ograve"] = 1;
legalamps["oline"] = 1;
legalamps["omega"] = 1;
legalamps["omicron"] = 1;
legalamps["oplus"] = 1;
legalamps["or"] = 1;
legalamps["ordf"] = 1;
legalamps["ordm"] = 1;
legalamps["oslash"] = 1;
legalamps["otilde"] = 1;
legalamps["otimes"] = 1;
legalamps["ouml"] = 1;
legalamps["para"] = 1;
legalamps["part"] = 1;
legalamps["permil"] = 1;
legalamps["perp"] = 1;
legalamps["phi"] = 1;
legalamps["pi"] = 1;
legalamps["piv"] = 1;
legalamps["plusmn"] = 1;
legalamps["pound"] = 1;
legalamps["prime"] = 1;
legalamps["prod"] = 1;
legalamps["prop"] = 1;
legalamps["psi"] = 1;
legalamps["quot"] = 1;
legalamps["rArr"] = 1;
legalamps["radic"] = 1;
legalamps["rang"] = 1;
legalamps["raquo"] = 1;
legalamps["rarr"] = 1;
legalamps["rceil"] = 1;
legalamps["rdquo"] = 1;
legalamps["real"] = 1;
legalamps["reg"] = 1;
legalamps["rfloor"] = 1;
legalamps["rho"] = 1;
legalamps["rlm"] = 1;
legalamps["rsaquo"] = 1;
legalamps["rsquo"] = 1;
legalamps["sbquo"] = 1;
legalamps["scaron"] = 1;
legalamps["sdot"] = 1;
legalamps["sect"] = 1;
legalamps["shy"] = 1;
legalamps["sigma"] = 1;
legalamps["sigmaf"] = 1;
legalamps["sim"] = 1;
legalamps["spades"] = 1;
legalamps["sub"] = 1;
legalamps["sube"] = 1;
legalamps["sum"] = 1;
legalamps["sup"] = 1;
legalamps["sup1"] = 1;
legalamps["sup2"] = 1;
legalamps["sup3"] = 1;
legalamps["supe"] = 1;
legalamps["szlig"] = 1;
legalamps["tau"] = 1;
legalamps["there4"] = 1;
legalamps["theta"] = 1;
legalamps["thetasym"] = 1;
legalamps["thinsp"] = 1;
legalamps["thorn"] = 1;
legalamps["tilde"] = 1;
legalamps["times"] = 1;
legalamps["trade"] = 1;
legalamps["uArr"] = 1;
legalamps["uacute"] = 1;
legalamps["uarr"] = 1;
legalamps["ucirc"] = 1;
legalamps["ugrave"] = 1;
legalamps["uml"] = 1;
legalamps["upsih"] = 1;
legalamps["upsilon"] = 1;
legalamps["uuml"] = 1;
legalamps["weierp"] = 1;
legalamps["xi"] = 1;
legalamps["yacute"] = 1;
legalamps["yen"] = 1;
legalamps["yuml"] = 1;
legalamps["zeta"] = 1;
legalamps["zwj"] = 1;
legalamps["zwnj"] = 1;
hex["0"] = 0;
hex["1"] = 1;
hex["2"] = 2;
hex["3"] = 3;
hex["4"] = 4;
hex["5"] = 5;
hex["6"] = 6;
hex["7"] = 7;
hex["8"] = 8;
hex["9"] = 9;
hex["a"] = 10;
hex["b"] = 11;
hex["c"] = 12;
hex["d"] = 13;
hex["e"] = 14;
hex["f"] = 15;
hex["A"] = 10;
hex["B"] = 11;
hex["C"] = 12;
hex["D"] = 13;
hex["E"] = 14;
hex["F"] = 15;
IGNORECASE=1;
RS="[\015\012]";
}
END {if (indoc) print "</text></document>"}
/[^\011 -~\240-\377].*[^\011 -~\240-\377].*[^\011 -~\240-\377]/ {next}
/^<doc>$/ {next}
/^<docoldno>/ {next}
/^<dochdr>/,/^<\/DOCHDR>/ {next}
/^<docno>/ {
		/* Entering a document. */
		filename=substr($0, 8, length($0)-15);
		split(filename, pieces, "-");
		docid = substr(pieces[1],4) substr(pieces[2],2) sprintf("%04d", pieces[3]+0);
		inscript = 0;
		intitle = 0;
		intag = 0;
		if (indoc)
		{
		    printtitle(0);
		    print "</text></document>";
		}
		print "<document><attributes><docid>", docid, "</docid><filename>", filename, "</filename></attributes><text>";
		indoc = 1;
		title = "";
		next;
           }
/[^\011 -~\240-\377]/ {gsub("[^\\011 -~\\240-\\377]", " ");}
/&/        {
	    /* &-processing! */
	    line = $0;
	    newline = "";
	    while (match(line, "&[#A-Za-z0-9]*") > 0)
	    {
		IGNORECASE=0;
		if (RSTART > 1)
		{
		    newline = newline substr(line, 1, RSTART-1);
		    line = substr(line, RSTART);
		}
		badamp = 0;
		if (RLENGTH > 1)
		{
		    /* Have &-something. */
		    endok = substr(line, 1+RLENGTH, 1) == ";";
		    amp = substr(line, 1+1, RLENGTH-1);
		    if (amp in legalamps)
		    {
			badamp = 0;
		    }
		    else if (substr(amp, 1, 1) == "#")
		    {
			/* Check number. */
			if (substr(amp, 2, 1) == "x" || substr(amp, 2, 1) == "X")
			{
			    val = 0;
			    for (idx = 3; !badamp && idx <= length(amp); idx++)
			    {
				if (!(substr(amp, idx, 1) in hex))
				    badamp = 1;
				else
				    val = val * 16 + hex[substr(amp, idx, 1)];
			    }

			    if (val < 32 || val > 127 && val < (128+32) || val > 255)
				badamp = 1;
			}
			else
			{
			    val = 0;
			    for (idx = 2; !badamp && idx <= length(amp); idx++)
			    {
				if (substr(amp, idx, 1) < "0" || substr(amp, idx, 1) > "9")
				    badamp = 1;
				else
				    val = val * 10 + (substr(amp, idx, 1)+0);
			    }

			    if (val < 32 || val > 127 && val < (128+32) || val > 255)
				badamp = 1;
			}
		    }
		    else
		    {
			badamp = 1;
		    }

		    if (badamp != 0)
		    {
			/* Remove illegal reference. */
			line = substr(line, 1+RLENGTH+endok);
		    }
		    else
		    {
			newline = newline substr(line, 1, RLENGTH);
			newline = newline ";";
			line = substr(line, 1+RLENGTH);
			if (endok)
			    line = substr(line, 2);
		    }
		}
		else
		{
		    /* Single lonely &. */
		    newline = newline "&amp;";
		    line = substr(line, 1+RLENGTH);
		}
		IGNORECASE=1;
	    }

	    $0 = newline line;
           }
/<title>.*<\/title>/ {
	    match($0, "<title>.*</title>");
	    title = substr($0, RSTART+7, RLENGTH-15);
	    printtitle(1);
	    $0 = substr($0, 1, RSTART-1) " " substr($0, RSTART+RLENGTH);
	}
/[<>]/	{
	    if (inscript)
	    {
		if (match($0, "</script") > 0)
		{
		    $0 = substr($0, RSTART);
		    inscript = 0;
		}
		else
		    next;
	    }
	    if (match($0, "<script[^>]*>?") > 0)
	    {
		sstart = RSTART;
		slen = RLENGTH;
		if (match($0, "</script[^>]*>") > 0 && RSTART > sstart)
		{
		    /* Remove center script part. */
		    $0 = substr($0, 1, sstart-1) substr($0, RSTART+RLENGTH);
		}
		else
		{
		    /* Remove end. */
		    $0 = substr($0, 1, sstart-1);
		    inscript = 1;
		}
	    }

	    if (intag)
	    {
		sub("^[^<>]*", "");
		if (match($0, "^[<>]") == 0)
		    next;
		if (match($0, "^>") > 0)
		    $0 = substr($0, 2);
	    }
	    intag = match($0, "<[^<>]*$") > 0;
	    if (intag)
		$0 = $0 ">";

	    if (intitle)
	    {
		if (match($0, "</title") > 0)
		{
		    /* Got end of (a) title. */
		    title = title " " substr($0, 1, RSTART);
		    printtitle(1);
		    $0 = substr($0, RSTART);
		}
		else
		{
		    title = title " " subtags($0);
		    next;
		}
	    }
	    if (match($0, "<title[^<>]*>?") > 0)
	    {
		sstart = RSTART;
		slen = RLENGTH;
		if (match($0, "</title[^<>]*>") > 0 && RSTART > sstart)
		{
		    /* Title is center part. */
		    title = substr($0, sstart, RSTART-sstart);
		    printtitle(1);
		    $0 = substr($0, 1, sstart-1) substr($0, RSTART+RLENGTH);
		}
		else
		{
		    /* Start of title is end of line, which is removed. */
		    title = substr($0, sstart);
		    sub("<title[^>]*>?", "", title);
		    $0 = substr($0, 1, sstart-1);
		    intitle = 1;
		}
	    }

	    $0 = subtags($0);
	}

	{
	    if (intitle > 1)
	    {
		if (length(title)+1+length($0) >= 200)
		{
		    printtitle(0);
		}
		else
		{
		    title = title " " $0;
		    next;
		}
	    }
	    if (indoc && intag <= 1 && inscript <= 1)
	    {
		gsub("  *", " ");
		if (length($0) > 1)
		    print;
	    }

	    if (inscript)
		inscript++;
	    if (intag)
		intag++;
	    if (intitle)
		intitle++;
	}
