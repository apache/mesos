#include "nimbus.h"


int main(int argc, char **argv)
{
  struct nimbus_sched_desc sched = { "simple_sched" };
  struct nimbus_exec_desc exec = { "simple_exec" };
  struct nimbus_framework fw = { &sched, &exec };

  nimbus_register(&fw);

  struct nimbus_job job;
  nimbus_start(&fw, &job);
  nimbus_wait(&fw, &job);

  nimbus_unregister(&fw);
}
