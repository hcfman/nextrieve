BEGIN {nlines=0; wanted=0;}
      {
	if (wanted <= 0)
	{
	    r = int(rand()*32);
	    wanted = 1;
	    limit = 16;
	    delta = 16;
	    while (delta > 0 && r >= limit)
	    {
		delta = delta / 2;
		limit = limit + delta;
		wanted++;
	    }
	    if (wanted == 0)
		wanted = 1;
	}

	out = out " " $0;
	if (--wanted <= 0)
	{
	    print out;
	    out = "";
	}
      }
