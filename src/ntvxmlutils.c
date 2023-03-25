/*
Copyright (c) 2003 Kim Hendrikse

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <config.h>

#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#include "ntvstandard.h"
#include "ntvxmlutils.h"
#include "ntverror.h"
#include "ntvmemlib.h"


/*
 * USEFUL XML RELATED STUFF.
 */

/*
 * ntvXML_analyze_attrs
 *
 * Given attributes and an attrinfo table, analyze the attributes.
 * Returns TRUE for good results, FALSE for an error.
 */
int ntvXML_analyze_attrs
	    (
		XML_Parser *p,
		char const *el,
		ntvxml_attrinfo_t *attrinfo,
		char const **attr,
		unsigned char **emsg,
		unsigned char **wmsg
	    )
{
    ntvxml_attrinfo_t *ai;

    if (emsg != NULL)
	*emsg = NULL;
    if (wmsg != NULL)
	*wmsg = NULL;

    /* Initialize everything to "not present". */
    for (ai = attrinfo; ai->attr_name != NULL; ai++)
	if (ai->res_string != NULL)
	    *ai->res_string = NULL;
	else if (ai->res_int != NULL)
	    *ai->res_int = -1;

    /* Scan given attributes. */
    for (; attr[0] != NULL; attr += 2)
    {
	for (ai = attrinfo; ai->attr_name != NULL; ai++)
	{
	    unsigned char const *sp1 = attr[0];
	    unsigned char const *sp2 = ai->attr_name;
	    for (; *sp1 != 0 && *sp1 == *sp2; sp1++, sp2++)
		; /* Do nothing. */

	    if (*sp1 == *sp2)
	    {
		if (ai->res_string != NULL)
		    *ai->res_string = attr[1];
		else if (ai->res_int != NULL)
		{
		    unsigned char const *scan;

		    /* Accept optional leading -, then digits. */
		    scan = attr[1];
		    while (isspace(*scan))
			scan++;
		    if (*scan == '-')
			scan++;
		    while (isdigit(*scan))
			scan++;
		    if (attr[1][0] == 0 || *scan != 0 || !isdigit(*(scan-1)))
		    {
			if (emsg != NULL)
			    *emsg = genmessage
				    (
					"XML: bad numeric value for attribute \"%s\","
					  " element \"%s\" at line %d",
					attr[0], el, XML_GetCurrentLineNumber(p)
				    );
			return FALSE;
		    }
		    *(ai->res_int) = atol(attr[1]);
		}
		break;
	    }
	}

	if (ai->attr_name == NULL)
	{
	    if (wmsg != NULL && *wmsg == NULL)
		*wmsg = genmessage
			(
			    "XML: ignoring unexpected attribute \"%s\" in \"<%s>\""
				" at line %d",
			    attr[0],
			    el,
			    XML_GetCurrentLineNumber(p)
			);
	}
    }

    /* Ensure required attributes have been specified... */
    for (ai = attrinfo; ai->attr_name != NULL; ai++)
	if
	(
	    ai->attr_required
	    &&
	    (
		(ai->res_string != NULL && *ai->res_string == NULL)
		|| (ai->res_int != NULL && *ai->res_int == -1)
	    )
	)
	{
	    if (emsg != NULL)
		*emsg = genmessage
			(
			    "XML: Required \"%s=\" attribute missing from \"%s\""
				" element at line %d.",
			    ai->attr_name,
			    el,
			    XML_GetCurrentLineNumber(p)
			);
	    return FALSE;
	}

    return TRUE;
}


