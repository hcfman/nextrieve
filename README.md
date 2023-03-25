# NexTrieve - A fuzzy text retrieval system

*UNDER CONSTRUCTION*

Note: THIS CODE ISN'T BUILDING JUST YET, It dates back to 2002/2003.

I've recently looked it up and placed it here for me it's part of the Dutch Internet historical reference.

I originally wrote it around 1995 when I had a company called Nexial Systems. Later, it became part of a National Search engine called Search.NL. Search.NL was one of the first national search engines in the Netherlands.

Here's one snapshot in the wayback machine

https://web.archive.org/web/20021125174556/http://www.search.nl/

There's a small piece of history about this (In Dutch) here

https://www.eindhovenfotos.nl/1/ilse.nl.html#news1-y8

According to the above artikel, in 1999 Computable magazine referred to Search.NL as the best scoring Dutch search engine, Altavista and Hotbot were first and second. I never read that article myself but I'm proud to read it in this history page.

I also started one of the first search engines in New Zealand, the URL for this still works and goes to a search engine, though I understand they use a different search engine now. The look and feel is pretty much the same as when I sold it to NZcity:

http://www.searchnz.co.nz/

It's based on tri-grams together with a secondary extra weighting step. The indexes use a technique for efficient document references based on a book called "Managing gigabytes". It was able to index 10GB of text with both fuzzy and exact indexes in about 5 hours on a Pentium IV with 2GB of ram. Not the fastest but not the slowest either. Not too bad for just 2GB total ram on the platform.

Before that it was used to index several mailing lists when the formative years of the Internet were under construction, such as:

* Cisco mailing list
* greatcircle.com's firewalls mailing list
* bugtraq
* ascend mailing list
* Livingston mailing list

It was also used to index a collection of documents surrounding "The IRT-affaire" (https://nl.wikipedia.org/wiki/Parlementaire_enqu%C3%AAte_opsporingsmethoden), a collection of documents obtained by Buro Jansen and Janssen and made public on the NRC Handelsblad (https://web.archive.org/web/20141007111749/https://retro.nrc.nl/W2/Archief/Hints/filmhints.html) (Current archive: https://www.burojansen.nl/traa/) that relates to abuse of police and justice department powers. At that time as I understand it a team called the IRTs or "Interregionaal Recherche Teams" (Inter-regional detective teams) were working effectively making criminals rich. This created a huge scandal. At the time my Dutch wasn't sufficiently good for me to understand it well, nor did it seem particularly relevant to me at the time. In any case, I'm proud to have provided the search facilities for such an important case for the Dutch democracy.

I have intentions to get it building sometime but to be honest it should be re-written in a language like rust for more memory safety.

Kim Hendrikse
