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

typedef enum {
    FALSERESULT,
    TRUERESULT,
    NOP2,
    NOP3,
    NOP4,
    NOP5,
    NOP6,
    NOP7,
    JLSS, 	    	/* Pad out to the addressing mode bits */
    NTJLSS,
    TNJLSS,
    TTJLSS,
    SSJLSS,
    STJLSS,
    TSJLSS,
    TT_S_JLSS,
    JGTR,
    NTJGTR,
    TNJGTR,
    TTJGTR,
    SSJGTR,
    STJGTR,
    TSJGTR,
    TT_S_JGTR,
    JLEQ,
    NTJLEQ,
    TNJLEQ,
    TTJLEQ,
    SSJLEQ,
    STJLEQ,
    TSJLEQ,
    TT_S_JLEQ,
    JGEQ,
    NTJGEQ,
    TNJGEQ,
    TTJGEQ,
    SSJGEQ,
    STJGEQ,
    TSJGEQ,
    TT_S_JGEQ,
    JEQUAL,
    NTJEQUAL,
    TNJEQUAL,
    TTJEQUAL,
    SSJEQUAL,
    STJEQUAL,
    TSJEQUAL,
    TT_S_JEQUAL,
    JNEQUAL,
    NTJNEQUAL,
    TNJNEQUAL,
    TTJNEQUAL,
    SSJNEQUAL,
    STJNEQUAL,
    TSJNEQUAL,
    TT_S_JNEQUAL,
    JLIKE,
    JNLIKE,
    ATTROR,
    ATTRORI,
    NATTROR,
    NATTRORI,
    ATTRAND,
    ATTRANDI,
    NATTRAND,
    NATTRANDI,
    NEXIST,
    INNEQUAL,
    INEQUAL,
} instruction_t;


extern unsigned long *ntvCompileQuery(reqbuffer_t *req, unsigned long *simple);
extern void ntvCompileFree(unsigned long *codeBuffer);
