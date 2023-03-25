<ntv:resource xmlns:ntv="http://www.nextrieve.com/1.0">
    <licensefile name="%%NTVLICENSE%%"/>
    <indexdir name="%%NTVCHROOTBASE%%/%%NTVNAME%%/index"/>
    <logfile name="%%NTVCHROOTBASE%%/%%NTVNAME%%/logs/log.txt"/>

    <indexcreation>
	<attribute name="from"    type="string" key="notkey" nvals="1"/>
	<attribute name="to"      type="string" key="notkey" nvals="1"/>
	<attribute name="cc"      type="string" key="notkey" nvals="1"/>
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

	<!-- string attribute constraints -->
	<vblsub name="vblfromcons" text="from like '&lt;ntv-value&gt;'"/>
	<vbluse name="vblfromcons" type="constraint" class="all"/>
	<vblsub name="vbltocons" text="(to like '&lt;ntv-value&gt;' or cc like '&lt;ntv-value&gt;')"/>
	<vbluse name="vbltocons" type="constraint" class="all"/>
	<vblsub name="vblsubcons" text="subject like '&lt;ntv-value&gt;'"/>
	<vbluse name="vblsubcons" type="constraint" class="all"/>
	<vblsub name="vblnotsubcons" text="(not (subject like '&lt;ntv-value&gt;'))"/>
	<vbluse name="vblnotsubcons" type="constraint" class="all"/>
	<vblsub name="vblnotfromcons" text="(not (from like '&lt;ntv-value&gt;'))"/>
	<vbluse name="vblnotfromcons" type="constraint" class="all"/>
	<vblsub name="vblnottocons" text="(not (to like '&lt;ntv-value&gt;' or cc like '&lt;ntv-value&gt;'))"/>
	<vbluse name="vblnottocons" type="constraint" class="all"/>

	<!-- start checkboxes -->
	<!-- end checkboxes -->
    </ultralite>
</ntv:resource>
