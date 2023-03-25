<ntv:resource xmlns:ntv="http://www.nextrieve.com/1.0">
    <indexdir name="testidx"/>
    <logfile name="testidx/log.txt"/>

    <indexcreation>
	<attribute name="flag"     type="flag"   key="notkey" nvals="1"/>
	<attribute name="number"   type="number" key="notkey" nvals="1"/>
	<attribute name="title"    type="string" key="notkey" nvals="1"/>
	<attribute name="filename" type="string" key="key-unique" nvals="1"/>

	<texttype  name="title"/>
    </indexcreation>

    <searching>
	<querylog path="testidx"/>
    </searching>

    <ultralite>
	<!-- %%NTVSERVER%%: replaced with server host:port if client. -->

	<log value="1"/>

	<!-- searching on titles -->
	<vblsub name="vbltitlesonly" text="title"/>
	<vbluse name="vbltitlesonly" type="texttype"/>

	<!-- all-words option. -->
	<vblsub name="vblall" text="all"/>
	<vbluse name="vblq" type="text" class="&lt;ntv-vbl vblall&gt;"/>
    </ultralite>
</ntv:resource>
