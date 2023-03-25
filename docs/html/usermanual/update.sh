#!/bin/sh

./pod2htmlfilt.sh ../../manpages/ntvcachecfgxml.pod "Caching Server Configuration File"
./pod2htmlfilt.sh ../../manpages/ntvcached.pod "Running the Caching Server"
./pod2htmlfilt.sh ../../manpages/ntvcheck.pod "Text Index Integrity Checking"
./pod2htmlfilt.sh ../../manpages/ntvdtds.pod "DTDs"
./pod2htmlfilt.sh ../../manpages/ntvhitlistxml.pod "Result List XML"
./pod2htmlfilt.sh ../../manpages/ntvindex.pod "Creating an Index with Ntvindex"
./pod2htmlfilt.sh ../../manpages/ntvindexerxml.pod "Indexer XML"
./pod2htmlfilt.sh ../../manpages/ntvopt.pod "Optimizing an Index with Ntvopt"
./pod2htmlfilt.sh ../../manpages/ntvquerygen.pod "Query-XML Generator"
./pod2htmlfilt.sh ../../manpages/ntvqueryxml.pod "Query XML"
./pod2htmlfilt.sh ../../manpages/ntvresourcefile.pod "Text Index Resource File"
./pod2htmlfilt.sh ../../manpages/ntvsearch.pod "Searching an Index with Ntvsearch"
./pod2htmlfilt.sh ../../manpages/ntvsearchd.pod "Running a Search Server"
./pod2htmlfilt.sh ../../manpages/ntvultralite.pod "HTML Template Filling"
./pod2htmlfilt.sh ../../manpages/ntvtxtfilter.pod "Text to index-XML filter"
./pod2htmlfilt.sh ../../manpages/ntvpdffilter.pod "PDF to index-XML filter"
./pod2htmlfilt.sh ../../manpages/ntvdocfilter.pod "DOC to index-XML filter"
./pod2htmlfilt.sh ../../manpages/ntvhtmlfilter.pod "HTML to index-XML filter"
./pod2htmlfilt.sh ../../manpages/ntvmailfilter.pod "MAIL to index-XML filter"

echo cp ../../manpages/bgblend.gif .
cp ../../manpages/bgblend.gif .
