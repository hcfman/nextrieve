BEGIN {}
    {
	if ($3 == "Lu" || $3 == "Ll" || $3 == "Lt" || $3 == "Lo" || $3 == "Nd" || $3 == "Nl" || $3 == "No")
	    lastinteresting = nct;
	chartype[$1] = $3;
	sortct[nct++] = $1;
	if (length($6) > 0)
	{
	    decompose[$1] = $6;
	    sortdec[ndec++] = $1;
	}
	next;
    }
function decomposeit(str,   n, a, j, nmore, decmore) {
	    n = split(str, a, " ");
	    for (j = 1; j <= n; j++)
	    {
		if (substr(a[j], 1, 1) == "<")
		    continue;
		if (a[j] in decompose)
		{
		    /* Can decompose further... */
		    decomposeit(decompose[a[j]]);
		}
		else
		{
		    /* Cannot decompose further, delete this char if it's */
		    /* a combining mark (accent), otherwise emit it. */
		    if (!(chartype[a[j]] in combtype))
			fulldec[nfulldec++] = a[j];
		}
	    }
}
END {
	type["Lu"] = 1; /* alpha */
	type["Ll"] = 1;
	type["Lt"] = 1;
	type["Lo"] = 1;
	type["Nd"] = 2; /* numeric. */
	type["Nl"] = 2;
	type["No"] = 2;
	combtype["Mn"] = 0; /* decomposable. */
	combtype["Mc"] = 0;
	combtype["Me"] = 0;
	type["Zs"] = 8; /* space. */
	type["Zl"] = 8;
	type["Zp"] = 8;
	type["Cc"] = 16; /* control (replaced with space during indexing). */
	type["Cf"] = 16;
	/* Print out char type stuff... */
	print sortct[lastinteresting] > "utf8class.txt";
	for (i = 0; i <= lastinteresting; i++)
	{
	    flags = 0;
	    if (chartype[sortct[i]] in type)
		flags = type[chartype[sortct[i]]]
	    if (sortct[i] in decompose)
		flags += 4;
	    if (flags != 0)
		print sortct[i], flags >> "utf8class.txt";
	}

	printf "" > "utf8decomp.txt";

	/* Generate full decompositions. */
	for (i = 0; i < ndec; i++)
	{
	    delete fulldec;
	    nfulldec = 0;
	    decomposeit(decompose[sortdec[i]]);
	    if (nfulldec > 0)
	    {
		printf "%s", sortdec[i] >> "utf8decomp.txt";
		for (j = 0; j < nfulldec; j++)
		    printf " %s", fulldec[j] >> "utf8decomp.txt";
		printf "\n" >> "utf8decomp.txt";
	    }
	}
    }
