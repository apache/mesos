#ifndef __MESOS_IDS_H__ 
#define __MESOS_IDS_H__ 

#include <stdint.h>

typedef const char *framework_id;
typedef int32_t task_id;
typedef const char *slave_id;
typedef const char *offer_id;
typedef int32_t mesos_handle;

enum task_state {
  TASK_STARTING,
  TASK_RUNNING,
  TASK_FINISHED,
  TASK_FAILED,
  TASK_KILLED,
  TASK_LOST,
};

#endif /* __MESOS_IDS_H__ */
