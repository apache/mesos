/* Mesos. */

#include <stdint.h>
#include <mesos.h>

#ifndef MESOS_SCHED_H
#define MESOS_SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

struct mesos_sched {
  // Human-readable framework name
  const char* framework_name;
  
  // Executor library name
  const char* executor_name;
  
  // Callbacks
  void (*registered) (struct mesos_sched*, framework_id);
  void (*slot_offer) (struct mesos_sched*,
                      offer_id,
                      struct mesos_slot*,
                      int);
  void (*slot_offer_rescinded) (struct mesos_sched*, offer_id);
  void (*status_update) (struct mesos_sched*, struct mesos_task_status*);
  void (*framework_message) (struct mesos_sched*,
                             struct mesos_framework_message*);
  void (*slave_lost) (struct mesos_sched*, slave_id);
  void (*error) (struct mesos_sched*, int, const char*);
  
  // Executor init argument
  void* init_arg;
  size_t init_arg_len;
  
  // Opaque data that can be used to associate extra info with the scheduler
  void* data;
};

int mesos_sched_init(struct mesos_sched*);
int mesos_sched_destroy(struct mesos_sched*);

// Register a scheduler, connecting to a given URL
int mesos_sched_reg(struct mesos_sched*, const char* url);

// Register a scheduler, connecting to the master URL specified through the
// given options string (which should contain key=value pairs, one per line).
int mesos_sched_reg_with_params(struct mesos_sched*, const char* params);

// Register a scheduler, connecting to the master URL specified through the
// given command line arguments. Note that argv[0] is expected to be the
// program name and is therefore ignored by Mesos.
int mesos_sched_reg_with_cmdline(struct mesos_sched*, int argc, char** argv);

int mesos_sched_unreg(struct mesos_sched*);

int mesos_sched_send_message(struct mesos_sched*,
                             struct mesos_framework_message*);

int mesos_sched_kill_task(struct mesos_sched*, task_id);

int mesos_sched_reply_to_offer(struct mesos_sched*,
                               offer_id,
                               struct mesos_task_desc*,
                               int,
                               const char*);

int mesos_sched_revive_offers(struct mesos_sched*);

int mesos_sched_join(struct mesos_sched*);

#ifdef __cplusplus
}
#endif

#endif /* MESOS_SCHED_H */
