#ifndef __MESOS_H__
#define __MESOS_H__

#include <unistd.h>

#include <mesos_types.h>


// Note, we use 'const' in our C code because it helps correctness
// and a lot of our C code interacts with C++ code so please use a
// compiler that doesn't suck.


struct mesos_task_desc {
  task_id tid;
  slave_id sid;
  const char *name;
  const char *params;
  const void *arg;
  size_t arg_len;
};

struct mesos_task_status {
  task_id tid;
  enum task_state state;
  const void *data;
  size_t data_len;
};

struct mesos_slot {
  slave_id sid;
  const char *host;
  const char *params;
};

struct mesos_framework_message {
  slave_id sid;
  task_id tid;
  const void *data;
  size_t data_len;
};

int params_get_int(const char *params, const char *key, int default_value);

int32_t params_get_int32(const char *params,
                         const char *key,
                         int32_t default_value);

int64_t params_get_int64(const char *params,
                         const char *key,
                         int64_t default_value);

#endif /* __MESOS_H__ */
