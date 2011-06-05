#ifndef MESOS_EXEC_H
#define MESOS_EXEC_H

#include <stdint.h>
#include <mesos.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesos_exec {
  void (*init) (struct mesos_exec*,
                slave_id,
                const char*,  // hostname
                framework_id,
                const char*,  // framework_name
                const void*,  // init_arg
                int);         // init_arg_len
  void (*launch_task) (struct mesos_exec*, struct mesos_task_desc*);
  void (*kill_task) (struct mesos_exec*, task_id tid);
  void (*framework_message) (struct mesos_exec*,
                             struct mesos_framework_message*);
  void (*shutdown) (struct mesos_exec*);
  void (*error) (struct mesos_exec*, int, const char*);

  // Opaque data that can be used to associate extra info with executor
  void* data;
};

int mesos_exec_run(struct mesos_exec*);

int mesos_exec_send_message(struct mesos_exec*,
                            struct mesos_framework_message*);

int mesos_exec_status_update(struct mesos_exec*, struct mesos_task_status*);

#ifdef __cplusplus
}
#endif

#endif /* MESOS_EXEC_H */
