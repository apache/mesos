#include <rctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>

#include "libproc.h"


void print_rctl(rctlblk_t *rblk)
{
  int tmp;
  int sig;

  printf ("  Process ID: %ld\n", (long) rctlblk_get_recipient_pid (rblk));

  printf ("  Privilege: ");
  switch (rctlblk_get_privilege (rblk)) {
  case RCPRIV_BASIC:
    printf ("RCPRIV_BASIC\n");
    break;

  case RCPRIV_PRIVILEGED:
    printf ("RCPRIV_PRIVILEGED\n");
    break;

  case RCPRIV_SYSTEM:
    printf ("RCPRIV_SYSTEM\n");
    break;

  default:
    printf ("Unknown privilege\n");
    break;
  }

  printf ("  Value: %llu\n", rctlblk_get_value (rblk));
  printf ("  Enforced value: %llu\n", rctlblk_get_enforced_value (rblk));

  printf ("  Global flags: ");
  tmp = rctlblk_get_global_flags (rblk);
  if (tmp & RCTL_GLOBAL_DENY_ALWAYS)
    printf ("RCTL_GLOBAL_DENY_ALWAYS ");
  if (tmp & RCTL_GLOBAL_DENY_NEVER)
    printf ("RCTL_GLOBAL_DENY_NEVER ");
  if (tmp & RCTL_GLOBAL_CPU_TIME)
    printf ("RCTL_GLOBAL_CPU_TIME ");
  if (tmp & RCTL_GLOBAL_FILE_SIZE)
    printf ("RCTL_GLOBAL_FILE_SIZE ");
  if (tmp & RCTL_GLOBAL_INFINITE)
    printf ("RCTL_GLOBAL_INFINITE ");
  if (tmp & RCTL_GLOBAL_LOWERABLE)
    printf ("RCTL_GLOBAL_LOWERABLE ");
  if (tmp & RCTL_GLOBAL_NOBASIC)
    printf ("RCTL_GLOBAL_NOBASIC ");
  if (tmp & RCTL_GLOBAL_NOLOCALACTION)
    printf ("RCTL_GLOBAL_NOLOCALACTION ");
  if (tmp & RCTL_GLOBAL_UNOBSERVABLE)
    printf ("RCTL_GLOBAL_UNOBSERVABLE ");
  printf ("\n");

  printf ("  Global actions: ");
  tmp = rctlblk_get_global_action (rblk);
  if (tmp & RCTL_GLOBAL_NOACTION)
    printf ("RCTL_GLOBAL_NOACTION ");
  if (tmp & RCTL_GLOBAL_SYSLOG)
    printf ("RCTL_GLOBAL_SYSLOG ");
  printf ("\n");

  printf ("  Local flags: ");
  tmp = rctlblk_get_local_flags (rblk);
  if (tmp & RCTL_LOCAL_MAXIMAL)
    printf ("RCTL_LOCAL_MAXIMAL ");
  printf ("\n");

  printf ("  Local actions: ");
  tmp = rctlblk_get_local_action (rblk, &sig);
  if (tmp & RCTL_LOCAL_DENY)
    printf ("RCTL_LOCAL_DENY ");
  if (tmp & RCTL_LOCAL_NOACTION)
    printf ("RCTL_LOCAL_NOACTION ");
  if (tmp & RCTL_LOCAL_SIGNAL) {
    printf ("RCTL_LOCAL_SIGNAL ");
    switch (sig) {
    case SIGABRT:
      printf ("(SIGABRT)");
      break;

    case SIGXRES:
      printf ("(SIGXRES)");
      break;

    case SIGHUP:
      printf ("(SIGHUP)");
      break;

    case SIGSTOP:
      printf ("(SIGSTOP)");
      break;

    case SIGTERM:
      printf ("(SIGTERM)");
      break;

    case SIGKILL:
      printf ("(SIGKILL)");
      break;

    case SIGXCPU:
      printf ("(SIGXCPU)");
      break;

    case SIGXFSZ:
      printf ("(SIGXFSZ)");
      break;

    default:
      printf ("(Illegal signal)");
      break;
    }
  }
  printf ("\n");

  printf ("  Firing time: %llu\n\n", rctlblk_get_firing_time (rblk));
}


int print_rctls(struct ps_prochandle *pr, const char *name)
{
  rctlblk_t *rblk;

  if ((rblk = malloc(rctlblk_size())) == NULL)
    return (-1);

  if (pr_getrctl(pr, name, NULL, rblk, RCTL_FIRST) == -1)
    return (-1);

  printf("%s:\n", name);
  print_rctl(rblk);

  while (pr_getrctl(pr, name, rblk, rblk, RCTL_NEXT) != -1)
    print_rctl(rblk);

  free(rblk);

  return (0);
}


int set_cpu_shares(struct ps_prochandle *pr, pid_t pid, int shares)
{
  rctlblk_t *rblk_old, *rblk_new;

  if ((rblk_old = malloc(rctlblk_size())) == NULL)
    return (-1);

  if ((rblk_new = malloc(rctlblk_size())) == NULL) {
    free(rblk_old);
    return (-1);
  }

  if (pr_getrctl(pr, "project.cpu-shares", NULL, rblk_old, RCTL_FIRST) == -1) {
    free(rblk_old);
    free(rblk_new);
    return (-1);
  }

  bcopy(rblk_old, rblk_new, rctlblk_size());

  rctlblk_set_value(rblk_new, shares);

  if (pr_setrctl(pr, "project.cpu-shares", rblk_old, rblk_new,
	      RCTL_REPLACE) != 0)
    return (-1);

  free(rblk_old);
  free(rblk_new);

  return (0);
}


int main(int argc, char **argv)
{
  if (argc != 3) {
    printf("usage: %s pid value\n", argv[0]);
    return -1;
  }

  struct ps_prochandle *pr;

  int gret;

  if ((pr = Pgrab(atoi(argv[1]), 0, &gret)) != NULL) {
    if (Psetflags(pr, PR_RLC) != 0) {
      Prelease(pr, 0);
      return 2;
    }
    if (Pcreate_agent(pr) == 0) {
      if (set_cpu_shares(pr, atoi(argv[1]), atoi(argv[2])) == 0)
	print_rctls(pr, "project.cpu-shares");
      Pdestroy_agent(pr);
      Prelease(pr, 0);
      return 0;
    } else {
      Prelease(pr, 0);
      return 3;
    }
  } else {
    Pgrab_error(gret);
    return 1;
  }

  return 0;
}
