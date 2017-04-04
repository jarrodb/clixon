/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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

 *
 * XML code
 *
 *                        file          
 *                       +---------+    db2xml_key->       save_db_to_xml->
 *       +-------------> | database| <------------+------------------+
 *       |               +---------+  <-xml2db    | <-load_xml_to_db |
 *       |                                        |                  |
 *       |                                        |                  |
 *       v                                        v                  v
 *   +---------+     <-xml2cvec_key       +-----------+           +---------+
 *   |  cvec   |  <---------------------> | xml cxobj |<--------->| xmlfile |
 *   +---------+   cvec2xml->             +-----------+           +---------+
 *                 cvec2xml_1(yang)->  xml2json->|
 *                                     xml2txt-> |
 *                                     xml2cli-> v
 *                                        +---------+
 *                                        |  file   |
 *                                        +---------+
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <arpa/inet.h>
#include <assert.h>
#include <syslog.h>
#include <fcntl.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */

#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_chunk.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_yang_type.h"
#include "clixon_options.h"
#include "clixon_xml.h"
#include "clixon_xsl.h"
#include "clixon_log.h"
#include "clixon_err.h"
#include "clixon_xml_map.h"

/* Something to do with reverse engineering of junos syntax? */
#undef SPECIAL_TREATMENT_OF_NAME  




/*
 * A node is a leaf if it contains a body.
 */
static cxobj *
leaf(cxobj *xn)
{
    cxobj *xc = NULL;

    while ((xc = xml_child_each(xn, xc, CX_BODY)) != NULL)
	break;
    return xc;
}

/*! x is element and has eactly one child which in turn has none */
static int
tleaf(cxobj *x)
{
    cxobj *c;

    if (xml_type(x) != CX_ELMNT)
	return 0;
    if (xml_child_nr(x) != 1)
	return 0;
    c = xml_child_i(x, 0);
    return (xml_child_nr(c) == 0);
}

/*! Translate XML -> TEXT
 * @param[in]  level  print 4 spaces per level in front of each line
 */
int 
xml2txt(FILE *f, cxobj *x, int level)
{
    cxobj *xe = NULL;
    int    children=0;
    char  *term;
    int    retval = -1;
#ifdef SPECIAL_TREATMENT_OF_NAME  
    cxobj *xname;
#endif

    xe = NULL;     /* count children */
    while ((xe = xml_child_each(x, xe, -1)) != NULL)
	children++;
    if (!children){ 
	if (xml_type(x) == CX_BODY){
	    /* Kludge for escaping encrypted passwords */
	    if (strcmp(xml_name(xml_parent(x)), "encrypted-password")==0)
		term = chunk_sprintf(__FUNCTION__, "\"%s\"", xml_value(x));
	    else
		term = xml_value(x);
	}
	else{
	    fprintf(f, "%*s", 4*level, "");
	    term = xml_name(x);
	}
	fprintf(f, "%s;\n", term);
	retval = 0;
	goto done;
    }
    fprintf(f, "%*s", 4*level, "");

#ifdef SPECIAL_TREATMENT_OF_NAME  
    if (strcmp(xml_name(x), "name") != 0)
	fprintf(f, "%s ", xml_name(x));
    if ((xname = xml_find(x, "name")) != NULL){
	if (children > 1)
	    fprintf(f, "%s ", xml_body(xname));
	if (!tleaf(x))
	    fprintf(f, "{\n");
    }
    else
	if (!tleaf(x))
	    fprintf(f, "{\n");
#else
	fprintf(f, "%s ", xml_name(x));
	if (!tleaf(x))
	    fprintf(f, "{\n");
#endif /* SPECIAL_TREATMENT_OF_NAME */

    xe = NULL;
    while ((xe = xml_child_each(x, xe, -1)) != NULL){
#ifdef SPECIAL_TREATMENT_OF_NAME  
	if (xml_type(xe) == CX_ELMNT &&  (strcmp(xml_name(xe), "name")==0) && 
	    (children > 1)) /* skip if this is a name element (unless 0 children) */
	    continue;
#endif
	if (xml2txt(f, xe, level+1) < 0)
	    break;
    }
    if (!tleaf(x))
	fprintf(f, "%*s}\n", 4*level, "");
    retval = 0;
  done:
    return retval;
}

/*! Translate from XML to CLI commands
 * Howto: join strings and pass them down. 
 * Identify unique/index keywords for correct set syntax.
 * Args:
 *  @param[in] f        Where to print cli commands
 *  @param[in] x        XML Parse-tree (to translate)
 *  @param[in] prepend0 Print this text in front of all commands.
 *  @param[in] gt       option to steer cli syntax
 *  @param[in] label    Memory chunk allocation label
 */
int 
xml2cli(FILE              *f, 
	cxobj             *x, 
	char              *prepend0, 
	enum genmodel_type gt,
	const char        *label)
{
    int              retval = -1;
    cxobj           *xe = NULL;
    char            *term;
    char            *prepend;
    int              bool;
    int              nr;
    int              i;

    nr = xml_child_nr(x);
    if (!nr){
	if (xml_type(x) == CX_BODY)
	    term = xml_value(x);
	else
	    term = xml_name(x);
	if (prepend0)
	    fprintf(f, "%s ", prepend0);
	fprintf(f, "%s\n", term);
	retval = 0;
	goto done;
    }
    prepend = "";
    if (prepend0)
	if ((prepend = chunk_sprintf(label, "%s%s", prepend, prepend0)) == NULL)
	    goto done;
/* bool determines when to print a variable keyword:
   !leaf           T for all (ie parameter)
   index GT_NONE   F
   index GT_VARS   F
   index GT_ALL    T
   !index GT_NONE  F
   !index GT_VARS  T
   !index GT_ALL   T
 */
    bool = !leaf(x) || gt == GT_ALL || (gt == GT_VARS && !xml_index(x));
//    bool = (!x->xn_index || gt == GT_ALL);
    if (bool &&
	(prepend = chunk_sprintf(label, "%s%s%s", 
					prepend, 
					strlen(prepend)?" ":"",
				 xml_name(x))) == NULL)
	goto done;
    xe = NULL;
    /* First child is unique, then add that, before looping. */
    i = 0;
    while ((xe = xml_child_each(x, xe, -1)) != NULL){
	/* Dont call this if it is index and there are other following */
	if (xml_index(xe) && i < nr-1) 
	    ;
	else
	    if (xml2cli(f, xe, prepend, gt, label) < 0)
		goto done;
	if (xml_index(xe)){ /* assume index is first, otherwise need one more while */
	    if (gt ==GT_ALL && (prepend = chunk_sprintf(label, "%s %s", 
							prepend, 
							xml_name(xe))) == NULL)

		goto done;
	    if ((prepend = chunk_sprintf(label, "%s %s", 
					 prepend, 
					 xml_value(xml_child_i(xe, 0)))) == NULL)
		goto done;
	}
	i++;
    }
    retval = 0;
  done:
    return retval;
}


/*! Validate a single XML node with yang specification
 * -  If no value and mandatory flag set in spec, report error.
 * - Validate value versus spec, and report error if no match. Currently 
 *   only int ranges and string regexp checked.
 * @retval  0 OK
  */
int
xml_yang_validate(cxobj     *xt, 
		  yang_stmt *ys0)
{
    int        retval = -1;
    cg_var    *cv = NULL;
    char      *reason = NULL;
    yang_stmt *yc;
    int        i;
    yang_stmt *ys;
    char      *body;
    
    /* if not given by argument (overide) use default link */
    ys = ys0?ys0:xml_spec(xt);
    switch (ys->ys_keyword){
    case Y_LIST:
	/* fall thru */
    case Y_CONTAINER:
	for (i=0; i<ys->ys_len; i++){
	    yc = ys->ys_stmt[i];
	    if (yc->ys_keyword != Y_LEAF)
		continue;
	    if (yang_mandatory(yc) && xml_find(xt, yc->ys_argument)==NULL){
		clicon_err(OE_CFG, 0,"Missing mandatory variable: %s",
			   yc->ys_argument);
		goto done;
	    }
	}
	break;
    case Y_LEAF:
	/* fall thru */
    case Y_LEAF_LIST:
	/* validate value against ranges, etc */
	if ((cv = cv_dup(ys->ys_cv)) == NULL){
	    clicon_err(OE_UNIX, errno, "cv_dup");
	    goto done;
	}
	/* In the union case, value is parsed as generic REST type,
	 * needs to be reparsed when concrete type is selected
	 */
	if ((body = xml_body(xt)) != NULL){
	    if (cv_parse(body, cv) <0){
		clicon_err(OE_UNIX, errno, "cv_parse");
		goto done;
	    }
	    if ((ys_cv_validate(cv, ys, &reason)) != 1){
		clicon_err(OE_DB, 0,
			   "validation of %s failed %s",
			   ys->ys_argument, reason?reason:"");
		if (reason)
		    free(reason);
		goto done;
	    }
	}
	break;
    default:
	break;
    }
    retval = 0;
 done:
    if (cv)
	cv_free(cv);
    return retval;
}

/*! Translate a single xml node to a cligen variable vector. Note not recursive 
 * @param[in]  xt   XML tree containing one top node
 * @param[in]  ys   Yang spec containing type specification of top-node of xt
 * @param[out] cvv  CLIgen variable vector. Should be freed by cvec_free()
 * @retval     0    Everything OK, cvv allocated and set
 * @retval    -1    Something wrong, clicon_err() called to set error. No cvv returned
 * 'Not recursive' means that only one level of XML bodies is translated to cvec:s.
 * yang is needed to know which type an xml element has.
 * Example: 
    <a>
      <b>23</b>
      <c>88</c>
      <d>      
        <e>99</e>
      </d>
    </a> 
         --> b:23, c:88
 * @see cvec2xml
 */
int
xml2cvec(cxobj      *xt, 
	 yang_stmt  *yt, 
	 cvec      **cvv0)
{
    int               retval = -1;
    cvec             *cvv = NULL;
    cxobj            *xc;         /* xml iteration variable */
    yang_stmt        *ys;         /* yang spec */
    cg_var           *cv;
    cg_var           *ycv;
    char             *body;
    char             *reason = NULL;
    int               ret;
    int               i = 0;
    int               len = 0;
    char             *name;

    xc = NULL;
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL)
	len++;
    if ((cvv = cvec_new(len)) == NULL){
	clicon_err(OE_UNIX, errno, "cvec_new");
	goto err;
    }
    xc = NULL;
    /* Go through all children of the xml tree */
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL){
	name = xml_name(xc);
	if ((ys = yang_find_syntax((yang_node*)yt, name)) == NULL){
	    clicon_debug(0, "%s: yang sanity problem: %s in xml but not present in yang under %s",
			 __FUNCTION__, name, yt->ys_argument);
	    if ((body = xml_body(xc)) != NULL){
		cv = cvec_i(cvv, i++);
		cv_type_set(cv, CGV_STRING);
		cv_name_set(cv, name);
		if ((ret = cv_parse1(body, cv, &reason)) < 0){
		    clicon_err(OE_PLUGIN, errno, "cv_parse");
		    goto err;
		}
		if (ret == 0){
		    clicon_err(OE_PLUGIN, errno, "cv_parse: %s", reason);
		    if (reason)
			free(reason);
		    goto err;
		}
	    }
	}
	else
	if ((ycv = ys->ys_cv) != NULL){
	    if ((body = xml_body(xc)) != NULL){
		/* XXX: cvec_add uses realloc, can we avoid that? */
		cv = cvec_i(cvv, i++);
		if (cv_cp(cv, ycv) < 0){
		    clicon_err(OE_PLUGIN, errno, "cv_cp");
		    goto err;
		}
		if ((ret = cv_parse1(body, cv, &reason)) < 0){
		    clicon_err(OE_PLUGIN, errno, "cv_parse");
		    goto err;
		}
		if (ret == 0){
		    clicon_err(OE_PLUGIN, errno, "cv_parse: %s", reason);
		    if (reason)
			free(reason);
		    goto err;
		}
	    }
	}
    }
    if (debug > 1){
	clicon_debug(2, "%s cvv:\n", __FUNCTION__);
	cvec_print(stderr, cvv);
    }
    *cvv0 = cvv;
    return 0;
 err:
    if (cvv)
	cvec_free(cvv);
    return retval;
}

/*! Translate a cligen variable vector to an XML tree with depth one 
 * @param[in]   cvv  CLIgen variable vector. Should be freed by cvec_free()
 * @param[in]   toptag    The XML tree in xt will have this XML tag
 * @param[in]   xt   Parent, or NULL
 * @param[out]  xt   Pointer to XML tree containing one top node. Should be freed with xml_free
 * @retval      0    Everything OK, cvv allocated and set
 * @retval     -1    Something wrong, clicon_err() called to set error. No xt returned
 * @see xml2cvec
 * @see cvec2xml   This does more but has an internal xml2cvec translation
*/
int
cvec2xml_1(cvec   *cvv, 
	   char   *toptag, 
	   cxobj  *xp,
	   cxobj **xt0)
{
    int               retval = -1;
    cxobj            *xt = NULL;
    cxobj            *xn;
    cxobj            *xb;
    cg_var           *cv;
    char             *val;
    int               len=0;
    int               i;

    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL) 
	len++;
    if ((xt = xml_new(toptag, xp)) == NULL)
	goto err;
    if (xml_childvec_set(xt, len) < 0)
	goto err;
    cv = NULL;
    i = 0;
    while ((cv = cvec_each(cvv, cv)) != NULL) {
	if ((xn = xml_new(cv_name_get(cv), NULL)) == NULL) /* this leaks */
	    goto err;
	xml_parent_set(xn, xt);
	xml_child_i_set(xt, i++, xn);
	if ((xb = xml_new("body", xn)) == NULL) /* this leaks */
	    goto err;
	xml_type_set(xb, CX_BODY);
	val = cv2str_dup(cv);
	xml_value_set(xb, val); /* this leaks */
	if (val)
	    free(val);
    }
    *xt0 = xt;
    return 0;
 err:
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Return 1 if value is a body of one of the named children of xt */
static int
xml_is_body(cxobj *xt, 
	    char  *name, 
	    char  *val)
{
    cxobj *x;

    x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	if (strcmp(name, xml_name(x)))
	    continue;
	if (strcmp(xml_body(x), val) == 0)
	    return 1;
    }
    return 0;
}

/*! Recursive help function to compute differences between two xml trees
 * @see dbdiff_vector.
 */
static int
xml_diff1(yang_stmt *ys, 
	  cxobj     *xt1, 
	  cxobj     *xt2,
	  cxobj   ***first,
	  size_t    *firstlen,
	  cxobj   ***second,
	  size_t    *secondlen,
	  cxobj   ***changed1,
	  cxobj   ***changed2,
	  size_t    *changedlen)
{
    int        retval = -1;
    cxobj     *x1 = NULL;
    cxobj     *x2 = NULL;
    yang_stmt *y;
    yang_stmt *ykey;
    char      *name;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    char      *keyname;
    int        equal;
    char      *body1;
    char      *body2;

    clicon_debug(2, "%s: %s", __FUNCTION__, ys->ys_argument?ys->ys_argument:"yspec");
    /* Check nodes present in xt1 and xt2 + nodes only in xt1
     * Loop over xt1
     */
    x1 = NULL;
    while ((x1 = xml_child_each(xt1, x1, CX_ELMNT)) != NULL){
	name = xml_name(x1);
	if (ys->ys_keyword == Y_SPEC)
	    y = yang_find_topnode((yang_spec*)ys, name);
	else
	    y = yang_find_syntax((yang_node*)ys, name);
	if (y == NULL){
	    clicon_err(OE_UNIX, errno, "No yang node found: %s", name);
	    goto done;
	}
	switch (y->ys_keyword){
	case Y_LIST:
	    if ((ykey = yang_find((yang_node*)y, Y_KEY, NULL)) == NULL){
		clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
			   __FUNCTION__, y->ys_argument);
		goto done;
	    }
	    /* The value is a list of keys: <key>[ <key>]*  */
	    if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
		goto done;
	    /* Iterate over xt2 tree to (1) find a child that matches name
	       (2) that have keys that matches */
	    equal = 0;
	    x2 = NULL;
	    while ((x2 = xml_child_each(xt2, x2, CX_ELMNT)) != NULL){
		if (strcmp(xml_name(x2), name))
		    continue;
		cvi = NULL;
		equal = 0;
		/* (2) Match keys between x1 and x2 */
		while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		    keyname = cv_string_get(cvi);
		    if ((body1 = xml_find_body(x1, keyname)) == NULL)
			continue; /* may be error */
		    if ((body2 = xml_find_body(x2, keyname)) == NULL)
			continue; /* may be error */
		    if (strcmp(body1, body2)==0)
			equal=1;
		    else{
			equal=0; /* stop as soon as inequal key found */
			break;
		    }
		}
		if (equal) /* found x1 and x2 equal, otherwise look 
			      for other x2 */
		    break;
	    }
	    if (cvk){
		cvec_free(cvk);
		cvk = NULL;
	    }
	    if (equal){ 
		if (xml_diff1(y, x1, x2,   
			      first, firstlen, 
			      second, secondlen, 
			      changed1, changed2, changedlen)< 0)
		    goto done;
		break;
	    }
	    else
		if (cxvec_append(x1, first, firstlen) < 0) 
		    goto done;

	    break;
	case Y_CONTAINER:
	    /* Equal regardless */
	    if ((x2 = xml_find(xt2, name)) == NULL){
		if (cxvec_append(x1, first, firstlen) < 0) 
		    goto done;
		break;
	    }
	    if (xml_diff1(y, x1, x2,   
			  first, firstlen, 
			  second, secondlen, 
			  changed1, changed2, changedlen)< 0)
		goto done;
	    break;
	case Y_LEAF:
	    if ((x2 = xml_find(xt2, name)) == NULL){
		if (cxvec_append(x1, first, firstlen) < 0) 
		    goto done;
		break;
	    }
	    if (strcmp(xml_body(x1), xml_body(x2))){
		if (cxvec_append(x1, changed1, changedlen) < 0) 
		    goto done;
		(*changedlen)--; /* append two vectors */
		if (cxvec_append(x2, changed2, changedlen) < 0) 
		    goto done;
	    }
	    break;
	case Y_LEAF_LIST:
	    body1 = xml_body(x1);
	    if (!xml_is_body(xt2, name, body1)) /* where body is */
		if (cxvec_append(x1, first, firstlen) < 0) 
		    goto done;
	    break;
	default:
	    break;
	}
    } /* while xt1 */
    /* Check nodes present only in xt2
     * Loop over xt2
     */
    x2 = NULL;
    while ((x2 = xml_child_each(xt2, x2, CX_ELMNT)) != NULL){
	name = xml_name(x2);
	if (ys->ys_keyword == Y_SPEC)
	    y = yang_find_topnode((yang_spec*)ys, name);
	else
	    y = yang_find_syntax((yang_node*)ys, name);
	if (y == NULL){
	    clicon_err(OE_UNIX, errno, "No yang node found: %s", name);
	    goto done;
	}
	switch (y->ys_keyword){
	case Y_LIST:
	    if ((ykey = yang_find((yang_node*)y, Y_KEY, NULL)) == NULL){
		clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
			   __FUNCTION__, y->ys_argument);
		goto done;
	    }
	    /* The value is a list of keys: <key>[ <key>]*  */
	    if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
		goto done;
	    /* Iterate over xt1 tree to (1) find a child that matches name
	       (2) that have keys that matches */
	    equal = 0;
	    x1 = NULL;
	    while ((x1 = xml_child_each(xt1, x1, CX_ELMNT)) != NULL){
		if (strcmp(xml_name(x1), name))
		    continue;
		cvi = NULL;
		equal = 0;
		/* (2) Match keys between x2 and x1 */
		while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		    keyname = cv_string_get(cvi);
		    if ((body2 = xml_find_body(x2, keyname)) == NULL)
			continue; /* may be error */
		    if ((body1 = xml_find_body(x1, keyname)) == NULL)
			continue; /* may be error */
		    if (strcmp(body2, body1)==0)
			equal=1;
		    else{
			equal=0; /* stop as soon as inequal key found */
			break;
		    }
		}
		if (equal) /* found x1 and x2 equal, otherwise look 
			      for other x2 */
		    break;
	    }
	    if (cvk){
		cvec_free(cvk);
		cvk = NULL;
	    }
	    if (!equal)
		if (cxvec_append(x2, second, secondlen) < 0) 
		    goto done;
	    break;
	case Y_CONTAINER:
	    /* Equal regardless */
	    if ((x1 = xml_find(xt1, name)) == NULL)
		if (cxvec_append(x2, second, secondlen) < 0) 
		    goto done;
	    break;
	case Y_LEAF:
	    if ((x1 = xml_find(xt1, name)) == NULL)
		if (cxvec_append(x2, second, secondlen) < 0) 
		    goto done;
	    break;
	case Y_LEAF_LIST:
	    body2 = xml_body(x2);
	    if (!xml_is_body(xt1, name, body2)) /* where body is */
		if (cxvec_append(x2, second, secondlen) < 0) 
		    goto done;
	    break;
	default:
	    break;
	}
    } /* while xt1 */
    retval = 0;
 done:
    if (cvk)
	cvec_free(cvk);
    return retval;
}

/*! Compute differences between two xml trees
 * @param[in]  yspec     Yang specification
 * @param[in]  xt1       First XML tree
 * @param[in]  xt2       Second XML tree
 * @param[out] first     Pointervector to XML nodes existing in only first tree
 * @param[out] firstlen  Length of first vector
 * @param[out] second    Pointervector to XML nodes existing in only second tree
 * @param[out] secondlen Length of second vector
 * @param[out] changed1  Pointervector to XML nodes changed value
 * @param[out] changed2  Pointervector to XML nodes changed value
 * @param[out] changedlen Length of changed vector
 * All xml vectors should be freed after use.
 * Bot xml trees should be freed with xml_free()
 */
int
xml_diff(yang_spec *yspec, 
	 cxobj     *xt1, 
	 cxobj     *xt2,
	 cxobj   ***first,
	 size_t    *firstlen,
	 cxobj   ***second,
	 size_t    *secondlen,
	 cxobj   ***changed1,
	 cxobj   ***changed2,
	 size_t    *changedlen)
{
    int retval = -1;

    *firstlen = 0;
    *secondlen = 0;    
    *changedlen = 0;
    if (xt1 == NULL && xt2 == NULL)
	return 0;
    if (xt2 == NULL){
	if (cxvec_append(xt1, first, firstlen) < 0) 
	    goto done;
	goto ok;
    }
    if (xt1 == NULL){
	if (cxvec_append(xt1, second, secondlen) < 0) 
	    goto done;
	goto ok;
    }
    if (xml_diff1((yang_stmt*)yspec, xt1, xt2,
		  first, firstlen, 
		  second, secondlen, 
		  changed1, changed2, changedlen) < 0)
	goto done;
 ok:
    retval = 0;
 done:
    return retval;
}
