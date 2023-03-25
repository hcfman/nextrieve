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

#include "expat.h"

/*
 * Used to analyze XML attributes.
 */
typedef struct
{
    char const *attr_name;
    int         attr_required; /* TRUE implies this attribute must be present.*/

    /* NOTE: only one of res_string or res_int should be set. */
    unsigned char const **res_string; /* string result placed here */
                                      /* (not allocated). */
    long        *res_int;    /* int result put here. */
} ntvxml_attrinfo_t;

int ntvXML_analyze_attrs
	    (
		XML_Parser *p,
		char const *el,
		ntvxml_attrinfo_t *attrinfo,
		char const **attr,
		unsigned char **emsg,
		unsigned char **wmsg
	    );
