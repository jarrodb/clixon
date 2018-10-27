/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <pwd.h>
#include <netinet/in.h>
#include <libgen.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_netconf.h"
#include "netconf_lib.h"
#include "netconf_hello.h"
#include "netconf_rpc.h"

/* Command line options to be passed to getopt(3) */
#define NETCONF_OPTS "hD:f:l:qa:u:d:y:U:t:"

#define NETCONF_LOGFILE "/tmp/clixon_netconf.log"

/*! Process incoming packet 
 * @param[in]   h    Clicon handle
 * @param[in]   cb   Packet buffer
 */
static int
process_incoming_packet(clicon_handle h, 
			cbuf         *cb)
{
    char  *str;
    char  *str0;
    cxobj *xreq = NULL; /* Request (in) */
    int    isrpc = 0;   /* either hello or rpc */
    cbuf  *cbret = NULL;
    cxobj *xret = NULL; /* Return (out) */
    cxobj *xrpc;
    cxobj *xc;
    yang_spec *yspec;

    clicon_debug(1, "RECV");
    clicon_debug(2, "%s: RCV: \"%s\"", __FUNCTION__, cbuf_get(cb));
    yspec = clicon_dbspec_yang(h);
    if ((str0 = strdup(cbuf_get(cb))) == NULL){
	clicon_log(LOG_ERR, "%s: strdup: %s", __FUNCTION__, strerror(errno));
	return -1;
    }
    str = str0;
    /* Parse incoming XML message */
    if (xml_parse_string(str, yspec, &xreq) < 0){ 
	if ((cbret = cbuf_new()) == NULL){
	    if (netconf_operation_failed(cbret, "rpc", "internal error")< 0)
		goto done;
	    netconf_output(1, cbret, "rpc-error");
	}
	else
	    clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	free(str0);
	goto done;
    }
    free(str0);
    if ((xrpc=xpath_first(xreq, "//rpc")) != NULL)
        isrpc++;
    else
        if (xpath_first(xreq, "//hello") != NULL)
	    ;
        else{
            clicon_log(LOG_WARNING, "Invalid netconf msg: neither rpc or hello: dropped");
            goto done;
        }
    if (!isrpc){ /* hello */
	if (netconf_hello_dispatch(xreq) < 0)
	    goto done;
    }
    else  /* rpc */
	if (netconf_rpc_dispatch(h, xrpc, &xret) < 0){
	    goto done;
	}
	else{ /* there is a return message in xret */
	    cxobj *xa, *xa2;
	    assert(xret);

	    if ((cbret = cbuf_new()) != NULL){
		if ((xc = xml_child_i(xret,0))!=NULL){
		    xa=NULL;
		    /* Copy message-id attribute from incoming to reply. 
		     * RFC 6241:
		     * If additional attributes are present in an <rpc> element, a NETCONF
		     * peer MUST return them unmodified in the <rpc-reply> element.  This
		     * includes any "xmlns" attributes.
		     */
		    while ((xa = xml_child_each(xrpc, xa, CX_ATTR)) != NULL){
			if ((xa2 = xml_dup(xa)) ==NULL)
			    goto done;
			if (xml_addsub(xc, xa2) < 0)
			    goto done;
		    }
		    add_preamble(cbret);

		    clicon_xml2cbuf(cbret, xml_child_i(xret,0), 0, 0);
		    add_postamble(cbret);
		    if (netconf_output(1, cbret, "rpc-reply") < 0){
			cbuf_free(cbret);
			goto done;
		    }
		}
	    }
	}
  done:
    if (xreq)
	xml_free(xreq);
    if (xret)
	xml_free(xret);
    if (cbret)
	cbuf_free(cbret);
    return 0;
}

/*! Get netconf message: detect end-of-msg 
 * @param[in]   s    Socket where input arrived. read from this.
 * @param[in]   arg  Clicon handle.
 * This routine continuously reads until no more data on s. There could
 * be risk of starvation, but the netconf client does little else than
 * read data so I do not see a danger of true starvation here.
 */
static int
netconf_input_cb(int   s, 
		 void *arg)
{
    int           retval = -1;
    clicon_handle h = arg;
    unsigned char buf[BUFSIZ];
    int           i;
    int           len;
    cbuf         *cb=NULL;
    int           xml_state = 0;
    int           poll;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	return retval;
    }
    memset(buf, 0, sizeof(buf));
    while (1){
	if ((len = read(s, buf, sizeof(buf))) < 0){
	    if (errno == ECONNRESET)
		len = 0; /* emulate EOF */
	    else{
		clicon_log(LOG_ERR, "%s: read: %s", __FUNCTION__, strerror(errno));
		goto done;
	    }
	} /* read */
	if (len == 0){ 	/* EOF */
	    cc_closed++;
	    close(s);
	    retval = 0;
	    goto done;
	}
	for (i=0; i<len; i++){
	    if (buf[i] == 0)
		continue; /* Skip NULL chars (eg from terminals) */
	    cprintf(cb, "%c", buf[i]);
	    if (detect_endtag("]]>]]>",
			      buf[i],
			      &xml_state)) {
		/* OK, we have an xml string from a client */
		/* Remove trailer */
		*(((char*)cbuf_get(cb)) + cbuf_len(cb) - strlen("]]>]]>")) = '\0';
		if (process_incoming_packet(h, cb) < 0)
		    goto done;
		if (cc_closed)
		    break;
		cbuf_reset(cb);
	    }
	}
	/* poll==1 if more, poll==0 if none */
	if ((poll = event_poll(s)) < 0)
	    goto done;
	if (poll == 0)
	    break; /* No data to read */
    } /* while */
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (cc_closed) 
	retval = -1;
    return retval;
}

/*! Send netconf hello message
 * @param[in]   h   Clicon handle
 * @param[in]   s   File descriptor to write on (eg 1 - stdout)
 */
static int
send_hello(clicon_handle h,
	   int           s)
{
    int   retval = -1;
    cbuf *cb;
    
    if ((cb = cbuf_new()) == NULL){
	clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	goto done;
    }
    if (netconf_create_hello(h, cb, getpid()) < 0)
	goto done;
    if (netconf_output(s, cb, "hello") < 0)
	goto done;
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

static int
netconf_terminate(clicon_handle h)
{
    yang_spec  *yspec;
    cxobj      *x;
    
    clixon_plugin_exit(h);
    rpc_callback_delete_all();
    clicon_rpc_close_session(h);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	yspec_free(yspec);
    if ((yspec = clicon_config_yang(h)) != NULL)
	yspec_free(yspec);
    if ((x = clicon_conf_xml(h)) != NULL)
	xml_free(x);
    event_exit();
    clicon_handle_exit(h);
    clicon_log_exit();
    return 0;
}

static int
timeout_fn(int s,
	   void *arg)
{
    clicon_err(OE_EVENTS, ETIME, "User request timeout");
    return -1; 
}

/*! Usage help routine
 * @param[in]  h      Clicon handle
 * @param[in]  argv0  command line
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
	    "\t-D <level>\tDebug level\n"
            "\t-q\t\tQuiet: dont send hello prompt\n"
    	    "\t-f <file>\tConfiguration file (mandatory)\n"
	    "\t-l (e|o|s|f<file>) \tLog on std(e)rr, std(o)ut, (s)yslog, (f)ile (syslog is default)\n"
    	    "\t-a UNIX|IPv4|IPv6\tInternal backend socket family\n"
    	    "\t-u <path|addr>\tInternal socket domain path or IP addr (see -a)\n"
	    "\t-d <dir>\tSpecify netconf plugin directory dir (default: %s)\n"

	    "\t-y <file>\tLoad yang spec file (override yang main module)\n"
	    "\t-U <user>\tOver-ride unix user with a pseudo user for NACM.\n"
	    "\t-t <sec>\tTimeout in seconds. Quit after this time.\n",
	    argv0,
	    clicon_netconf_dir(h)
	    );
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    char             c;
    char            *tmp;
    char            *argv0 = argv[0];
    int              quiet = 0;
    clicon_handle    h;
    char            *dir;
    int              logdst = CLICON_LOG_STDERR;
    struct passwd   *pw;
    struct timeval   tv = {0,}; /* timeout */
    yang_spec       *yspec = NULL;
    yang_spec       *yspecfg = NULL; /* For config XXX clixon bug */
    char            *yang_filename = NULL;
    
    /* Create handle */
    if ((h = clicon_handle_init()) == NULL)
	return -1;
    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Set username to clicon handle. Use in all communication to backend */
    if ((pw = getpwuid(getuid())) == NULL){
	clicon_err(OE_UNIX, errno, "getpwuid");
	goto done;
    }
    if (clicon_username_set(h, pw->pw_name) < 0)
	goto done;
    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	    usage(h, argv[0]);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &debug) != 1)
		usage(h, argv[0]);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	     break;
	}

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, logdst); 
    clicon_debug_init(debug, NULL); 

    /* Create configure yang-spec */
    if ((yspecfg = yspec_new()) == NULL)
	goto done;
    /* Find and read configfile */
    if (clicon_options_main(h, yspecfg) < 0)
	return -1;
    clicon_config_yang_set(h, yspecfg);
    /* Now rest of options */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'l':  /* log  */
	    break; /* see above */
	case 'a': /* internal backend socket address family */
	    clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
	    break;
	case 'u': /* internal backend socket unix domain path or ip host */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_SOCK", optarg);
	    break;
	case 'q':  /* quiet: dont write hello */
	    quiet++;
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_NETCONF_DIR", optarg);
	    break;
	case 'y' :{ /* Load yang spec file (override yang main module) */
	    yang_filename = optarg;
	    break;
	}
	case 'U': /* Clixon 'pseudo' user */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_username_set(h, optarg) < 0)
		goto done;
	    break;
	case 't': /* timeout in seconds */
	    tv.tv_sec = atoi(optarg);
	    break;

	default:
	    usage(h, argv[0]);
	    break;
	}
    argc -= optind;
    argv += optind;

    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);	
    /* Load main application yang specification either module or specific file
     * If -y <file> is given, it overrides main module */
    if (yang_filename){
	if (yang_spec_parse_file(h, yang_filename, clicon_yang_dir(h), yspec, NULL) < 0)
	    goto done;
    }
    else if (yang_spec_parse_module(h, clicon_yang_module_main(h),
				    clicon_yang_dir(h),
				    clicon_yang_module_revision(h),
				    yspec, NULL) < 0)
	goto done;
    
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;
    /* Add netconf yang spec, used by netconf client and as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    /* Initialize plugins group */
    if ((dir = clicon_netconf_dir(h)) != NULL)
	if (clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
	    goto done;

    /* Call start function is all plugins before we go interactive */
    tmp = *(argv-1);
    *(argv-1) = argv0;
    clixon_plugin_start(h, argc+1, argv-1);
    *(argv-1) = tmp;

    if (!quiet)
	send_hello(h, 1);
    if (event_reg_fd(0, netconf_input_cb, h, "netconf socket") < 0)
	goto done;
    if (debug)
	clicon_option_dump(h, debug);
    if (tv.tv_sec || tv.tv_usec){
	struct timeval t;
	gettimeofday(&t, NULL);
	timeradd(&t, &tv, &t);
	if (event_reg_timeout(t, timeout_fn, NULL, "timeout") < 0)
	    goto done;
    }
    if (event_loop() < 0)
	goto done;
  done:
    netconf_terminate(h);
    clicon_log_init(__PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clicon_log(LOG_NOTICE, "%s: %u Terminated\n", __PROGRAM__, getpid());
    return 0;
}
