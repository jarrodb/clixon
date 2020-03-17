/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * Clixon XML object parse and print functions
 * @see https://www.w3.org/TR/2008/REC-xml-20081126
 *      https://www.w3.org/TR/2009/REC-xml-names-20091208
 * Canonical XML version (just for info)
 *      https://www.w3.org/TR/xml-c14n
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h" /* xml_spec_populate */
#include "clixon_yang_module.h"
#include "clixon_xml_map.h" /* xml_spec_populate */
#include "clixon_xml_vec.h"
#include "clixon_xml_sort.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_parse.h"
#include "clixon_xml_io.h"

/*
 * Constants
 */
/* Size of xml read buffer */
#define BUFLEN 1024  
/* Indentation for xml pretty-print. Consider option? */
#define XML_INDENT 3
/* Name of xml top object created by xml parse functions */
#define XML_TOP_SYMBOL "top" 


/*------------------------------------------------------------------------
 * XML printing functions. Output a parse tree to file, string cligen buf
 *------------------------------------------------------------------------*/

/*! Print an XML tree structure to an output stream and encode chars "<>&"
 *
 * @param[in]   f           UNIX output stream
 * @param[in]   xn          clicon xml tree
 * @param[in]   level       how many spaces to insert before each line
 * @param[in]   prettyprint insert \n and spaces tomake the xml more readable.
 * @see clicon_xml2cbuf
 * One can use clicon_xml2cbuf to get common code, but using fprintf is
 * much faster than using cbuf and then printing that,...
 */
int
clicon_xml2file(FILE  *f, 
		cxobj *x, 
		int    level, 
		int    prettyprint)
{
    int    retval = -1;
    char  *name;
    char  *namespace;
    cxobj *xc;
    int    hasbody;
    int    haselement;
    char  *val;
    char  *encstr = NULL; /* xml encoded string */
    
    if (x == NULL)
	goto ok;
    name = xml_name(x);
    namespace = xml_prefix(x);
    switch(xml_type(x)){
    case CX_BODY:
	if ((val = xml_value(x)) == NULL) /* incomplete tree */
	    break;
	if (xml_chardata_encode(&encstr, "%s", val) < 0)
	    goto done;
	fprintf(f, "%s", encstr);
	break;
    case CX_ATTR:
	fprintf(f, " ");
	if (namespace)
	    fprintf(f, "%s:", namespace);
	fprintf(f, "%s=\"%s\"", name, xml_value(x));
	break;
    case CX_ELMNT:
	fprintf(f, "%*s<", prettyprint?(level*XML_INDENT):0, "");
	if (namespace)
	    fprintf(f, "%s:", namespace);
	fprintf(f, "%s", name);
	hasbody = 0;
	haselement = 0;
	xc = NULL;
	/* print attributes only */
	while ((xc = xml_child_each(x, xc, -1)) != NULL) {
	    switch (xml_type(xc)){
	    case CX_ATTR:
		if (clicon_xml2file(f, xc, level+1, prettyprint) <0)
		    goto done;
		break;
	    case CX_BODY:
		hasbody=1;
		break;
	    case CX_ELMNT:
		haselement=1;
		break;
	    default:
		break;
	    }
	}
	/* Check for special case <a/> instead of <a></a>:
	 * Ie, no CX_BODY or CX_ELMNT child.
	 */
	if (hasbody==0 && haselement==0) 
	    fprintf(f, "/>");
	else{
	    fprintf(f, ">");
	    if (prettyprint && hasbody == 0)
		    fprintf(f, "\n");
	    xc = NULL;
	    while ((xc = xml_child_each(x, xc, -1)) != NULL) {
		if (xml_type(xc) != CX_ATTR)
		    if (clicon_xml2file(f, xc, level+1, prettyprint) <0)
			goto done;
	    }
	    if (prettyprint && hasbody==0)
		fprintf(f, "%*s", level*XML_INDENT, "");
	    fprintf(f, "</");
	    if (namespace)
		fprintf(f, "%s:", namespace);
	    fprintf(f, "%s>", name);
	}
	if (prettyprint)
	    fprintf(f, "\n");
	break;
    default:
	break;
    }/* switch */
 ok:
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}

/*! Print an XML tree structure to an output stream
 *
 * Uses clicon_xml2file internally
 *
 * @param[in]   f           UNIX output stream
 * @param[in]   xn          clicon xml tree
 * @see clicon_xml2cbuf
 * @see clicon_xml2file
 */
int
xml_print(FILE  *f, 
	  cxobj *xn)
{
    return clicon_xml2file(f, xn, 0, 1);
}

/*! Print an XML tree structure to a cligen buffer and encode chars "<>&"
 *
 * @param[in,out] cb          Cligen buffer to write to
 * @param[in]     xn          Clicon xml tree
 * @param[in]     level       Indentation level for prettyprint
 * @param[in]     prettyprint insert \n and spaces tomake the xml more readable.
 * @param[in]     depth       Limit levels of child resources: -1 is all, 0 is none, 1 is node itself
 *
 * @code
 * cbuf *cb;
 * cb = cbuf_new();
 * if (clicon_xml2cbuf(cb, xn, 0, 1, -1) < 0)
 *   goto err;
 * fprintf(stderr, "%s", cbuf_get(cb));
 * cbuf_free(cb);
 * @endcode
 * @see  clicon_xml2file
 */
int
clicon_xml2cbuf(cbuf   *cb, 
		cxobj  *x, 
		int     level,
		int     prettyprint,
		int32_t depth)
{
    int    retval = -1;
    cxobj *xc;
    char  *name;
    int    hasbody;
    int    haselement;
    char  *namespace;
    char  *encstr = NULL; /* xml encoded string */
    char  *val;
    
    if (depth == 0)
	goto ok;
    name = xml_name(x);
    namespace = xml_prefix(x);
    switch(xml_type(x)){
    case CX_BODY:
	if ((val = xml_value(x)) == NULL) /* incomplete tree */
	    break;
	if (xml_chardata_encode(&encstr, "%s", val) < 0)
	    goto done;
	cprintf(cb, "%s", encstr);
	break;
    case CX_ATTR:
	cprintf(cb, " ");
	if (namespace)
	    cprintf(cb, "%s:", namespace);
	cprintf(cb, "%s=\"%s\"", name, xml_value(x));
	break;
    case CX_ELMNT:
	cprintf(cb, "%*s<", prettyprint?(level*XML_INDENT):0, "");
	if (namespace)
	    cprintf(cb, "%s:", namespace);
	cprintf(cb, "%s", name);
	hasbody = 0;
	haselement = 0;
	xc = NULL;
	/* print attributes only */
	while ((xc = xml_child_each(x, xc, -1)) != NULL) 
	    switch (xml_type(xc)){
	    case CX_ATTR:
		if (clicon_xml2cbuf(cb, xc, level+1, prettyprint, -1) < 0)
		    goto done;
		break;
	    case CX_BODY:
		hasbody=1;
		break;
	    case CX_ELMNT:
		haselement=1;
		break;
	    default:
		break;
	    }
	/* Check for special case <a/> instead of <a></a> */
	if (hasbody==0 && haselement==0) 
	    cprintf(cb, "/>");
	else{
	    cprintf(cb, ">");
	    if (prettyprint && hasbody == 0)
		cprintf(cb, "\n");
	    xc = NULL;
	    while ((xc = xml_child_each(x, xc, -1)) != NULL) 
		if (xml_type(xc) != CX_ATTR)
		    if (clicon_xml2cbuf(cb, xc, level+1, prettyprint, depth-1) < 0)
			goto done;
	    if (prettyprint && hasbody == 0)
		cprintf(cb, "%*s", level*XML_INDENT, "");
	    cprintf(cb, "</");
	    if (namespace)
		cprintf(cb, "%s:", namespace);
	    cprintf(cb, "%s>", name);
	}
	if (prettyprint)
	    cprintf(cb, "\n");
	break;
    default:
	break;
    }/* switch */
 ok:
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}

/*! Print actual xml tree datastructures (not xml), mainly for debugging
 * @param[in,out] cb          Cligen buffer to write to
 * @param[in]     xn          Clicon xml tree
 * @param[in]     level       Indentation level
 */
int
xmltree2cbuf(cbuf  *cb, 
	     cxobj *x,
	     int    level)
{
    cxobj *xc;
    int    i;

    for (i=0; i<level*XML_INDENT; i++)
	cprintf(cb, " ");
    if (xml_type(x) != CX_BODY)
	cprintf(cb, "%s", xml_type2str(xml_type(x)));
    if (xml_prefix(x)==NULL)
	cprintf(cb, " %s", xml_name(x));
    else
	cprintf(cb, " %s:%s", xml_prefix(x), xml_name(x));
    if (xml_value(x))
	cprintf(cb, " value:\"%s\"", xml_value(x));
    if (xml_flag(x, 0xff))
	cprintf(cb, " flags:0x%x", xml_flag(x, 0xff));
    if (xml_child_nr(x))
	cprintf(cb, " {");
    cprintf(cb, "\n");
    xc = NULL;
    while ((xc = xml_child_each(x, xc, -1)) != NULL) 
	xmltree2cbuf(cb, xc, level+1);
    if (xml_child_nr(x)){
	for (i=0; i<level*XML_INDENT; i++)
	    cprintf(cb, " ");
	cprintf(cb, "}\n");
    }
    return 0;
}

/*--------------------------------------------------------------------
 * XML parsing functions. Create XML parse tree from string and file.
 *--------------------------------------------------------------------*/
/*! Common internal xml parsing function string to parse-tree
 *
 * Given a string containing XML, parse into existing XML tree and return
 * @param[in]     str   Pointer to string containing XML definition. 
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification (only if bind is TOP or CONFIG)
 * @param[in,out] xtop  Top of XML parse tree. Assume created. Holds new tree.
 * @param[out]    xerr  Reason for failure (yang assignment not made)
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval       -1     Error with clicon_err called. Includes parse error
 * @see xml_parse_file
 * @see xml_parse_string
 * @see xml_parse_va
 * @see _json_parse
 * @note special case is empty XML where the parser is not invoked.
 */
static int 
_xml_parse(const char    *str, 
	   enum yang_bind yb,
	   yang_stmt     *yspec,
	   cxobj         *xt,
	   cxobj        **xerr)
{
    int             retval = -1;
    clixon_xml_yacc xy = {0,};
    cxobj          *x;
    int             ret;
    int             failed = 0; /* yang assignment */
    int             i;

    clicon_debug(1, "%s %s", __FUNCTION__, str);
    if (strlen(str) == 0)
	return 0; /* OK */
    if (xt == NULL){
	clicon_err(OE_XML, errno, "Unexpected NULL XML");
	return -1;	
    }
    if ((xy.xy_parse_string = strdup(str)) == NULL){
	clicon_err(OE_XML, errno, "strdup");
	return -1;
    }
    xy.xy_xtop = xt;
    xy.xy_xparent = xt;
    xy.xy_yspec = yspec;
    if (clixon_xml_parsel_init(&xy) < 0)
	goto done;    
    if (clixon_xml_parseparse(&xy) != 0)  /* yacc returns 1 on error */
	goto done;
    /* Purge all top-level body objects */
    x = NULL;
    while ((x = xml_find_type(xt, NULL, "body", CX_BODY)) != NULL)
	xml_purge(x);
    /* Traverse new objects */
    for (i = 0; i < xy.xy_xlen; i++) {
	x = xy.xy_xvec[i];
	/* Verify namespaces after parsing */
	if (xml_apply0(x, CX_ELMNT, xml_localname_check, NULL) < 0)
	    goto done;
	/* Populate, ie associate xml nodes with yang specs 
	 */
	switch (yb){
	case YB_RPC:
	case YB_UNKNOWN:
	case YB_NONE:
	    break;
	case YB_PARENT:
	    /* xt:n         Has spec
	     * x:   <a> <-- populate from parent
	     */
	    if ((ret = xml_spec_populate0_parent(x, xerr)) < 0)
		goto done;
	    if (ret == 0)
		failed++;
	    break;

	case YB_TOP:
	    /* xt:<top>     nospec
	     * x:   <a> <-- populate from modules
	     */
#ifdef XMLDB_CONFIG_HACK
	    if (strcmp(xml_name(x),"config") == 0 ||
		strcmp(xml_name(x),"data") == 0){
		/* xt:<top>         nospec
		 * x:   <config>
		 *         <a>  <-- populate from modules
		 */
		if ((ret = xml_spec_populate(x, yspec, xerr)) < 0)
		    goto done;
	    }
	    else
#endif
	    if ((ret = xml_spec_populate0(x, yspec, xerr)) < 0)
		goto done;
	    if (ret == 0)
		failed++;
	    break;
	}
    }
    /* Sort the complete tree after parsing. Sorting is less meaningful if Yang not bound */
    if (xml_apply0(xt, CX_ELMNT, xml_sort, NULL) < 0)
	goto done;
    retval = (failed==0) ? 1 : 0;
  done:
    clixon_xml_parsel_exit(&xy);
    if (xy.xy_parse_string != NULL)
	free(xy.xy_parse_string);
    if (xy.xy_xvec)
	free(xy.xy_xvec);
    return retval; 
}

/*! Read an XML definition from file and parse it into a parse-tree. 
 *
 * @param[in]     fd  A file descriptor containing the XML file (as ASCII characters)
 * @param[in]     yspec   Yang specification, or NULL
 * @param[in,out] xt   Pointer to XML parse tree. If empty, create.
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial)
 * @retval       -1     Error with clicon_err called. Includes parse error *
 * @code
 *  cxobj *xt = NULL;
 *  int    fd;
 *  fd = open(filename, O_RDONLY);
 *  xml_parse_file(fd, yspec, &xt);
 *  xml_free(xt);
 * @endcode
 * @see xml_parse_string
 * @see xml_parse_va
 * @note, If xt empty, a top-level symbol will be added so that <tree../> will be:  <top><tree.../></tree></top>
 * @note May block on file I/O
 * @see xml_parse_file2 for a more advanced API
 */
int 
xml_parse_file(int        fd, 
	       yang_stmt *yspec,
	       cxobj    **xt)
{
    enum yang_bind yb = YB_PARENT;

    if (xt==NULL){
	clicon_err(OE_XML, EINVAL, "xt is NULL");
	return -1;
    }
    if (*xt==NULL)
	yb = YB_TOP;
    return xml_parse_file2(fd, yb, yspec, NULL, xt, NULL);
}

/*! FSM to detect substring
 */
static inline int
FSM(char *tag, 
    char  ch, 
    int   state)
{
    if (tag[state] == ch)
	return state+1;
    else
	return 0;
}

/*! Read an XML definition from file and parse it into a parse-tree, advanced API
 *
 * @param[in]     fd    A file descriptor containing the XML file (as ASCII characters)
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification (only if bind is TOP or CONFIG)
 * @param[in]     endtag  Read until encounter "endtag" in the stream, or NULL
 * @param[in,out] xt    Pointer to XML parse tree. If empty, create.
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval       -1     Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  cxobj *xerr = NULL;
 *  int    fd;
 *  fd = open(filename, O_RDONLY);
 *  if ((ret = xml_parse_file2(fd, YB_TOP, yspec, "</config>", &xt, &xerr)) < 0)
 *    err;
 *  xml_free(xt);
 * @endcode
 * @see xml_parse_string
 * @see xml_parse_file
 * @note, If xt empty, a top-level symbol will be added so that <tree../> will be:  <top><tree.../></tree></top>
 * @note May block on file I/O
 */
int 
xml_parse_file2(int            fd, 
		enum yang_bind yb,
		yang_stmt     *yspec,
		char          *endtag,
		cxobj        **xt,
		cxobj        **xerr)
{
    int   retval = -1;
    int   ret;
    int   len = 0;
    char  ch;
    char *xmlbuf = NULL;
    char *ptr;
    int   xmlbuflen = BUFLEN; /* start size */
    int   endtaglen = 0;
    int   state = 0;
    int   oldxmlbuflen;
    int   failed = 0;

    if (endtag != NULL)
	endtaglen = strlen(endtag);
    if ((xmlbuf = malloc(xmlbuflen)) == NULL){
	clicon_err(OE_XML, errno, "malloc");
	goto done;
    }
    memset(xmlbuf, 0, xmlbuflen);
    ptr = xmlbuf;
    while (1){
	if ((ret = read(fd, &ch, 1)) < 0){
	    clicon_err(OE_XML, errno, "read: [pid:%d]", 
		    (int)getpid());
	    break;
	}
	if (ret != 0){
	    if (endtag)
		state = FSM(endtag, ch, state);
	    xmlbuf[len++] = ch;
	}
	if (ret == 0 ||
	    (endtag && (state == endtaglen))){
	    state = 0;
	    if (*xt == NULL)
		if ((*xt = xml_new(XML_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
		    goto done;
	    if ((ret = _xml_parse(ptr, yb, yspec, *xt, xerr)) < 0)
		goto done;
	    if (ret == 0)
		failed++;
	    break;
	}
	if (len>=xmlbuflen-1){ /* Space: one for the null character */
	    oldxmlbuflen = xmlbuflen;
	    xmlbuflen *= 2;
	    if ((xmlbuf = realloc(xmlbuf, xmlbuflen)) == NULL){
		clicon_err(OE_XML, errno, "realloc");
		goto done;
	    }
	    memset(xmlbuf+oldxmlbuflen, 0, xmlbuflen-oldxmlbuflen);
	    ptr = xmlbuf;
	}
    } /* while */
    retval = (failed==0) ? 1 : 0;
 done:
    if (retval < 0 && *xt){
	free(*xt);
	*xt = NULL;
    }
    if (xmlbuf)
	free(xmlbuf);
    return retval;
}

/*! Read an XML definition from string and parse it into a parse-tree, advanced API
 *
 * @param[in]     str   String containing XML definition. 
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification, or NULL
 * @param[in,out] xt    Pointer to XML parse tree. If empty will be created.
 * @param[out]    xerr  Reason for failure (yang assignment not made)
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial)
 * @retval       -1     Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  cxobj *xerr = NULL;
 *  if (xml_parse_string2(str, YB_TOP, yspec, &xt, &xerr) < 0)
 *    err;
 *  if (xml_rootchild(xt, 0, &xt) < 0) # If you want to remove TOP
 *    err;
 * @endcode
 * @see xml_parse_file
 * @see xml_parse_va
 * @note You need to free the xml parse tree after use, using xml_free()
 * @note If empty on entry, a new TOP xml will be created named "top"
 */
int 
xml_parse_string2(const char    *str, 
		  enum yang_bind yb,
		  yang_stmt     *yspec,
		  cxobj        **xt,
		  cxobj        **xerr)
{
    if (xt==NULL){
	clicon_err(OE_XML, EINVAL, "xt is NULL");
	return -1;
    }
    if (*xt == NULL){
	if ((*xt = xml_new(XML_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
	    return -1;
    }
    return _xml_parse(str, yb, yspec, *xt, xerr);
}

/*! Read an XML definition from string and parse it into a parse-tree
 *
 * @param[in]     str   String containing XML definition. 
 * @param[in]     yspec Yang specification, or NULL
 * @param[in,out] xt    Pointer to XML parse tree. If empty will be created.
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial)
 * @retval       -1     Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  if (xml_parse_string(str, yspec, &xt) < 0)
 *    err;
 *  if (xml_rootchild(xt, 0, &xt) < 0) # If you want to remove TOP
 *    err;
 * @endcode
 * @see xml_parse_file
 * @see xml_parse_va
 * @note You need to free the xml parse tree after use, using xml_free()
 * @note If xt is empty on entry, a new TOP xml will be created named "top" and yang binding 
 *       assumed to be TOP
 */
int 
xml_parse_string(const char *str, 
		 yang_stmt  *yspec,
		 cxobj     **xt)
{
    enum yang_bind yb = YB_PARENT;

    if (xt==NULL){
	clicon_err(OE_XML, EINVAL, "xt is NULL");
	return -1;
    }
    if (*xt == NULL){
	yb = YB_TOP; /* ad-hoc #1 */
	if ((*xt = xml_new(XML_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
	    return -1;
    }
    else{
	if (xml_spec(*xt) == NULL)
	    yb = YB_TOP;  /* ad-hoc #2 */
    }
    return _xml_parse(str, yb, yspec, *xt, NULL);
}

/*! Read XML from var-arg list and parse it into xml tree
 *
 * Utility function using stdarg instead of static string.
 * @param[in,out] xtop   Top of XML parse tree. If it is NULL, top element 
                         called 'top' will be created. Call xml_free() after use
 * @param[in]     yspec  Yang specification, or NULL
 * @param[in]     format Format string for stdarg according to printf(3)
 * @retval        1      Parse OK and all yang assignment made
 * @retval        0      Parse OK but yang assigment not made (or only partial)
 * @retval       -1      Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  if (xml_parse_va(&xt, NULL, "<xml>%d</xml>", 22) < 0)
 *    err;
 *  xml_free(xt);
 * @endcode
 * @see xml_parse_string
 * @see xml_parse_file
 * @note If vararg list is empty, consider using xml_parse_string()
 */
int 
xml_parse_va(cxobj     **xtop,
	     yang_stmt  *yspec,		 
	     const char *format, ...)
{
    int     retval = -1;
    va_list args;
    char   *str = NULL;
    int     len;

    va_start(args, format);
    len = vsnprintf(NULL, 0, format, args) + 1;
    va_end(args);
    if ((str = malloc(len)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(str, 0, len);
    va_start(args, format);
    len = vsnprintf(str, len, format, args) + 1;
    va_end(args);
    retval = xml_parse_string(str, yspec, xtop); /* xml_parse_string2 */
 done:
    if (str)
	free(str);
    return retval;
}

#ifdef NOTUSED
/*! Generic parse function for xml values
 * @param[in]   xb       xml tree body node, ie containing a value to be parsed
 * @param[in]   type     Type of value to be parsed in value
 * @param[out]  cvp      CLIgen variable containing the parsed value
 * @note free cv with cv_free after use.
 * @see xml_body_int32   etc, for type-specific parse functions
 * @note range check failure returns 0
 */
static int
xml_body_parse(cxobj       *xb,
	       enum cv_type type,
	       cg_var     **cvp)
{
    int     retval = -1;
    cg_var *cv = NULL;
    int     cvret;
    char   *bstr;
    char   *reason = NULL;

    if ((bstr = xml_body(xb)) == NULL){
	clicon_err(OE_XML, 0, "No body found");
	goto done;
    }
    if ((cv = cv_new(type)) == NULL){
	clicon_err(OE_XML, errno, "cv_new");
	goto done;
    }
    if ((cvret = cv_parse1(bstr, cv, &reason)) < 0){
	clicon_err(OE_XML, errno, "cv_parse");
	goto done;
    }
    if (cvret == 0){  /* parsing failed */
	clicon_err(OE_XML, errno, "Parsing CV: %s", reason);
	if (reason)
	    free(reason);
    }
    *cvp = cv;
    retval = 0;
 done:
    if (retval < 0 && cv != NULL)
	cv_free(cv);
    return retval;
}

/*! Parse an xml body as int32
 * The real parsing functions are in the cligen code
 * @param[in]   xb          xml tree body node, ie containing a value to be parsed
 * @param[out]  val         Value after parsing
 * @retval      0           OK, parsed value in 'val'
 * @retval     -1           Error, one of: body not found, parse error, 
 *                          alloc error.
 * @note extend to all other cligen var types and generalize
 * @note use yang type info?
 * @note range check failure returns 0
 */
int
xml_body_int32(cxobj    *xb,
	       int32_t *val)
{
    cg_var *cv = NULL;

    if (xml_body_parse(xb, CGV_INT32, &cv) < 0)
	return -1;
    *val = cv_int32_get(cv);
    cv_free(cv);
    return 0;
}

/*! Parse an xml body as uint32
 * The real parsing functions are in the cligen code
 * @param[in]   xb          xml tree body node, ie containing a value to be parsed
 * @param[out]  val         Value after parsing
 * @retval      0           OK, parsed value in 'val'
 * @retval     -1           Error, one of: body not found, parse error, 
 *                          alloc error.
 * @note extend to all other cligen var types and generalize
 * @note use yang type info?
 * @note range check failure returns 0
 */
int
xml_body_uint32(cxobj    *xb,
		uint32_t *val)
{
    cg_var *cv = NULL;

    if (xml_body_parse(xb, CGV_UINT32, &cv) < 0)
	return -1;
    *val = cv_uint32_get(cv);
    cv_free(cv);
    return 0;
}

#endif /* NOTUSED */
