
				HTMLSIMPLE

You've just installed a basic HTML/DOC/PDF indexer.

(*) At any time a full reindex can be performed by using the command:
        %%NTVINST%%/%%VERSION%%/bin/ntvfullreindex -B "%%NTVBASE%%" %%NTVNAME%%
    This will take all nominated files (*.htm* and/or *.doc and/or *.pdf)
    under %%NTVBASEDIR%% and put them in the index present under
    %%NTVBASE%%/%%NTVNAME%%/index.

    These files are converted to XML using an appropriate filter under
    %%NTVINST%%/%%VERSION%%/bin/filters.  These filters produce text,
    title and filename attributes.

(*) The default HTML templates show the title text in a link to a
    small program that simply displays the original file if clicked.


TYPICAL ADDITIONS

    - You may want to have a "url" parameter generated from someplace
      else, such as an external database.  Modify a copy of
      of ntvhtml2ntvml to produce the url from somewhere for each
      converted file (normally this information is present in some
      external database), and call the modified program from
      %%NTVBASE%%/%%NTVNAME%%/bin/%%NTVNAME%%fullxml.
      You may have to modify the resource file
      (%%NTVBASE%%/%%NTVNAME%%/%%NTVNAME%%.res) to add the extra string
      "url" parameter, and you'll have to modify some templates to
      correctly use "url" for display.
