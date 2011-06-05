#ifndef MESSAGES_HPP
#define MESSAGES_HPP

#include <float.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include <reliable.hpp>

#include <tuples/tuples.hpp>

#include <mesos.hpp>
#include <mesos_types.hpp>

#include "common/foreach.hpp"
#include "common/logging.hpp"
#include "common/params.hpp"
#include "common/resources.hpp"
#include "common/task.hpp"

#include "master/state.hpp"

#include "slave/state.hpp"


namespace mesos { namespace internal {

const std::string MESOS_MESSAGING_VERSION = "0";

enum MessageType {
  /* From framework to master. */
  F2M_REGISTER_FRAMEWORK = RELIABLE_MSGID,
  F2M_REREGISTER_FRAMEWORK,
  F2M_UNREGISTER_FRAMEWORK,
  F2M_SLOT_OFFER_REPLY,
  F2M_REVIVE_OFFERS,
  F2M_KILL_TASK,
  F2M_FRAMEWORK_MESSAGE,

  F2F_SLOT_OFFER_REPLY,
  F2F_FRAMEWORK_MESSAGE,
  F2F_TASK_RUNNING_STATUS,
  
  /* From master to framework. */
  M2F_REGISTER_REPLY,
  M2F_SLOT_OFFER,
  M2F_RESCIND_OFFER,
  M2F_STATUS_UPDATE,
  M2F_FT_STATUS_UPDATE,
  M2F_LOST_SLAVE,
  M2F_FRAMEWORK_MESSAGE,
  M2F_ERROR,
  
  /* From slave to master. */
  S2M_REGISTER_SLAVE,
  S2M_REREGISTER_SLAVE,
  S2M_UNREGISTER_SLAVE,
  S2M_STATUS_UPDATE,
  S2M_FT_STATUS_UPDATE,
  S2M_FRAMEWORK_MESSAGE,
  S2M_LOST_EXECUTOR,

  /* From slave heart to master. */
  SH2M_HEARTBEAT,

  /* From master detector to processes */
  GOT_MASTER_ID,
  NEW_MASTER_DETECTED,
  NO_MASTER_DETECTED,
  
  /* From master to slave. */
  M2S_REGISTER_REPLY,
  M2S_REREGISTER_REPLY,
  M2S_RUN_TASK,
  M2S_KILL_TASK,
  M2S_KILL_FRAMEWORK,
  M2S_FRAMEWORK_MESSAGE,
  M2S_UPDATE_FRAMEWORK_PID,
  M2S_SHUTDOWN, // Used in unit tests to shut down cluster

  /* From executor to slave. */
  E2S_REGISTER_EXECUTOR,
  E2S_STATUS_UPDATE,
  E2S_FRAMEWORK_MESSAGE,

  /* From slave to executor. */
  S2E_REGISTER_REPLY,
  S2E_RUN_TASK,
  S2E_KILL_TASK,
  S2E_FRAMEWORK_MESSAGE,
  S2E_KILL_EXECUTOR,

#ifdef __sun__
  /* From projd to slave. */
  PD2S_REGISTER_PROJD,
  PD2S_PROJECT_READY,

  /* From slave to projd. */
  S2PD_UPDATE_RESOURCES,
  S2PD_KILL_ALL,
#endif /* __sun__ */

  /* Internal to master */
  M2M_GET_STATE,         // Used by web UI
  M2M_GET_STATE_REPLY,
  M2M_TIMER_TICK,        // Timer for expiring filters etc
  M2M_FRAMEWORK_EXPIRED, // Timer for expiring frameworks
  M2M_SHUTDOWN,          // Used in tests to shut down master

  /* Internal to slave */
  S2S_GOT_MASTER,        // Used when looking up master with ZooKeeper
  S2S_GET_STATE,         // Used by web UI
  S2S_GET_STATE_REPLY,
  S2S_CHILD_EXIT,        // Sent by reaper process
  S2S_SHUTDOWN,          // Used in tests to shut down slave

  MESOS_MSGID,
};


/*
 * Include tuples details for our namespace (this strategy is
 * typically called "supermacros" and is often used to build types or
 * messages).
 */
#include <tuples/details.hpp>


class MesosProcess : public ReliableProcess
{
public:
  template <MSGID ID>
  static void post(const PID &to, const tuple<ID> &t)
  {
    const std::string &data = MESOS_MESSAGING_VERSION + "|" + std::string(t);
    ReliableProcess::post(to, ID, data.data(), data.size());
  }

protected:
  std::string body() const
  {
    size_t size;
    const char *s = ReliableProcess::body(&size);
    const std::string data(s, size);
    size_t index = data.find('|');
    CHECK(index != std::string::npos);
    return data.substr(index + 1);
  }

  template <MSGID ID>
  void send(const PID &to, const tuple<ID> &t)
  {
    const std::string &data = MESOS_MESSAGING_VERSION + "|" + std::string(t);
    ReliableProcess::send(to, ID, data.data(), data.size());
  }

  template <MSGID ID>
  int rsend(const PID &to, const tuple<ID> &t)
  {
    const std::string &data = MESOS_MESSAGING_VERSION + "|" + std::string(t);
    return ReliableProcess::rsend(to, ID, data.data(), data.size());
  }

  virtual MSGID receive() { return receive(0); }

  virtual MSGID receive(double secs)
  {
    bool indefinite = secs == 0;
    double now = elapsed();
    MSGID id = ReliableProcess::receive(secs);
    if (RELIABLE_MSGID < id && id < MESOS_MSGID) {
      size_t size;
      const char *s = ReliableProcess::body(&size);
      const std::string data(s, size);
      size_t index = data.find('|');
      if (index == std::string::npos ||
          MESOS_MESSAGING_VERSION != data.substr(0, index)) {
        LOG(ERROR) << "Dropping message from " << from()
                   << " with incorrect messaging version!";
        if (!indefinite) {
          double remaining = secs - (elapsed() - now);
          return receive(remaining <= 0 ? DBL_EPSILON : remaining);
        } else {
          return receive(0);
        }
      }
    }
    return id;
  }
};


using boost::tuples::tie;



TUPLE(F2M_REGISTER_FRAMEWORK,
      (std::string /*name*/,
       std::string /*user*/,
       ExecutorInfo));

TUPLE(F2M_REREGISTER_FRAMEWORK,
      (FrameworkID,
       std::string /*name*/,
       std::string /*user*/,
       ExecutorInfo,
       int32_t /*generation*/));

TUPLE(F2M_UNREGISTER_FRAMEWORK,
      (FrameworkID));

TUPLE(F2M_SLOT_OFFER_REPLY,
      (FrameworkID,
       OfferID,
       std::vector<TaskDescription>,
       Params));

TUPLE(F2M_REVIVE_OFFERS,
      (FrameworkID));

TUPLE(F2M_KILL_TASK,
      (FrameworkID,
       TaskID));

TUPLE(F2M_FRAMEWORK_MESSAGE,
      (FrameworkID,
       FrameworkMessage));

TUPLE(F2F_SLOT_OFFER_REPLY,
      (OfferID,
       std::vector<TaskDescription>,
       Params));

TUPLE(F2F_FRAMEWORK_MESSAGE,
      (FrameworkMessage));

TUPLE(F2F_TASK_RUNNING_STATUS,
      ());

TUPLE(M2F_REGISTER_REPLY,
      (FrameworkID));

TUPLE(M2F_SLOT_OFFER,
      (OfferID,
       std::vector<SlaveOffer>,
       std::map<SlaveID, PID>));

TUPLE(M2F_RESCIND_OFFER,
      (OfferID));

TUPLE(M2F_STATUS_UPDATE,
      (TaskID,
       TaskState,
       std::string));

TUPLE(M2F_FT_STATUS_UPDATE,
      (TaskID,
       TaskState,
       std::string));

TUPLE(M2F_LOST_SLAVE,
      (SlaveID));

TUPLE(M2F_FRAMEWORK_MESSAGE,
      (FrameworkMessage));

TUPLE(M2F_ERROR,
      (int32_t /*code*/,
       std::string /*msg*/));


TUPLE(S2M_REGISTER_SLAVE,
      (std::string /*name*/,
       std::string /*publicDns*/,
       Resources));

TUPLE(S2M_REREGISTER_SLAVE,
      (SlaveID,
       std::string /*name*/,
       std::string /*publicDns*/,
       Resources,
       std::vector<Task>));

TUPLE(S2M_UNREGISTER_SLAVE,
      (SlaveID));

TUPLE(S2M_STATUS_UPDATE,
      (SlaveID,
       FrameworkID,
       TaskID,
       TaskState,
       std::string));

TUPLE(S2M_FT_STATUS_UPDATE,
      (SlaveID,
       FrameworkID,
       TaskID,
       TaskState,
       std::string));

TUPLE(S2M_FRAMEWORK_MESSAGE,
      (SlaveID,
       FrameworkID,
       FrameworkMessage));

TUPLE(S2M_LOST_EXECUTOR,
      (SlaveID,
       FrameworkID,
       int32_t /*exitStatus*/));

TUPLE(SH2M_HEARTBEAT,
      (SlaveID));
    
TUPLE(NEW_MASTER_DETECTED,
      (std::string, /* master seq */
       PID /* master PID */));

TUPLE(NO_MASTER_DETECTED,
      ());

TUPLE(GOT_MASTER_ID,
      (std::string /* id */));
  
TUPLE(M2S_REGISTER_REPLY,
      (SlaveID,
       double /*heartbeat interval*/));

TUPLE(M2S_REREGISTER_REPLY,
      (SlaveID,
       double /*heartbeat interval*/));

TUPLE(M2S_RUN_TASK,
      (FrameworkID,
       TaskID,
       std::string /*frameworkName*/,
       std::string /*user*/,
       ExecutorInfo,
       std::string /*taskName*/,
       std::string /*taskArgs*/,
       Params,
       PID /*framework PID*/));

TUPLE(M2S_KILL_TASK,
      (FrameworkID,
       TaskID));

TUPLE(M2S_KILL_FRAMEWORK,
      (FrameworkID));

TUPLE(M2S_FRAMEWORK_MESSAGE,
      (FrameworkID,
       FrameworkMessage));

TUPLE(M2S_UPDATE_FRAMEWORK_PID,
      (FrameworkID,
       PID));

TUPLE(M2S_SHUTDOWN,
      ());

TUPLE(E2S_REGISTER_EXECUTOR,
      (FrameworkID));

TUPLE(E2S_STATUS_UPDATE,
      (FrameworkID,
       TaskID,
       TaskState,
       std::string));

TUPLE(E2S_FRAMEWORK_MESSAGE,
      (FrameworkID,
       FrameworkMessage));

TUPLE(S2E_REGISTER_REPLY,
      (SlaveID,
       std::string /*hostname*/,
       std::string /*frameworkName*/,
       std::string /*initArg*/));

TUPLE(S2E_RUN_TASK,
      (TaskID,
       std::string /*name*/,
       std::string /*arg*/,
       Params));

TUPLE(S2E_KILL_TASK,
      (TaskID));

TUPLE(S2E_FRAMEWORK_MESSAGE,
      (FrameworkMessage));

TUPLE(S2E_KILL_EXECUTOR,
      ());

#ifdef __sun__
TUPLE(PD2S_REGISTER_PROJD,
      (std::string /*project*/));

TUPLE(PD2S_PROJECT_READY,
      (std::string /*project*/));

TUPLE(S2PD_UPDATE_RESOURCES,
      (Resources));

TUPLE(S2PD_KILL_ALL,
      ());
#endif /* __sun__ */

TUPLE(M2M_GET_STATE,
      ());

TUPLE(M2M_GET_STATE_REPLY,
      (master::state::MasterState *));

TUPLE(M2M_TIMER_TICK,
      ());

TUPLE(M2M_FRAMEWORK_EXPIRED,
      (FrameworkID));

TUPLE(M2M_SHUTDOWN,
      ());

TUPLE(S2S_GOT_MASTER,
      ());

TUPLE(S2S_GET_STATE,
      ());

TUPLE(S2S_GET_STATE_REPLY,
      (slave::state::SlaveState *));

TUPLE(S2S_CHILD_EXIT,
      (int32_t /*OS PID*/,
       int32_t /*exitStatus*/));

TUPLE(S2S_SHUTDOWN,
      ());


/* Serialization functions for sharing objects of local Mesos types. */

void operator & (process::tuples::serializer&, const master::state::MasterState *);
void operator & (process::tuples::deserializer&, master::state::MasterState *&);

void operator & (process::tuples::serializer&, const slave::state::SlaveState *);
void operator & (process::tuples::deserializer&, slave::state::SlaveState *&);


/* Serialization functions for various Mesos data types. */

void operator & (process::tuples::serializer&, const TaskState&);
void operator & (process::tuples::deserializer&, TaskState&);

void operator & (process::tuples::serializer&, const SlaveOffer&);
void operator & (process::tuples::deserializer&, SlaveOffer&);

void operator & (process::tuples::serializer&, const TaskDescription&);
void operator & (process::tuples::deserializer&, TaskDescription&);

void operator & (process::tuples::serializer&, const FrameworkMessage&);
void operator & (process::tuples::deserializer&, FrameworkMessage&);

void operator & (process::tuples::serializer&, const ExecutorInfo&);
void operator & (process::tuples::deserializer&, ExecutorInfo&);

void operator & (process::tuples::serializer&, const Params&);
void operator & (process::tuples::deserializer&, Params&);

void operator & (process::tuples::serializer&, const Resources&);
void operator & (process::tuples::deserializer&, Resources&);

void operator & (process::tuples::serializer&, const Task&);
void operator & (process::tuples::deserializer&, Task&);


/* Serialization functions for STL vectors. */

template<typename T>
void operator & (process::tuples::serializer& s, const std::vector<T>& v)
{
  int32_t size = (int32_t) v.size();
  s & size;
  for (size_t i = 0; i < size; i++) {
    s & v[i];
  }
}


template<typename T>
void operator & (process::tuples::deserializer& d, std::vector<T>& v)
{
  int32_t size;
  d & size;
  v.resize(size);
  for (size_t i = 0; i < size; i++) {
    d & v[i];
  }
}


/* Serialization functions for STL maps. */

template<typename K, typename V>
void operator & (process::tuples::serializer& s, const std::map<K, V>& m)
{
  int32_t size = (int32_t) m.size();
  s & size;
  foreachpair (const K& k, const V& v, m) {
    s & k;
    s & v;
  }
}


template<typename K, typename V>
void operator & (process::tuples::deserializer& d, std::map<K, V>& m)
{
  m.clear();
  int32_t size;
  d & size;
  K k;
  V v;
  for (size_t i = 0; i < size; i++) {
    d & k;
    d & v;
    m[k] = v;
  }
}


}} /* namespace mesos { namespace internal { */


#endif /* MESSAGES_HPP */
