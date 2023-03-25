
	    KEEPING YOUR MAILING LIST ARCHIVE UP TO DATE

You probably want to run things automatically.  Here
are a few suggestions:

(*) You might wish to add the alias line:
        %%NTVNAME%%:     |%%NTVBASE%%/%%NTVNAME%%/bin/saveit
    to your /etc/aliases file, and then run newaliases.
    This is only relevant if you subscribe a user "%%NTVNAME%%" to
    the mailing list.
    NOTE: If you don't already know the permissions required by
    incoming mail messages, you may have to use
        chmod 777 %%NTVBASE%%/%%NTVNAME%%/spool
    receive an incoming mail message, then modify the
    permissions and ownership of %%NTVBASE%%/%%NTVNAME%%/spool
    appropriately to agree with the ownership of the mail message.

(*) Incoming mail for the mailing list related to %%NTVNAME%% should
    be passed to the perl script
        %%NTVBASE%%/%%NTVNAME%%/bin/saveit in order to be saved
    under
        %%NTVBASE%%/%%NTVNAME%%/spool/.
    When this connection has been made, test it by looking in
    %%NTVBASE%%/%%NTVNAME%%/spool/ after a mail message has
    been received for this mailing list to verify that there is, indeed,
    a file containing the email message present.

(*) Periodically (maybe daily) run the command
	%%NTVINST%%/%%VERSION%%/ntvincreindex -B "%%NTVBASE%%" %%NTVNAME%%
    to incorporate mail messages residing under
        %%NTVBASE%%/%%NTVNAME%%/spool/
    into the main index under
        %%NTVBASE%%/%%NTVNAME%%/index/.
    The spooled mail messages will be automatically removed
    and appended to appropriate mailbox files under
        %%NTVBASE%%/%%NTVNAME%%/archive/.

(*) After indexing run the command
        %%NTVBASE%%/%%NTVNAME%%/bin/%%NTVNAME%%adjust
    to create the include file with the checkboxes for date selection
    and the datemap include file for colored boxes as well as editing
    the resource file to setup for selecting on the checkboxes

(*) At any time a full reindex can be performed by using the command:
        %%NTVINST%%/%%VERSION%%/ntvfullreindex -B "%%NTVBASE%%" %%NTVNAME%%

(*) If you have any existing email files you want to index, use:
        %%NTVBASE%%/%%NTVNAME%%/bin/splitemails < mailfile
    for each mail file you have to append these messages to appropriate
    archive files under %%NTVBASE%%/%%NTVNAME%%/archive/.
    Then run:
        %%NTVINST%%/%%VERSION%%/ntvfullreindex -B "%%NTVBASE%%" %%NTVNAME%%
