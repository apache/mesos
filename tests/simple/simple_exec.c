#include <stdio.h>
#include <stdlib.h>

#include "mesos.h"


bool simple_exec_initialize()
{
  printf("simple_exec_initialize\n");
}


bool simple_exec_execute(const struct nimbus_task *task)
{
  printf("simple_exec_execute\n");
}
