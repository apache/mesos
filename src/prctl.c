/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident   "@(#)prctl.c    1.18    05/06/08 SMI"

#include <unistd.h>
#include <rctl.h>
#include <libproc.h>
#include <stdio.h>
#include <libintl.h>
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <strings.h>
#include <ctype.h>
#include <project.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/varargs.h>
#include <priv.h>
#include <zone.h>
#include "utils.h"

/* Valid user actions */
#define ACTION_DISABLE          0x01
#define ACTION_ENABLE           0x02
#define ACTION_SET              0x04
#define ACTION_REPLACE          0x08
#define ACTION_DELETE           0x10

#define PRCTL_VALUE_WIDTH       4

/* Maximum string length for deferred errors */
#define GLOBAL_ERR_SZ           1024

/* allow important process values to be passed together easily */
typedef struct pr_info_handle {
  struct ps_prochandle *pr;
  pid_t pid;
  psinfo_t psinfo;
  taskid_t taskid;
  projid_t projid;
  char *projname;
  zoneid_t zoneid;
  char *zonename;

} pr_info_handle_t;

/* Structures for list of resource controls */
typedef struct prctl_value {
  rctlblk_t *rblk;
  struct prctl_value *next;
} prctl_value_t;

typedef struct prctl_list {
  char *name;
  prctl_value_t *val_list;
  struct prctl_list *next;
} prctl_list_t;

static  volatile int    interrupt;

static  prctl_list_t    *global_rctl_list_head = NULL;
static  prctl_list_t    *global_rctl_list_tail = NULL;
static  char            global_error[GLOBAL_ERR_SZ];

/* global variables that contain commmand line option info */
static  int     arg_operation = 0;
static  int     arg_force = 0;


/* String and type from -i */
static  rctl_entity_t   arg_entity_type = RCENTITY_PROCESS;
static  char    *arg_entity_string = NULL;

/* -n argument */
static  char    *arg_name = NULL;

static  rctl_entity_t   arg_name_entity = 0;

/* -t argument value */
static  int     arg_priv = 0;

/* -v argument string */
static  char    *arg_valuestring = NULL;

/* global flags of rctl name passed to -n */
static  int     arg_global_flags = 0;
static  rctl_qty_t      arg_global_max;

/* appropriate scaling variables determined by rctl unit type */
scale_t         *arg_scale;
static  char    *arg_unit = NULL;

/* -v argument string converted to uint64_t */
static  uint64_t arg_value = 0;

/* if -v argument is scaled value, points to "K", "M", "G", ... */
static  char    *arg_modifier = NULL;

/* -e/-d argument string */
static  char    *arg_action_string = NULL;

/* Set to RCTL_LOCAL_SIGNAL|DENY based on arg_action_string */
static  int     arg_action = 0;

/* if -e/-d arg is signal=XXX, set to signal number of XXX */
static  int     arg_signal = 0;

/* -p arg if -p is specified */
static  int     arg_pid = -1;
static  char    *arg_pid_string = NULL;

/* Set to 1 if -P is specified */
static  int     arg_parseable_mode = 0;

/* interupt handler */
static  void    intr(int);

static  int     get_rctls(struct ps_prochandle *);
static  int     store_rctls(const char *rctlname, void *walk_data);
static  prctl_value_t   *store_value_entry(rctlblk_t *rblk, prctl_list_t *list);
static  prctl_list_t    *store_list_entry(const char *name);
static  void    free_lists();

static  int     change_action(rctlblk_t *blk);

static  int     prctl_setrctl(struct ps_prochandle *Pr, const char *name,
			      rctlblk_t *, rctlblk_t *, uint_t);

static  int match_rctl(struct ps_prochandle *Pr, rctlblk_t **rctl, char *name,
		       char *valuestringin, int valuein, rctl_priv_t privin,
		       int pidin);
static  int     match_rctl_blk(rctlblk_t *rctl, char *valuestringin,
			       uint64_t valuein,
			       rctl_priv_t privin, int pidin);
static  pid_t   regrab_process(pid_t pid, pr_info_handle_t *p, int, int *gret);
static  pid_t   grab_process_by_id(char *idname, rctl_entity_t type,
				   pr_info_handle_t *p, int, int *gret);
static  int     grab_process(pr_info_handle_t *p, int *gret);
static  void    release_process(struct ps_prochandle *Pr);
static  void    preserve_error(char *format, ...);

static  void    print_rctls(pr_info_handle_t *p);
static  void    print_priv(rctl_priv_t local_priv, char *format);
static  void    print_local_action(int action, int *signalp, char *format);

static const char USAGE[] = ""
"usage:\n"
"    Report resource control values and actions:\n"
"       prctl [-P] [-t [basic | privileged | system]\n"
"       [-n name] [-i process | task | project | zone] id ...\n"
"       -P space delimited output\n"
"       -t privilege level of rctl values to get\n"
"       -n name of resource control values to get\n"
"       -i idtype of operand list\n"
"    Manipulate resource control values:\n"
"       prctl [-t [basic | privileged | system]\n"
"       -n name [-srx] [-v value] [-p pid ] [-e | -d action]\n"
"       [-i process | task | project | zone] id ...\n"
"       -t privilege level of rctl value to set/replace/delete/modify\n"
"       -n name of resource control to set/replace/delete/modify\n"
"       -s set new resource control value\n"
"       -r replace first rctl value of matching privilege\n"
"       -x delete first rctl value of matching privilege, value, and \n"
"          recipient pid\n"
"       -v value of rctl to set/replace/delete/modify\n"
"       -p recipient pid of rctl to set/replace/delete/modify\n"
"       -e enable action of first rctl value of matching privilege,\n"
"          value, and recipient pid\n"
"       -d disable action of first rctl value of matching privilege,\n"
"          value, and recipient pid\n"
  "       -i idtype of operand list\n";


static void
usage()
{
  (void) fprintf(stderr, gettext(USAGE));
  exit(2);
}

int
main(int argc, char **argv)
{
  int flags;
  int opt, errflg = 0;
  rctlblk_t *rctlblkA = NULL;
  rctlblk_t *rctlblkB = NULL;
  rctlblk_t *tmp = NULL;
  pid_t pid;
  char *target_id;
  int search_type;
  int signal;
  int localaction;
  int printed = 0;
  int gret;
  char *end;

  (void) setlocale(LC_ALL, "");
  (void) textdomain(TEXT_DOMAIN);
  (void) setprogname(argv[0]);

  while ((opt = getopt(argc, argv, "sPp:Fd:e:i:n:rt:v:x")) != EOF) {

    switch (opt) {
    case 'F':       /* force grabbing (no O_EXCL) */
      arg_force = PGRAB_FORCE;
      break;
    case 'i':       /* id type for arguments */
      arg_entity_string = optarg;
      if (strcmp(optarg, "process") == 0 ||
	  strcmp(optarg, "pid") == 0)
	arg_entity_type = RCENTITY_PROCESS;
      else if (strcmp(optarg, "project") == 0 ||
	       strcmp(optarg, "projid") == 0)
	arg_entity_type = RCENTITY_PROJECT;
      else if (strcmp(optarg, "task") == 0 ||
	       strcmp(optarg, "taskid") == 0)
	arg_entity_type = RCENTITY_TASK;
      else if (strcmp(optarg, "zone") == 0 ||
	       strcmp(optarg, "zoneid") == 0)
	arg_entity_type = RCENTITY_ZONE;
      else {
	warn(gettext("unknown idtype %s"), optarg);
	errflg = 1;
      }
      break;
    case 'd':
      arg_action_string = optarg;
      arg_operation |= ACTION_DISABLE;
      break;
    case 'e':
      arg_action_string = optarg;
      arg_operation |= ACTION_ENABLE;
      break;
    case 'n':       /* name of rctl */
      arg_name = optarg;
      if (strncmp(optarg, "process.",
		  strlen("process.")) == 0)
	arg_name_entity = RCENTITY_PROCESS;
      else if (strncmp(optarg, "project.",
		       strlen("project.")) == 0)
	arg_name_entity = RCENTITY_PROJECT;
      else if (strncmp(optarg, "task.",
		       strlen("task.")) == 0)
	arg_name_entity = RCENTITY_TASK;
      else if (strncmp(optarg, "zone.",
		       strlen("zone.")) == 0)
	arg_name_entity = RCENTITY_ZONE;
      break;
    case 'r':
      arg_operation |= ACTION_REPLACE;
      break;
    case 't':       /* rctl type */
      if (strcmp(optarg, "basic") == 0)
	arg_priv = RCPRIV_BASIC;
      else if (strcmp(optarg, "privileged") == 0)
	arg_priv = RCPRIV_PRIVILEGED;
      else if (strcmp(optarg, "priv") == 0)
	arg_priv = RCPRIV_PRIVILEGED;
      else if (strcmp(optarg, "system") == 0)
	arg_priv = RCPRIV_SYSTEM;
      else {
	warn(gettext("unknown privilege %s"), optarg);
	errflg = 1;
      }
      break;
    case 'v':       /* value */
      arg_valuestring = optarg;
      break;
    case 's':
      arg_operation |= ACTION_SET;
      break;
    case 'x':       /* delete */
      arg_operation |= ACTION_DELETE;
      break;
    case 'p':
      errno = 0;
      /* Stick with -1 if arg is "-" */
      if (strcmp("-", optarg) == 0)
	break;

      arg_pid_string = optarg;
      arg_pid = strtoul(optarg, &end, 10);
      if (errno || *end != '\0' || end == optarg) {
	warn(gettext("invalid pid %s"), optarg);
	errflg = 1;
	break;
      }
      break;
    case 'P':
      arg_parseable_mode = 1;
      break;
    default:
      warn(gettext("unknown option"));
      errflg = 1;
      break;
    }
  }
  argc -= optind;
  argv += optind;

  if (argc < 1) {
    warn(gettext("no arguments specified"));
    errflg = 1;
    goto done_parse;
  }
  /* if -v is specified without -r, -x, -d, or -e, -s is implied */
  if (arg_valuestring &&
      (!(arg_operation & (ACTION_REPLACE | ACTION_DELETE |
			  ACTION_DISABLE | ACTION_ENABLE)))) {
    arg_operation |= ACTION_SET;
  }
  /* operations require -n */
  if (arg_operation && (arg_name == NULL)) {
    warn(gettext("-n is required with -s, -r, -x, -e, or -d"));
    errflg = 1;
    goto done_parse;
  }
  /* enable and disable are exclusive */
  if ((arg_operation & ACTION_ENABLE) &&
      (arg_operation & ACTION_DISABLE)) {
    warn(gettext("options -d and -e are exclusive"));
    errflg = 1;
    goto done_parse;
  }
  /* -s, -r, and -x are exclusive */
        flags = arg_operation &
	  (ACTION_REPLACE | ACTION_SET | ACTION_DELETE);
        if (flags & (flags - 1)) {
	  warn(gettext("options -s, -r, and -x are exclusive"));
	  errflg = 1;
	  goto done_parse;
        }
        /* -e or -d makes no sense with -x */
        if ((arg_operation & ACTION_DELETE) &
            (arg_operation & (ACTION_ENABLE | ACTION_DISABLE))) {
	  warn(gettext("options -e or -d  not allowed with -x"));
	  errflg = 1;
	  goto done_parse;
        }
        /* if -r is specified -v must be as well */
        if ((arg_operation & ACTION_REPLACE) && (!arg_valuestring)) {
	  warn(gettext("option -r requires use of option -v"));
	  errflg = 1;
	  goto done_parse;
        }
        /* if -s is specified -v must be as well */
        if ((arg_operation & ACTION_SET) && (!arg_valuestring)) {
	  warn(gettext("option -s requires use of option -v"));
	  errflg = 1;
	  goto done_parse;
        }
        /* Specifying a recipient pid on a non-basic rctl makes no sense */
        if (arg_pid != -1 && arg_priv > RCPRIV_BASIC) {
	  warn(gettext("option -p not allowed on non-basic rctl"));
	  errflg = 1;
	  goto done_parse;
        }
        /* Specifying a recipient pid on a privileged rctl makes no sense */
        if (arg_pid != -1 &&
            arg_priv == RCPRIV_PRIVILEGED) {
	  warn(gettext("option -p not allowed with privileged rctl"));
	  errflg = 1;
	  goto done_parse;
        }
        if (arg_operation) {

	  /* do additional checks if there is an operation */

	  if (arg_parseable_mode == 1) {
	    warn(gettext("-P not valid when manipulating "
			 "resource control values"));
	    errflg = 1;
	    goto done_parse;
	  }
	  /* get rctl global flags to determine if actions are valid */
	  if ((rctlblkA = calloc(1, rctlblk_size())) == NULL) {
	    warn(gettext("malloc failed: %s"),
		 strerror(errno));
	    errflg = 1;
	    goto done_parse;
	  }
	  if ((rctlblkB = calloc(1, rctlblk_size())) == NULL) {
	    warn(gettext("malloc failed: %s"),
		 strerror(errno));
	    errflg = 1;
	    goto done_parse;
	  }
	  /* get system rctl to get global flags and max value */
	  if (getrctl(arg_name, NULL, rctlblkA, RCTL_FIRST)) {
	    warn(gettext("failed to get resource control "
			 "for %s: %s"), arg_name, strerror(errno));
	    errflg = 1;
	    goto done_parse;
	  }
	  while (getrctl(arg_name, rctlblkA, rctlblkB, RCTL_NEXT) == 0) {

	    /* allow user interrupt */
	    if (interrupt) {
	      errflg = 1;
	      goto done_parse;
	    }
	    tmp = rctlblkB;
	    rctlblkB = rctlblkA;
	    rctlblkA = tmp;

	    if (rctlblk_get_privilege(rctlblkA) ==
		RCPRIV_SYSTEM) {
	      break;
	    }
	  }
	  if (rctlblk_get_privilege(rctlblkA) != RCPRIV_SYSTEM) {
	    warn(gettext("failed to get system resource control "
			 "for %s: %s"), arg_name, strerror(errno));
	    errflg = 1;
	    goto done_parse;
	  }
	  /* figure out the correct scale and unit for this rctl */
	  arg_global_flags = rctlblk_get_global_flags(rctlblkA);
	  arg_global_max = rctlblk_get_value(rctlblkA);

	  if (arg_global_flags & RCTL_GLOBAL_BYTES) {
	    arg_unit = SCALED_UNIT_BYTES;
	    arg_scale = scale_binary;

	  } else if (arg_global_flags & RCTL_GLOBAL_SECONDS) {
	    arg_unit = SCALED_UNIT_SECONDS;
	    arg_scale = scale_metric;

	  } else {
	    arg_unit = SCALED_UNIT_NONE;
	    arg_scale = scale_metric;
	  }
	  /* parse -v value string */
	  if (arg_valuestring) {
	    if (scaledtouint64(arg_valuestring,
			       &arg_value, NULL, &arg_modifier, NULL,
			       arg_scale, arg_unit,
			       SCALED_ALL_FLAGS)) {

	      warn(gettext("invalid -v value %s"),
		   arg_valuestring);
	      errflg = 1;
	      goto done_parse;
	    }
	    if (arg_value > arg_global_max) {
	      warn(gettext("-v value %s exceeds system "
			   "limit for resource control: %s"),
		   arg_valuestring, arg_name);
	      errflg = 1;
	      goto done_parse;
	    }
	  }
	  /* parse action */
	  if (arg_action_string) {

	    char *sigchr;
	    char *iter;

	    if ((strcmp(arg_action_string,
			"signal") == 0) ||
		(strcmp(arg_action_string,
			"sig") == 0)) {

	      if (arg_operation & ACTION_ENABLE) {
		warn(gettext(
                                            "signal name or number must be "
                                            "specified with -e"));
		errflg = 1;
		goto done_parse;
	      }
	      arg_action = RCTL_LOCAL_SIGNAL;
	      arg_signal = -1;

	    } else if ((strncmp(arg_action_string,
				"signal=", strlen("signal=")) == 0) ||
		       (strncmp(arg_action_string,
                                "sig=", strlen("sig=")) == 0)) {

	      arg_action = RCTL_LOCAL_SIGNAL;
	      sigchr = strrchr(arg_action_string, '=');
	      sigchr++;

	      iter = sigchr;
	      while (*iter) {
		*iter = toupper(*iter);
		iter++;
	      }
	      if (strncmp("SIG", sigchr, 3) == 0)
		sigchr += 3;


	      if (str2sig(sigchr, &arg_signal) != 0) {
		warn(gettext("signal invalid"));
		errflg = 1;
		goto done_parse;
	      }
	    } else if (strcmp(arg_action_string, "deny") == 0) {

	      arg_action = RCTL_LOCAL_DENY;

	    } else if (strcmp(arg_action_string, "all") == 0) {

	      if (arg_operation & ACTION_ENABLE) {
		warn(gettext(
			     "cannot use action 'all' with -e"));
		errflg = 1;
		goto done_parse;
	      }
                                arg_action = RCTL_LOCAL_DENY |
				  RCTL_LOCAL_SIGNAL;
                                arg_signal = -1;
                                goto done_parse;
	    } else {
	      warn(gettext("action invalid"));
	      errflg = 1;
	      goto done_parse;
	    }
	  }
	  /* cannot manipulate system rctls */
	  if (arg_priv == RCPRIV_SYSTEM) {

	    warn(gettext("cannot modify system values"));
	    errflg = 1;
	    goto done_parse;
	  }
	  /* validate that the privilege is allowed */
	  if ((arg_priv == RCPRIV_BASIC) &&
	      (arg_global_flags & RCTL_GLOBAL_NOBASIC)) {

	    warn(gettext("basic values not allowed on rctl %s"),
		 arg_name);
	    errflg = 1;
	    goto done_parse;
	  }
	  /* validate that actions are appropriate for given rctl */
	  if ((arg_operation & ACTION_ENABLE) &&
	      (arg_action & RCTL_LOCAL_DENY) &&
	      (arg_global_flags & RCTL_GLOBAL_DENY_NEVER)) {

	    warn(gettext("unable to enable deny on rctl with "
			 "global flag 'no-deny'"));
	    errflg = 1;
	    goto done_parse;
	  }
	  if ((arg_operation & ACTION_DISABLE) &&
	      (arg_action & RCTL_LOCAL_DENY) &&
	      (arg_global_flags & RCTL_GLOBAL_DENY_ALWAYS)) {

	    warn(gettext("unable to disable deny on rctl with "
			 "global flag 'deny'"));
	    errflg = 1;
	    goto done_parse;
	  }
	  if ((arg_operation & ACTION_ENABLE) &&
	      (arg_action & RCTL_LOCAL_SIGNAL) &&
	      (arg_global_flags & RCTL_GLOBAL_SIGNAL_NEVER)) {

	    warn(gettext("unable to enable signal on rctl with "
			 "global flag 'no-signal'"));
	    errflg = 1;
	    goto done_parse;
	  }
	  /* now set defaults for options not supplied */

	  /*
	   * default privilege to basic if this is a seting an rctl
	   * operation
	   */
	  if (arg_operation & ACTION_SET) {
	    if (arg_priv == 0) {
	      arg_priv = RCPRIV_BASIC;
	    }
	  }
	  /*
	   * -p is required when set a basic task,
	   * project or zone rctl
	   */
	  if ((arg_pid == -1) &&
	      (arg_priv == RCPRIV_BASIC) &&
	      (arg_entity_type != RCENTITY_PROCESS) &&
	      (arg_operation & ACTION_SET) &&
	      (arg_name) &&
	      (arg_name_entity == RCENTITY_TASK ||
                    arg_name_entity == RCENTITY_PROJECT ||
	       arg_name_entity == RCENTITY_ZONE)) {

	    warn(gettext("-p pid required when setting or "
			 "replacing task or project rctl"));
	    errflg = 1;
	    goto done_parse;
	  }
        } else {

	  /* validate for list mode */
	  /* -p is not valid in list mode */
	  if (arg_pid != -1) {
	    warn(gettext("-p pid requires -s, -r, -x, -e, or -d"));
	    errflg = 1;
	    goto done_parse;
	  }
        }
        /* getting/setting process rctl on task or project is error */
        if ((arg_name && (arg_name_entity == RCENTITY_PROCESS)) &&
            ((arg_entity_type == RCENTITY_TASK) ||
	     (arg_entity_type == RCENTITY_PROJECT))) {

	  warn(gettext("cannot get/set process rctl on task "
		       "or project"));
	  errflg = 1;
	  goto done_parse;
        }
        /* getting/setting task rctl on project is error */
        if ((arg_name && (arg_name_entity == RCENTITY_TASK)) &&
            (arg_entity_type == RCENTITY_PROJECT)) {

	  warn(gettext("cannot get/set task rctl on project"));
	  errflg = 1;
	  goto done_parse;
        }

 done_parse:

        /* free any rctlblk's that we may have allocated */
        if (rctlblkA) {
	  free(rctlblkA);
	  rctlblkA = NULL;
        }
        if (rctlblkB) {
	  free(rctlblkB);
	  rctlblkB = NULL;
        }
        if (errflg)
	  usage();

        /* catch signals from terminal */
        if (sigset(SIGHUP, SIG_IGN) == SIG_DFL)
	  (void) sigset(SIGHUP, intr);
        if (sigset(SIGINT, SIG_IGN) == SIG_DFL)
	  (void) sigset(SIGINT, intr);
        if (sigset(SIGQUIT, SIG_IGN) == SIG_DFL)
	  (void) sigset(SIGQUIT, intr);
        (void) sigset(SIGTERM, intr);

        while (--argc >= 0 && !interrupt) {
	  pr_info_handle_t p;
	  char *arg = *argv++;
	  int intarg;
	  char *end;
	  errflg = 0;

	  gret = 0;

	  /* Store int version of arg */
	  errno = 0;
	  intarg = strtoul(arg, &end, 10);
	  if (errno || *end != '\0' || end == arg) {
	    intarg = -1;
	  }

	  /*
	   * -p defaults to arg if basic and collective rctl
	   * and -i process is specified
	   */
	  if ((arg_pid == -1) &&
	      (arg_priv == RCPRIV_BASIC) &&
	      (arg_entity_type == RCENTITY_PROCESS) &&
	      (arg_name) &&
	      (arg_name_entity == RCENTITY_TASK ||
	       arg_name_entity == RCENTITY_PROJECT)) {
	    arg_pid_string = arg;
	    errno = 0;
	    arg_pid = intarg;
	  }
	  /* Specifying a recipient pid and -i pid is redundent */
	  if (arg_pid != -1 && arg_entity_type == RCENTITY_PROCESS &&
	      arg_pid != intarg) {
	    warn(gettext("option -p pid must match -i process"));
	    errflg = 1;
	    continue;
	  }
	  /* use recipient pid if we have one */
	  if (arg_pid_string != NULL) {
	    target_id = arg_pid_string;
	    search_type = RCENTITY_PROCESS;
	  } else {
	    target_id = arg;
	    search_type = arg_entity_type;
	  }
	  (void) fflush(stdout);  /* process-at-a-time */

	  if (arg_operation != 0) {

	    if ((pid = grab_process_by_id(target_id,
					  search_type, &p, arg_priv, &gret)) < 0) {
	      /*
	       * Mark that an error occurred so that the
	       * return value can be set, but continue
	       * on with other processes
	       */
	      errflg = 1;
	      continue;
	    }

	    /*
	     * At this point, the victim process is held.
	     * Do not call any Pgrab-unsafe functions until
	     * the process is released via release_process().
	     */

	    errflg = get_rctls(p.pr);

	    if (arg_operation & ACTION_DELETE) {

	      /* match by privilege, value, and pid */
	      if (match_rctl(p.pr, &rctlblkA, arg_name,
			     arg_valuestring, arg_value, arg_priv,
			     arg_pid) != 0 || rctlblkA == NULL) {

		if (interrupt)
		  goto out;

		preserve_error(gettext("no matching "
                                            "resource control found for "
				       "deletion"));
		errflg = 1;
		goto out;
	      }
	      /*
	       * grab correct process.  This is neccessary
	       * if the recipient pid does not match the
	       * one we grabbed
	       */
	      pid = regrab_process(
				   rctlblk_get_recipient_pid(rctlblkA),
				   &p, arg_priv, &gret);

	      if (pid < 0) {
		errflg = 1;
		goto out;
	      }
	      if (prctl_setrctl(p.pr, arg_name, NULL,
				rctlblkA, RCTL_DELETE) != 0) {
		errflg = 1;
		goto out;
	      }
	    } else if (arg_operation & ACTION_SET) {

	      /* match by privilege, value, and pid */

	      if (match_rctl(p.pr, &rctlblkA, arg_name,
			     arg_valuestring, arg_value, arg_priv,
			     arg_pid) == 0) {

		if (interrupt)
		  goto out;

		preserve_error(gettext("resource "
				       "control already exists"));
		errflg = 1;
		goto out;
	      }
	      rctlblkB = calloc(1, rctlblk_size());
	      if (rctlblkB == NULL) {
		preserve_error(gettext(
				       "malloc failed"), strerror(errno));
		errflg = 1;
		goto out;
	      }
	      rctlblk_set_value(rctlblkB, arg_value);
	      rctlblk_set_privilege(rctlblkB, arg_priv);
	      if (change_action(rctlblkB)) {
		errflg = 1;
		goto out;
	      }
	      if (prctl_setrctl(p.pr, arg_name, NULL,
				rctlblkB, RCTL_INSERT) != 0) {
		errflg = 1;
		goto out;
	      }
	    } else if (arg_operation & ACTION_REPLACE) {
	      /*
	       * match rctl for deletion by privilege and
	       * pid only
	       */
	      if (match_rctl(p.pr, &rctlblkA, arg_name,
			     NULL, 0, arg_priv,
			     arg_pid) != 0 || rctlblkA == NULL) {

		if (interrupt)
		  goto out;

		preserve_error(gettext("no matching "
				       "resource control to replace"));
		errflg = 1;
		goto out;
	      }
	      /*
	       * grab correct process.  This is neccessary
	       * if the recipient pid does not match the
	       * one we grabbed
	       */
	      pid = regrab_process(
				   rctlblk_get_recipient_pid(rctlblkA),
				   &p, arg_priv, &gret);
	      if (pid < 0) {
		errflg = 1;
		goto out;
	      }
	      pid = rctlblk_get_recipient_pid(rctlblkA);

	      /*
	       * match by privilege, value and pid to
	       * check if new rctl  already exists
	       */
	      if (match_rctl(p.pr, &rctlblkB, arg_name,
			     arg_valuestring, arg_value, arg_priv,
			     pid) < 0) {

		if (interrupt)
		  goto out;

		preserve_error(gettext(
				       "Internal Error"));
		errflg = 1;
		goto out;
	      }
	      /*
	       * If rctl already exists, and it does not
	       *  match the one that we will delete, than
	       *  the replace will fail.
	       */
	      if (rctlblkB != NULL &&
                                    arg_value !=
		  rctlblk_get_value(rctlblkA)) {

		preserve_error(gettext("replacement "
                                            "resource control already "
				       "exists"));

		errflg = 1;
		goto out;
	      }
	      /* create new rctl */
	      rctlblkB = calloc(1, rctlblk_size());
	      if (rctlblkB == NULL) {
		preserve_error(gettext(
				       "malloc failed"), strerror(errno));
		errflg = 1;
		goto out;
	      }
	      rctlblk_set_value(rctlblkB, arg_value);
	      rctlblk_set_privilege(rctlblkB,
                                    rctlblk_get_privilege(rctlblkA));
	      if (change_action(rctlblkB)) {
		errflg = 1;
		goto out;
	      }
	      /* do replacement */
	      if (prctl_setrctl(p.pr, arg_name, rctlblkA,
				rctlblkB, RCTL_REPLACE) != 0) {
		errflg = 1;
		goto out;
	      }
	    } else if (arg_operation &
		       (ACTION_ENABLE | ACTION_DISABLE)) {

	      rctlblkB = calloc(1, rctlblk_size());
	      if (rctlblkB == NULL) {
		preserve_error(gettext(
				       "malloc failed"), strerror(errno));
		errflg = 1;
		goto out;
	      }
	      /* match by privilege, value, and pid */
	      if (match_rctl(p.pr, &rctlblkA, arg_name,
			     arg_valuestring, arg_value, arg_priv,
			     arg_pid) != 0) {

		if (interrupt)
		  goto out;

		/* if no match, just set new rctl */
		if (arg_priv == 0)
		  arg_priv = RCPRIV_BASIC;

		if ((arg_priv == RCPRIV_BASIC) &&
		    (arg_entity_type !=
		     RCENTITY_PROCESS) &&
		    (arg_pid_string == NULL)) {
		  preserve_error(gettext(
                                                    "-p required when setting "
                                                    "basic rctls"));
		  errflg = 1;
		  goto out;
		}
		rctlblk_set_value(rctlblkB,
				  arg_value);
		rctlblk_set_privilege(
				      rctlblkB, arg_priv);
		if (change_action(rctlblkB)) {
		  errflg = 1;
		  goto out;
		}
		if (prctl_setrctl(p.pr,
				  arg_name, NULL, rctlblkB,
				  RCTL_INSERT) != 0) {
		  errflg = 1;
		  goto out;
		}
		goto out;
	      }
	      if (rctlblkA == NULL) {
		preserve_error(gettext("no matching "
				       "resource control found"));
		errflg = 1;
		goto out;
	      }
	      /*
	       * grab correct process.  This is neccessary
	       * if the recipient pid does not match the
	       * one we grabbed
	       */
	      pid = regrab_process(
				   rctlblk_get_recipient_pid(rctlblkA),
				   &p, arg_priv, &gret);
	      if (pid < 0) {
		errflg = 1;
		goto out;
	      }
                                localaction =
				  rctlblk_get_local_action(rctlblkA,
							   &signal);
                                rctlblk_set_local_action(rctlblkB, localaction,
							 signal);
                                rctlblk_set_privilege(rctlblkB,
						      rctlblk_get_privilege(rctlblkA));
                                rctlblk_set_value(rctlblkB,
						  rctlblk_get_value(rctlblkA));

                                if (change_action(rctlblkB)) {
				  errflg = 1;
				  goto out;
                                }
                                if (prctl_setrctl(p.pr, arg_name, rctlblkA,
						  rctlblkB, RCTL_REPLACE) != 0) {
				  errflg = 1;
				  goto out;
                                }
	    }
	  out:
	    release_process(p.pr);
	    if (rctlblkA)
	      free(rctlblkA);
	    if (rctlblkB)
	      free(rctlblkB);

	    /* Print any errors that occurred */
	    if (errflg && *global_error != '\0') {
	      proc_unctrl_psinfo(&(p.psinfo));
	      (void) fprintf(stderr, "%d:\t%.70s\n",
			     (int)p.pid, p.psinfo.pr_psargs);
	      warn("%s\n", global_error);
	      break;
	    }
	  } else {

	    struct project projent;
	    char buf[PROJECT_BUFSZ];
	    char zonename[ZONENAME_MAX];

	    /*
	     * Hack to allow the user to specify a system
	     * process.
	     */
	    gret = G_SYS;
	    pid = grab_process_by_id(
				     target_id, search_type, &p, RCPRIV_BASIC, &gret);

	    /*
	     * Print system process if user chose specifically
	     * to inspect a system process.
	     */
	    if (arg_entity_type == RCENTITY_PROCESS &&
                            pid < 0 &&
		gret == G_SYS) {
	      /*
	       * Add blank lines between output for
	       * operands.
	       */
	      if (printed) {
		(void) fprintf(stdout, "\n");
	      }

	      proc_unctrl_psinfo(&(p.psinfo));
	      (void) printf(
			    "process: %d: %s [ system process ]\n",
			    (int)p.pid, p.psinfo.pr_psargs);

	      printed = 1;
	      continue;

	    } else if (pid < 0) {

	      /*
	       * Mark that an error occurred so that the
	       * return value can be set, but continue
	       * on with other processes
	       */
	      errflg = 1;
	      continue;
	    }

	    errflg = get_rctls(p.pr);

	    release_process(p.pr);

	    /* handle user interrupt of getting rctls */
	    if (interrupt)
	      break;

	    /* add blank lines between output for operands */
	    if (printed) {
	      (void) fprintf(stdout, "\n");
	    }
	    /* First print any errors */
	    if (errflg) {
	      warn("%s\n", global_error);
	      free_lists();
	      break;
	    }
	    if (getprojbyid(p.projid, &projent, buf,
                            sizeof (buf))) {
	      p.projname = projent.pj_name;
	    } else {
	      p.projname = "";
	    }
	    if (getzonenamebyid(p.zoneid, zonename,
				sizeof (zonename)) > 0) {
	      p.zonename = zonename;
	    } else {
	      p.zonename = "";
	    }
	    print_rctls(&p);
	    printed = 1;
	    /* Free the resource control lists */
	    free_lists();
	  }
        }
        if (interrupt)
	  errflg = 1;

        /*
         * return error if one occurred
         */
        return (errflg);
}


static void
intr(int sig)
{
  interrupt = sig;
}

/*
 * get_rctls(struct ps_prochandle *, const char *)
 *
 * If controlname is given, store only controls for that named
 * resource. If controlname is NULL, store all controls for all
 * resources.
 *
 * This function is Pgrab-safe.
 */
static int
get_rctls(struct ps_prochandle *Pr)
{
  int ret = 0;

  if (arg_name == NULL) {
    if (rctl_walk(store_rctls, Pr) != 0)
      ret = 1;
  } else {
    ret = store_rctls(arg_name, Pr);
  }
  return (ret);
}

/*
 * store_rctls(const char *, void *)
 *
 * Store resource controls for the given name in a linked list.
 * Honor the user's options, and store only the ones they are
 * interested in. If priv is not 0, show only controls that match
 * the given privilege.
 *
 * This function is Pgrab-safe
 */
static int
store_rctls(const char *rctlname, void *walk_data)
{
  struct ps_prochandle *Pr = walk_data;
  rctlblk_t *rblk2, *rblk_tmp, *rblk1 = NULL;
  prctl_list_t *list = NULL;
  rctl_priv_t rblk_priv;
  rctl_entity_t rblk_entity;

  if (((rblk1 = calloc(1, rctlblk_size())) == NULL) ||
      ((rblk2 = calloc(1, rctlblk_size())) == NULL)) {
    if (rblk1 != NULL)
      free(rblk1);
    preserve_error(gettext("malloc failed: %s"),
		   strerror(errno));
    return (1);
  }
  if (pr_getrctl(Pr, rctlname, NULL, rblk1, RCTL_FIRST)) {
    preserve_error(gettext("failed to get resource control "
			   "for %s: %s"), rctlname, strerror(errno));
    free(rblk1);
    free(rblk2);
    return (1);
  }
  /* Store control if it matches privilege and enity type criteria */
  rblk_priv = rctlblk_get_privilege(rblk1);
  rblk_entity = 0;
  if (strncmp(rctlname, "process.",
	      strlen("process.")) == 0)
    rblk_entity = RCENTITY_PROCESS;
  else if (strncmp(rctlname, "project.",
		   strlen("project.")) == 0)
    rblk_entity = RCENTITY_PROJECT;
  else if (strncmp(rctlname, "task.",
		   strlen("task.")) == 0)
    rblk_entity = RCENTITY_TASK;
  else if (strncmp(rctlname, "zone.",
		   strlen("zone.")) == 0)
    rblk_entity = RCENTITY_ZONE;

  if (((arg_priv == 0) || (rblk_priv == arg_priv)) &&
      ((arg_name == NULL) ||
       strncmp(rctlname, arg_name,
	       strlen(arg_name)) == 0) &&
      (arg_entity_string == NULL ||
       rblk_entity >= arg_entity_type)) {

    /* Once we know we have some controls, store the name */
    if ((list = store_list_entry(rctlname)) == NULL) {
      free(rblk1);
      free(rblk2);
      return (1);
    }
    if (store_value_entry(rblk1, list) == NULL) {
      free(rblk1);
      free(rblk2);
      return (1);
    }
  }
  while (pr_getrctl(Pr, rctlname, rblk1, rblk2, RCTL_NEXT) == 0) {

    /*
     * in case this is stuck for some reason, allow manual
     * interrupt
     */
    if (interrupt) {
      free(rblk1);
      free(rblk2);
      return (1);
    }
    rblk_priv = rctlblk_get_privilege(rblk2);
    /*
     * Store control if it matches privilege and entity type
     * criteria
     */
    if (((arg_priv == 0) || (rblk_priv == arg_priv)) &&
	((arg_name == NULL) ||
	 strncmp(rctlname, arg_name,
		 strlen(arg_name)) == 0) &&
	(arg_entity_string == NULL ||
	 rblk_entity == arg_entity_type)) {

      /* May not have created the list yet. */
      if (list == NULL) {
	if ((list = store_list_entry(rctlname))
	    == NULL) {
	  free(rblk1);
	  free(rblk2);
	  return (1);
	}
      }
      if (store_value_entry(rblk2, list) == NULL) {
	free(rblk1);
	free(rblk2);
	return (1);
      }
    }
    rblk_tmp = rblk1;
    rblk1 = rblk2;
    rblk2 = rblk_tmp;
  }
  free(rblk1);
  free(rblk2);
  return (0);
}

/*
 * store_value_entry(rctlblk_t *, prctl_list_t *)
 *
 * Store an rblk for a given resource control into the global list.
 *
 * This function is Pgrab-safe.
 */
prctl_value_t *
store_value_entry(rctlblk_t *rblk, prctl_list_t *list)
{
  prctl_value_t *e = calloc(1, sizeof (prctl_value_t));
  rctlblk_t *store_blk = calloc(1, rctlblk_size());
  prctl_value_t *iter = list->val_list;

  if (e == NULL || store_blk == NULL) {
    preserve_error(gettext("malloc failed %s"),
		   strerror(errno));
    if (e != NULL)
      free(e);
    if (store_blk != NULL)
      free(store_blk);
    return (NULL);
  }
  if (iter == NULL)
    list->val_list = e;
  else {
    while (iter->next != NULL) {
      iter = iter->next;
    }
    iter->next = e;
  }
  bcopy(rblk, store_blk, rctlblk_size());

  e->rblk = store_blk;
  e->next = NULL;
  return (e);
}

/*
 * store_list_entry(const char *)
 *
 * Store a new resource control value in the global list. No checking
 * for duplicates done.
 *
 * This function is Pgrab-safe.
 */
prctl_list_t *
store_list_entry(const char *name)
{
  prctl_list_t *e = calloc(1, sizeof (prctl_list_t));

  if (e == NULL) {
    preserve_error(gettext("malloc failed %s"),
		   strerror(errno));
    return (NULL);
  }
  if ((e->name = calloc(1, strlen(name) + 1)) == NULL) {
    preserve_error(gettext("malloc failed %s"),
		   strerror(errno));
    free(e);
    return (NULL);
  }
  (void) strcpy(e->name, name);
  e->val_list = NULL;

  if (global_rctl_list_head == NULL) {
    global_rctl_list_head = e;
    global_rctl_list_tail = e;
  } else {
    global_rctl_list_tail->next = e;
    global_rctl_list_tail = e;
  }
  e->next = NULL;
  return (e);
}

/*
 * free_lists()
 *
 * Free all resource control blocks and values from the global lists.
 *
 * This function is Pgrab-safe.
 */
void
free_lists()
{
  prctl_list_t *new_list, *old_list = global_rctl_list_head;
  prctl_value_t *old_val, *new_val;

  while (old_list != NULL) {
    old_val = old_list->val_list;
    while (old_val != NULL) {
      free(old_val->rblk);
      new_val = old_val->next;
      free(old_val);
      old_val = new_val;
    }
    free(old_list->name);
    new_list = old_list->next;
    free(old_list);
    old_list = new_list;
  }
  global_rctl_list_head = NULL;
  global_rctl_list_tail = NULL;
}

void
print_heading()
{

  /* print headings */
  (void) fprintf(stdout, "%-8s%-16s%-9s%-7s%-28s%10s\n",
		 "NAME", "PRIVILEGE", "VALUE",
		 "FLAG", "ACTION", "RECIPIENT");
}

/*
 * print_rctls()
 *
 * Print all resource controls from the global list that was
 * previously populated by store_rctls.
 */
void
print_rctls(pr_info_handle_t *p)
{
  prctl_list_t *iter_list = global_rctl_list_head;
  prctl_value_t *iter_val;
  rctl_qty_t  rblk_value;
  rctl_priv_t rblk_priv;
  uint_t local_action;
  int signal, local_flags, global_flags;
  pid_t pid;
  char rctl_valuestring[SCALED_STRLEN];
  char *unit = NULL;
  scale_t *scale;
  char *string;
  int doneheading = 0;

  if (iter_list == NULL)
    return;

  while (iter_list != NULL) {

    if (doneheading == 0 &&
	arg_entity_type == RCENTITY_PROCESS) {
      proc_unctrl_psinfo(&(p->psinfo));
      doneheading = 1;
      (void) fprintf(stdout,
		     "process: %d: %.70s\n", (int)p->pid,
		     p->psinfo.pr_psargs);
      if (!arg_parseable_mode)
	print_heading();
    }
    if (doneheading == 0 &&
	arg_entity_type == RCENTITY_TASK) {
      doneheading = 1;
      (void) fprintf(stdout, "task: %d\n", (int)p->taskid);
      if (!arg_parseable_mode)
	print_heading();
    }
    if (doneheading == 0 &&
	arg_entity_type == RCENTITY_PROJECT) {
      if (!arg_parseable_mode && doneheading)
	(void) fprintf(stdout, "\n");
      doneheading = 1;
      (void) fprintf(stdout,
		     "project: %d: %.70s\n", (int)p->projid,
		     p->projname);
      if (!arg_parseable_mode)
	print_heading();
    }
    if (doneheading == 0 &&
	arg_entity_type == RCENTITY_ZONE) {
      doneheading = 1;
      (void) fprintf(stdout,
		     "zone: %d: %.70s\n", (int)p->zoneid,
		     p->zonename);
      if (!arg_parseable_mode)
	print_heading();
    }
    /* only print name once in normal output */
    if (!arg_parseable_mode)
      (void) fprintf(stdout, "%s\n", iter_list->name);

    iter_val = iter_list->val_list;

    /* if for some reason there are no values, skip */
    if (iter_val == 0)
      continue;


    /* get the global flags the first rctl only */
    global_flags = rctlblk_get_global_flags(iter_val->rblk);


    if (global_flags & RCTL_GLOBAL_BYTES) {
      unit = SCALED_UNIT_BYTES;
      scale = scale_binary;

    } else if (global_flags & RCTL_GLOBAL_SECONDS) {
      unit = SCALED_UNIT_SECONDS;
      scale = scale_metric;

    } else {
      unit = SCALED_UNIT_NONE;
      scale = scale_metric;
    }
    /* iterate over an print all control values */
    while (iter_val != NULL) {

      /* print name or empty name field */
      if (!arg_parseable_mode)
	(void) fprintf(stdout, "%8s", "");
      else
	(void) fprintf(stdout, "%s ", iter_list->name);


      rblk_priv = rctlblk_get_privilege(iter_val->rblk);
      if (!arg_parseable_mode)
	print_priv(rblk_priv, "%-16s");
      else
	print_priv(rblk_priv, "%s ");

      rblk_value = rctlblk_get_value(iter_val->rblk);
      if (arg_parseable_mode) {
	(void) fprintf(stdout, "%llu ", rblk_value);

      } else {

	(void) uint64toscaled(rblk_value, 4, "E",
			      rctl_valuestring, NULL, NULL,
			      scale, NULL, 0);

	(void) fprintf(stdout, "%5s",
		       rctl_valuestring);
	(void) fprintf(stdout, "%-4s", unit);
      }
      local_flags = rctlblk_get_local_flags(iter_val->rblk);

      if (local_flags & RCTL_LOCAL_MAXIMAL) {

	if (global_flags & RCTL_GLOBAL_INFINITE) {
	  string = "inf";
	} else {
	  string = "max";
	}
      } else {
	string = "-";
      }
      if (arg_parseable_mode)
	(void) fprintf(stdout, "%s ", string);
      else
	(void) fprintf(stdout, "%4s%3s",
		       string, "");


      local_action = rctlblk_get_local_action(iter_val->rblk,
					      &signal);

      if (arg_parseable_mode)
	print_local_action(local_action, &signal,
			   "%s ");
      else
	print_local_action(local_action, &signal,
			   "%-28s");

      pid = rctlblk_get_recipient_pid(iter_val->rblk);

      if (arg_parseable_mode) {
	if (pid < 0) {
	  (void) fprintf(stdout, "%s\n", "-");
	} else {
	  (void) fprintf(stdout, "%d\n",
			 (int)pid);
	}
      } else {
	if (pid < 0) {
	  (void) fprintf(stdout, "%10s\n", "-");
	} else {
	  (void) fprintf(stdout, "%10d\n",
			 (int)pid);
	}
      }
      iter_val = iter_val->next;
    }
    iter_list = iter_list->next;
  }
}

/*
 *
 * match_rctl
 *
 * find the first rctl with matching name, value, priv, and recipient pid
 */
int
match_rctl(struct ps_prochandle *Pr, rctlblk_t **rctl, char *name,
	   char *valuestringin, int valuein, rctl_priv_t privin, int pidin)
{
  rctlblk_t *next;
  rctlblk_t *last;
  rctlblk_t *tmp;

  *rctl = NULL;

  next = calloc(1, rctlblk_size());
  last = calloc(1, rctlblk_size());

  if ((last == NULL) || (next == NULL)) {
    preserve_error(gettext("malloc failed"), strerror(errno));
    return (-1);
  }
  /*
   * For this resource name, now iterate through all
   * the  controls, looking for a match to the
   * user-specified input.
   */
  if (pr_getrctl(Pr, name, NULL, next, RCTL_FIRST)) {
    preserve_error(gettext("failed to get resource control "
			   "for %s: %s"), name, strerror(errno));
    return (-1);
  }
  if (match_rctl_blk(next, valuestringin, valuein, privin, pidin) == 1) {
    free(last);
    *rctl = next;
    return (0);
  }
  tmp = next;
  next = last;
  last = tmp;

  while (pr_getrctl(Pr, name, last, next, RCTL_NEXT) == 0) {

    /* allow user interrupt */
    if (interrupt)
      break;

    if (match_rctl_blk(next, valuestringin, valuein, privin, pidin)
	== 1) {
      free(last);
      *rctl = next;
      return (0);
    }
    tmp = next;
    next = last;
    last = tmp;
  }
  free(next);
  free(last);

  return (1);
}

/*
 * int match_rctl_blk(rctlblk_t *, char *, uint64, rctl_priv_t, int pid)
 *
 * Input
 *   Must supply a valid rctl, value, privilege, and pid to match on.
 *   If valuestring is NULL, then valuestring and valuein will not be used
 *   If privilege type is 0 it will not be used.
 *   If pid is -1 it will not be used.
 *
 * Return values
 *   Returns 1 if a matching rctl given matches the parameters specified, and
 *   0 if they do not.
 *
 * This function is Pgrab-safe.
 */
int
match_rctl_blk(rctlblk_t *rctl, char *valuestringin,
	       uint64_t valuein, rctl_priv_t privin, int pidin)
{

  rctl_qty_t value;
  rctl_priv_t priv;
  pid_t pid;
  int valuematch = 1;
  int privmatch = 1;
  int pidmatch = 1;

  value = rctlblk_get_value(rctl);
  priv = rctlblk_get_privilege(rctl);
  pid = rctlblk_get_recipient_pid(rctl);

  if (valuestringin) {

    if (arg_modifier == NULL) {
      valuematch = (valuein == value);
    } else {
      valuematch = scaledequint64(valuestringin, value,
				  PRCTL_VALUE_WIDTH,
				  arg_scale, arg_unit,
				  SCALED_ALL_FLAGS);
    }
  }
  if (privin != 0) {
    privmatch = (privin == priv);
  }
  if (pidin != -1) {
    pidmatch = (pidin == pid);
  }
  return (valuematch && privmatch && pidmatch);
}

static int
change_action(rctlblk_t *blk)
{
  int signal = 0;
  int action;

  action = rctlblk_get_local_action(blk, &signal);

  if (arg_operation & ACTION_ENABLE) {

    if (arg_action & RCTL_LOCAL_SIGNAL) {
      signal = arg_signal;
    }
    action = action | arg_action;
    /* add local action */
    rctlblk_set_local_action(blk, action, signal);

  } else if (arg_operation & ACTION_DISABLE) {

    /*
     * if deleting signal and signal number is specified,
     * then signal number must match
     */
    if ((arg_action & RCTL_LOCAL_SIGNAL) &&
	(arg_signal != -1)) {
      if (arg_signal != signal) {
	preserve_error(gettext("signal name or number "
			       "does not match existing action"));
	return (-1);
      }
    }
    /* remove local action */
    action = action & (~arg_action);
    rctlblk_set_local_action(blk, RCTL_LOCAL_NOACTION, 0);
    rctlblk_set_local_action(blk, action, signal);
  }
  /* enable deny if it must be enabled */
  if (arg_global_flags & RCTL_GLOBAL_DENY_ALWAYS) {
    rctlblk_set_local_action(blk, RCTL_LOCAL_DENY | action,
			     signal);
  }
  return (0);
}

/*
 * prctl_setrctl
 *
 * Input
 *   This function expects that input has been validated. In the
 *   case of a replace operation, both old_rblk and new_rblk must
 *   be valid resource controls. If a resource control is being
 *   created, only new_rblk must be supplied. If a resource control
 *   is being deleted, only new_rblk must be supplied.
 *
 * If the privilege is a priviliged type, at this time, the process
 * tries to take on superuser privileges.
 */
int
prctl_setrctl(struct ps_prochandle *Pr, const char *name,
	      rctlblk_t *old_rblk, rctlblk_t *new_rblk, uint_t flags)
{
  int ret = 0;
  rctl_priv_t rblk_priv;
  psinfo_t psinfo;
  zoneid_t oldzoneid = GLOBAL_ZONEID;
  prpriv_t *old_prpriv = NULL, *new_prpriv = NULL;
  priv_set_t *eset, *pset;
  boolean_t relinquish_failed = B_FALSE;

  rblk_priv = rctlblk_get_privilege(new_rblk);

  if (rblk_priv == RCPRIV_SYSTEM) {
    preserve_error(gettext("cannot modify system values"));
    return (1);
  }
  if (rblk_priv == RCPRIV_PRIVILEGED) {
    new_prpriv = proc_get_priv(Pstatus(Pr)->pr_pid);
    if (new_prpriv == NULL) {
      preserve_error(gettext("cannot get process privileges "
			     "for pid %d: %s"), Pstatus(Pr)->pr_pid,
		     strerror(errno));
      return (1);
    }
    /*
     * We only have to change the process privileges if it doesn't
     * already have PRIV_SYS_RESOURCE.  In addition, we want to make
     * sure that we don't leave a process with elevated privileges,
     * so we make sure the process dies if we exit unexpectedly.
     */
    eset = (priv_set_t *)
                    &new_prpriv->pr_sets[new_prpriv->pr_setsize *
					 priv_getsetbyname(PRIV_EFFECTIVE)];
    pset = (priv_set_t *)
                    &new_prpriv->pr_sets[new_prpriv->pr_setsize *
					 priv_getsetbyname(PRIV_PERMITTED)];
    if (!priv_ismember(eset, PRIV_SYS_RESOURCE)) {
      /* Keep track of original privileges */
      old_prpriv = proc_get_priv(Pstatus(Pr)->pr_pid);
      if (old_prpriv == NULL) {
	preserve_error(gettext("cannot get process "
			       "privileges for pid %d: %s"),
		       Pstatus(Pr)->pr_pid, strerror(errno));
	free(new_prpriv);
	return (1);
      }
      (void) priv_addset(eset, PRIV_SYS_RESOURCE);
      (void) priv_addset(pset, PRIV_SYS_RESOURCE);
      if (Psetflags(Pr, PR_KLC) != 0 ||
	  Psetpriv(Pr, new_prpriv) != 0) {
	preserve_error(gettext("cannot set process "
			       "privileges for pid %d: %s"),
		       Pstatus(Pr)->pr_pid, strerror(errno));
	(void) Punsetflags(Pr, PR_KLC);
	free(new_prpriv);
	free(old_prpriv);
	return (1);
      }
    }
    /*
     * If this is a zone.* rctl, it requires more than
     * PRIV_SYS_RESOURCE: it wants the process to have global-zone
     * credentials.  We temporarily grant non-global zone processes
     * these credentials, and make sure the process dies if we exit
     * unexpectedly.
     */
    if (arg_name &&
                    arg_name_entity == RCENTITY_ZONE &&
	getzoneid() == GLOBAL_ZONEID &&
	proc_get_psinfo(Pstatus(Pr)->pr_pid, &psinfo) == 0 &&
	(oldzoneid = psinfo.pr_zoneid) != GLOBAL_ZONEID) {
      /*
       * We need to give this process superuser
       * ("super-zone") privileges.
       *
       * Must never return without setting this back!
       */
      if (Psetflags(Pr, PR_KLC) != 0 ||
	  Psetzoneid(Pr, GLOBAL_ZONEID) < 0) {
	preserve_error(gettext(
                                    "cannot set global-zone "
                                    "privileges for pid %d: %s"),
		       Pstatus(Pr)->pr_pid, strerror(errno));
	/*
	 * We couldn't set the zoneid to begin with, so
	 * there's no point in warning the user about
	 * trying to un-set it.
	 */
	oldzoneid = GLOBAL_ZONEID;
	ret = 1;
	goto bail;
      }
    }
  }
  /* Now, actually populate the rctlblk in the kernel */
  if (flags == RCTL_REPLACE) {
    /*
     * Replace should be a delete followed by an insert. This
     * allows us to replace rctl value blocks which match in
     * privilege and value, but have updated actions, etc.
     * setrctl() doesn't allow a direct replace, but we
     * should do the right thing for the user in the command.
     */
    if (pr_setrctl(Pr, name, NULL,
		   old_rblk, RCTL_DELETE)) {
      preserve_error(gettext("failed to delete resource "
			     "control %s for pid %d: %s"), name,
		     Pstatus(Pr)->pr_pid, strerror(errno));
      ret = 1;
      goto bail;
    }
    if (pr_setrctl(Pr, name, NULL,
		   new_rblk, RCTL_INSERT)) {
      preserve_error(gettext("failed to insert resource "
			     "control %s for pid %d: %s"), name,
		     Pstatus(Pr)->pr_pid, strerror(errno));
      ret = 1;
      goto bail;
    }
  } else if (flags == RCTL_INSERT) {
    if (pr_setrctl(Pr, name, NULL,
		   new_rblk, RCTL_INSERT)) {
      preserve_error(gettext("failed to create resource "
			     "control %s for pid %d: %s"), name,
		     Pstatus(Pr)->pr_pid, strerror(errno));
      ret = 1;
      goto bail;
    }
  } else if (flags == RCTL_DELETE) {
    if (pr_setrctl(Pr, name, NULL,
		   new_rblk, RCTL_DELETE)) {
      preserve_error(gettext("failed to delete resource "
			     "control %s for pid %d: %s"), name,
		     Pstatus(Pr)->pr_pid, strerror(errno));
      ret = 1;
      goto bail;
    }
  }
 bail:
  if (oldzoneid != GLOBAL_ZONEID) {
    if (Psetzoneid(Pr, oldzoneid) != 0)
      relinquish_failed = B_TRUE;
  }
  if (old_prpriv != NULL) {
    if (Psetpriv(Pr, old_prpriv) != 0)
      relinquish_failed = B_TRUE;
    free(old_prpriv);
  }
  if (relinquish_failed) {
    /*
     * If this failed, we can't leave a process hanging
     * around with elevated privileges, so we'll have to
     * release the process from libproc, knowing that it
     * will be killed (since we set PR_KLC).
     */
    Pdestroy_agent(Pr);
    preserve_error(gettext("cannot relinquish privileges "
			   "for pid %d. The process was killed."),
		   Pstatus(Pr)->pr_pid);
  } else {
    if (Punsetflags(Pr, PR_KLC) != 0)
      preserve_error(gettext("cannot relinquish privileges "
			     "for pid %d. The process was killed."),
		     Pstatus(Pr)->pr_pid);
  }
  if (new_prpriv != NULL)
    free(new_prpriv);

  return (ret);
}

void
print_priv(rctl_priv_t local_priv, char *format)
{
  char pstring[11];

  switch (local_priv) {
  case RCPRIV_BASIC:
    (void) strcpy(pstring, "basic");
    break;
  case RCPRIV_PRIVILEGED:
    (void) strcpy(pstring, "privileged");
    break;
  case RCPRIV_SYSTEM:
    (void) strcpy(pstring, "system");
    break;
  default:
    (void) sprintf(pstring, "%d", local_priv);
    break;
  }
  /* LINTED */
  (void) fprintf(stdout, format, pstring);
}

void
print_local_action(int action, int *signalp, char *format)
{
  char sig[SIG2STR_MAX];
  char sigstring[SIG2STR_MAX + 7];
  char astring[5 + SIG2STR_MAX + 7];
  int set = 0;

  astring[0] = '\0';

  if (action == RCTL_LOCAL_NOACTION) {
    (void) strcat(astring, "none");
    set++;
  }
  if (action & RCTL_LOCAL_DENY) {
    (void) strcat(astring, "deny");
    set++;
  }
  if ((action & RCTL_LOCAL_DENY) &&
      (action & RCTL_LOCAL_SIGNAL)) {
    (void) strcat(astring, ",");
  }
  if (action & RCTL_LOCAL_SIGNAL) {
    if (sig2str(*signalp, sig))
      (void) snprintf(sigstring, sizeof (astring),
		      "signal=%d", *signalp);
    else
      (void) snprintf(sigstring, sizeof (astring),
		      "signal=%s", sig);

    (void) strcat(astring, sigstring);
    set++;
  }
  if (set)
    /* LINTED */
    (void) fprintf(stdout, format, astring);
  else
    /* LINTED */
    (void) fprintf(stdout, format, action);
}

/*
 * This function is used to grab the process matching the recipient pid
 */
pid_t
regrab_process(pid_t pid, pr_info_handle_t *p, int priv, int *gret)
{

  char pidstring[24];

  gret = 0;
  if (pid == -1)
    return (p->pid);
  if (p->pid == pid)
    return (p->pid);

  release_process(p->pr);
  (void) memset(p, 0, sizeof (*p));

  (void) snprintf(pidstring, 24, "%d", pid);
  return (grab_process_by_id(
			     pidstring, RCENTITY_PROCESS, p, priv, gret));
}

/*
 * int grab_process_by_id(char *, rctl_entity_t, pr_info_handle_t *, int, int *)
 *
 * Input
 *    Supply a non-NULL string containing:
 *      - logical project/zone name or project/zone number if type is
 *      RCENTITY_PROJECT or RCENTITY_ZONE
 *      - task number if type is RCENTITY_TYPE
 *      - a pid if type is RCENTITY_PID
 *    Also supply an un-allocated prochandle, and an allocated info_handle.
 *    This function assumes that the type is set.
 *    If priv is not RCPRIV_BASIC, the grabbed process is required to have
 *    PRIV_SYS_RESOURCE in it's limit set.
 *
 * Return Values
 *    Returns 0 on success and 1 on failure. If there is a process
 *    running under the specified id, success is returned, and
 *    Pr is pointed to the process. Success will be returned and Pr
 *    set to NULL if the matching process is our own.
 *    If success is returned, psinfo will be valid, and pid will
 *    be the process number. The process will also be held at the
 *    end, so release_process should be used by the caller.
 *
 * This function assumes that signals are caught already so that libproc
 * can be safely used.
 *
 * Return Values
 *      pid - Process found and grabbed
 *      -1 - Error
 */
pid_t
grab_process_by_id(char *idname, rctl_entity_t type, pr_info_handle_t *p,
		   int priv, int *gret)
{
  char prbuf[PROJECT_BUFSZ];
  projid_t projid;
  taskid_t taskid;
  zoneid_t zoneid;
  zoneid_t zone_self;
  struct project proj;
  DIR *dirp;
  struct dirent *dentp;
  int found = 0;
  int pid_self;
  int ret;
  int gret_in;
  int intidname;
  char *end;
  prpriv_t *prpriv;
  priv_set_t *prset;

  gret_in = *gret;

  /* get our pid se we do not try to operate on self */
  pid_self = getpid();

  /* Store integer version of id */
  intidname = strtoul(idname, &end, 10);
  if (errno || *end != '\0' || end == idname) {
    intidname = -1;
  }

  /*
   * get our zoneid so we don't try to operate on a project in
   * another zone
   */
  zone_self = getzoneid();

  if (idname == NULL || strcmp(idname, "") == 0) {
    warn(gettext("id name cannot be nuint64\n"));
    return (-1);
  }
  /*
   * Set up zoneid, projid or taskid, as appropriate, so that comparisons
   * can be done later with the input.
   */
  if (type == RCENTITY_ZONE) {
    if (zone_get_id(idname, &zoneid) != 0) {
      warn(gettext("%s: unknown zone\n"), idname);
      return (-1);
    }
  } else if (type == RCENTITY_PROJECT) {
    if (getprojbyname(idname, &proj, prbuf, PROJECT_BUFSZ)
	== NULL) {
      if (getprojbyid(intidname, &proj, prbuf,
		      PROJECT_BUFSZ) == NULL) {
	warn(gettext("%s: cannot find project\n"),
	     idname);
	return (-1);
      }
    }
    projid = proj.pj_projid;
  } else if (type == RCENTITY_TASK) {
    taskid = (taskid_t)atol(idname);
  }
  /*
   * Projects and tasks need to search through /proc for
   * a parent process.
   */
  if (type == RCENTITY_ZONE || type == RCENTITY_PROJECT ||
      type == RCENTITY_TASK) {
    if ((dirp = opendir("/proc")) == NULL) {
      warn(gettext("%s: cannot open /proc directory\n"),
	   idname);
      return (-1);
    }
    /*
     * Look through all processes in /proc. For each process,
     * check if the pr_projid in their psinfo matches the
     * specified id.
     */
    while (dentp = readdir(dirp)) {
      p->pid = atoi(dentp->d_name);

      /* Skip self */
      if (p->pid == pid_self)
	continue;

      if (proc_get_psinfo(p->pid, &(p->psinfo)) != 0)
	continue;

      /* Skip process if it is not what we are looking for */
      if (type == RCENTITY_ZONE &&
	  (p->psinfo).pr_zoneid != zoneid) {
	continue;
      } if (type == RCENTITY_PROJECT &&
	    ((p->psinfo).pr_projid != projid ||
	     (p->psinfo).pr_zoneid != zone_self)) {
	continue;
      } else if (type == RCENTITY_TASK &&
		 (p->psinfo).pr_taskid != taskid) {
	continue;
      }
      /* attempt to grab process */
      if (grab_process(p, gret) != 0)
	continue;

      /*
       * Re-confirm that this process is still running as
       * part of the specified project or task.  If it
       * doesn't match, release the process and return an
       * error. This should only be done if the Pr struct is
       * not NULL.
       */
      if (type == RCENTITY_PROJECT) {
	if (pr_getprojid(p->pr) != projid ||
	    pr_getzoneid(p->pr) != zone_self) {
	  release_process(p->pr);
	  continue;
	}
      } else if (type == RCENTITY_TASK) {
	if (pr_gettaskid(p->pr) != taskid) {
	  release_process(p->pr);
	  continue;
	}
      } else if (type == RCENTITY_ZONE) {
	if (pr_getzoneid(p->pr) != zoneid) {
	  release_process(p->pr);
	  continue;
	}
      }

      /*
       * If we are setting a privileged resource control,
       * verify that process has PRIV_SYS_RESOURCE in it's
       * limit set.  If it does not, then we will not be
       * able to give this process the privilege it needs
       * to set the resource control.
       */
      if (priv != RCPRIV_BASIC) {
	prpriv = proc_get_priv(p->pid);
	if (prpriv == NULL) {
	  release_process(p->pr);
	  continue;
	}
	prset = (priv_set_t *)
                                    &prpriv->pr_sets[prpriv->pr_setsize *
						     priv_getsetbyname(PRIV_LIMIT)];
	if (!priv_ismember(prset, PRIV_SYS_RESOURCE)) {
	  release_process(p->pr);
	  continue;
	}
      }
      found = 1;

      p->taskid = pr_gettaskid(p->pr);
      p->projid = pr_getprojid(p->pr);
      p->zoneid = pr_getzoneid(p->pr);

      break;
    }
    (void) closedir(dirp);

    if (found == 0) {
      warn(gettext("%s: No controllable process found in "
		   "task, project, or zone.\n"), idname);
      return (-1);
    }
    return (p->pid);

  } else if (type == RCENTITY_PROCESS) {

    /* fail if self */
    if (p->pid == pid_self) {

      warn(gettext("%s: cannot control self"), idname);
      return (-1);
    }
    /*
     * Process types need to be set up with the correct pid
     * and psinfo structure.
     */
    if ((p->pid = proc_arg_psinfo(idname, PR_ARG_PIDS,
				  &(p->psinfo), gret)) == -1) {
      warn(gettext("%s: cannot examine: %s"), idname,
	   Pgrab_error(*gret));
      return (-1);
    }
    /* grab process */
    ret = grab_process(p, gret);
    if (ret == 1) {
      /* Don't print error if G_SYS is allowed */
      if (gret_in == G_SYS && *gret == G_SYS) {
	return (-1);
      } else {
	warn(gettext("%s: cannot control: %s"), idname,
	     Pgrab_error(*gret));
	return (-1);
      }
    } else if (ret == 2) {
      ret = errno;
      warn(gettext("%s: cannot control: %s"), idname,
	   strerror(ret));
      return (-1);
    }
    p->taskid = pr_gettaskid(p->pr);
    p->projid = pr_getprojid(p->pr);
    p->zoneid = pr_getzoneid(p->pr);

    return (p->pid);

  } else {
    warn(gettext("%s: unknown resource entity type %d\n"), idname,
	 type);
    return (-1);
  }
}

/*
 * Do the work required to manipulate a process through libproc.
 * If grab_process() returns no errors (0), then release_process()
 * must eventually be called.
 *
 * Return values:
 *      0 Successful creation of agent thread
 *      1 Error grabbing
 *      2 Error creating agent
 */
int
grab_process(pr_info_handle_t *p, int *gret)
{

  if ((p->pr = Pgrab(p->pid, arg_force, gret)) != NULL) {

    if (Psetflags(p->pr, PR_RLC) != 0) {
      Prelease(p->pr, 0);
      return (1);
    }
    if (Pcreate_agent(p->pr) == 0) {
      return (0);

    } else {
      Prelease(p->pr, 0);
      return (2);
    }
  } else {
    return (1);
  }
}

/*
 * Release the specified process. This destroys the agent
 * and releases the process. If the process is NULL, nothing
 * is done. This function should only be called if grab_process()
 * has previously been called and returned success.
 *
 * This function is Pgrab-safe.
 */
void
release_process(struct ps_prochandle *Pr)
{
  if (Pr == NULL)
    return;

  Pdestroy_agent(Pr);
  Prelease(Pr, 0);
}

/*
 * preserve_error(char *, ...)
 *
 * preserve_error() should be called rather than warn() by any
 * function that is called while the victim process is held by Pgrab.
 * It will save the error until the process has been un-controlled
 * and output is reasonable again.
 *
 * Note that multiple errors are not stored. Any error in these
 * sections should be critical and return immediately.
 *
 * This function is Pgrab-safe.
 *
 * Since this function may copy untrusted command line arguments to
 * global_error, security practices require that global_error never be
 * printed directly.  Use printf("%s\n", global_error) or equivalent.
 */
/*PRINTFLIKE1*/
void
preserve_error(char *format, ...)
{
  va_list alist;

  va_start(alist, format);

  /*
   * GLOBAL_ERR_SZ is pretty big. If the error is longer
   * than that, just truncate it, rather than chance missing
   * the error altogether.
   */
  (void) vsnprintf(global_error, GLOBAL_ERR_SZ-1, format, alist);

  va_end(alist);
}
