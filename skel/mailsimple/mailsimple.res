<ntv:resource xmlns:ntv="http://www.nextrieve.com/1.0">
    <indexdir name="%%NTVCHROOTBASE%%/%%NTVNAME%%/index"/>
    <logfile name="%%NTVCHROOTBASE%%/%%NTVNAME%%/logs/log.txt"/>

    <indexcreation>
	<attribute name="from"    type="string" key="notkey" nvals="1"/>
	<attribute name="to"      type="string" key="notkey" nvals="*"/>
	<attribute name="subject" type="string" key="notkey" nvals="1"/>
	<attribute name="date"    type="number" key="notkey" nvals="1"/>
	<attribute name="mailbox" type="string" key="notkey" nvals="1"/>
	<attribute name="offset"  type="number" key="notkey" nvals="1"/>
	<attribute name="length"  type="number" key="notkey" nvals="1"/>

	<texttype  name="subject"/>
    </indexcreation>

    <searching>
	<querylog path="%%NTVCHROOTBASE%%/%%NTVNAME%%/logs"/>
    </searching>

    <ultralite>
	<!-- %%NTVSERVER%%: replaced with server host:port if client. -->

        <log value="1"/>
        <emitok value="%%NTVEMITOK%%"/>

	<!-- searching on subjects -->
	<vblsub name="vblsubjectsonly" text="subject"/>
	<vbluse name="vblsubjectsonly" type="texttype"/>

	<!-- all-words option. -->
	<vblsub name="vblall" text="all"/>
	<vbluse name="vblq" type="text" class="&lt;ntv-vbl vblall&gt;"/>
    </ultralite>
</ntv:resource>
