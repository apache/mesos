#ifndef NEXUS_EXEC_H
#define NEXUS_EXEC_H

#include <stdint.h>
#include <nexus.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nexus_exec {
  void (*init) (struct nexus_exec*,
                slave_id,
                framework_id,
                const char*,  // framework_name
                const void*,  // init_arg
                int);         // init_arg_len
  void (*run) (struct nexus_exec*, struct nexus_task_desc*);
  void (*kill) (struct nexus_exec*, task_id tid);
  void (*message) (struct nexus_exec*, struct nexus_framework_message*);
  void (*shutdown) (struct nexus_exec*);
  void (*error) (struct nexus_exec*, int, const char*);

  // Opaque data that can be used to associate extra info with executor
  void* data;
};

int nexus_exec_run(struct nexus_exec*);

int nexus_exec_send_message(struct nexus_exec*,
                            struct nexus_framework_message*);

int nexus_exec_status_update(struct nexus_exec*, struct nexus_task_status*);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_EXEC_H */
