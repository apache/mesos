#include <stdio.h>
#include <stdlib.h>

#include "nimbus.h"


struct nimbus_task * simple_sched_schedule(struct nimbus_slot *slot)
{
  printf("simple_sched_schedule\n");
  return NULL;
}


void simple_sched_status(struct nimbus_task *task)
{
  printf("simple_sched_status\n");
}
