#ifndef MESSAGES_HPP
#define MESSAGES_HPP

#include <map>
#include <string>
#include <vector>

#include <tuple.hpp>

#include <nexus.hpp>
#include <nexus_types.hpp>

#include "params.hpp"
#include "resources.hpp"
#include "foreach.hpp"
#include "task.hpp"


namespace nexus { namespace internal {

enum MessageType {
  /* From framework to master. */
  F2M_REGISTER_FRAMEWORK = PROCESS_MSGID,
  F2M_REREGISTER_FRAMEWORK,
  F2M_UNREGISTER_FRAMEWORK,
  F2M_SLOT_OFFER_REPLY,
  F2M_FT_SLOT_OFFER_REPLY,
  F2M_REVIVE_OFFERS,
  F2M_KILL_TASK,
  F2M_FRAMEWORK_MESSAGE,
  F2M_FT_FRAMEWORK_MESSAGE,

  F2F_SLOT_OFFER_REPLY,
  F2F_FRAMEWORK_MESSAGE,
  
  /* From master to framework. */
  M2F_REGISTER_REPLY,
  M2F_SLOT_OFFER,
  M2F_RESCIND_OFFER,
  M2F_STATUS_UPDATE,
  M2F_FT_STATUS_UPDATE,
  M2F_LOST_SLAVE,
  M2F_FRAMEWORK_MESSAGE,
  M2F_FT_FRAMEWORK_MESSAGE,
  M2F_ERROR,
  
  /* Used for FT. */
  FT_RELAY_ACK,
  FT_ACK,

  /* From slave to master. */
  S2M_REGISTER_SLAVE,
  S2M_REREGISTER_SLAVE,
  S2M_UNREGISTER_SLAVE,
  S2M_STATUS_UPDATE,
  S2M_FT_STATUS_UPDATE,
  S2M_FRAMEWORK_MESSAGE,
  S2M_FT_FRAMEWORK_MESSAGE,
  S2M_LOST_EXECUTOR,

  /* From slave heart to master. */
  SH2M_HEARTBEAT,

  /* From master detector to processes */
  NEW_MASTER_DETECTED,
  NO_MASTER_DETECTED,
  GOT_MASTER_SEQ,
  
  /* From master to slave. */
  M2S_REGISTER_REPLY,
  M2S_REREGISTER_REPLY,
  M2S_RUN_TASK,
  M2S_KILL_TASK,
  M2S_KILL_FRAMEWORK,
  M2S_FRAMEWORK_MESSAGE,
  M2S_FT_FRAMEWORK_MESSAGE,
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
  M2M_GET_STATE,        // Used by web UI
  M2M_GET_STATE_REPLY,
  M2M_TIMER_TICK,       // Timer for expiring filters etc
  M2M_SHUTDOWN,         // Used in tests to shut down master

  /* Internal to slave */
  S2S_GOT_MASTER,       // Used when looking up master with ZooKeeper
  S2S_GET_STATE,        // Used by web UI
  S2S_GET_STATE_REPLY,
  S2S_CHILD_EXIT,       // Sent by reaper process
  S2S_SHUTDOWN,         // Used in tests to shut down slave

  NEXUS_MESSAGES,
};


/*
 * To use the tuple message send/receive/pack/unpack routines, we need
 * to include the implementation and type definitions of the tuple
 * routines for our namespace (strategy is typically called
 * "supermacros", used to build new types).
 */
#include <tuple-impl.hpp>


TUPLE(F2M_REGISTER_FRAMEWORK,
      (std::string /*name*/,
       std::string /*user*/,
       ExecutorInfo));

TUPLE(F2M_REREGISTER_FRAMEWORK,
      (std::string /*fid*/,
       std::string /*name*/,
       std::string /*user*/,
       ExecutorInfo));

TUPLE(F2M_UNREGISTER_FRAMEWORK,
      (FrameworkID));

TUPLE(F2M_SLOT_OFFER_REPLY,
      (FrameworkID,
       OfferID,
       std::vector<TaskDescription>,
       Params));

TUPLE(F2M_FT_SLOT_OFFER_REPLY,
      (std::string, /* FT ID */
       std::string, /* original sender */
       FrameworkID,
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

TUPLE(F2M_FT_FRAMEWORK_MESSAGE,
      (std::string, /* FT ID */
       std::string, /* original sender */
       FrameworkID,
       FrameworkMessage));


TUPLE(F2F_SLOT_OFFER_REPLY,
      (OfferID,
       std::vector<TaskDescription>,
       Params));

TUPLE(F2F_FRAMEWORK_MESSAGE,
      (FrameworkMessage));



TUPLE(M2F_REGISTER_REPLY,
      (FrameworkID));

TUPLE(M2F_SLOT_OFFER,
      (OfferID,
       std::vector<SlaveOffer>));

TUPLE(M2F_RESCIND_OFFER,
      (OfferID));

TUPLE(M2F_STATUS_UPDATE,
      (TaskID,
       TaskState,
       std::string));

TUPLE(M2F_FT_STATUS_UPDATE,
      (std::string, /* FT ID */
       std::string, /* original sender */
       TaskID,
       TaskState,
       std::string));

TUPLE(M2F_LOST_SLAVE,
      (SlaveID));

TUPLE(M2F_FRAMEWORK_MESSAGE,
      (FrameworkMessage));

TUPLE(M2F_FT_FRAMEWORK_MESSAGE,
      (std::string, /* FT ID */
       std::string, /* original sender */
       FrameworkMessage));

TUPLE(M2F_ERROR,
      (int32_t /*code*/,
       std::string /*msg*/));


TUPLE(FT_RELAY_ACK,
      (std::string, /* FT ID */
       std::string /* PID of orig */
       ));

TUPLE(S2M_REGISTER_SLAVE,
      (std::string /*name*/,
       std::string /*publicDns*/,
       Resources));

TUPLE(S2M_REREGISTER_SLAVE,
      (std::string /*slave id*/,
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
      (std::string, /* unique msgId */
       std::string, /* senders PID */
       SlaveID,
       FrameworkID,
       TaskID,
       TaskState,
       std::string));

TUPLE(S2M_FRAMEWORK_MESSAGE,
      (SlaveID,
       FrameworkID,
       FrameworkMessage));

TUPLE(S2M_FT_FRAMEWORK_MESSAGE,
      (std::string, /* ftId */
       std::string, /* sender PID */
       SlaveID,
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

TUPLE(GOT_MASTER_SEQ,
      (std::string /* seq */));
  
TUPLE(M2S_REGISTER_REPLY,
      (SlaveID));

TUPLE(M2S_REREGISTER_REPLY,
      (SlaveID));

TUPLE(M2S_RUN_TASK,
      (FrameworkID,
       TaskID,
       std::string /*frameworkName*/,
       std::string /*user*/,
       ExecutorInfo,
       std::string /*taskName*/,
       std::string /*taskArgs*/,
       Params,
       std::string /*framework PID*/));

TUPLE(M2S_KILL_TASK,
      (FrameworkID,
       TaskID));

TUPLE(M2S_KILL_FRAMEWORK,
      (FrameworkID));

TUPLE(M2S_FRAMEWORK_MESSAGE,
      (FrameworkID,
       FrameworkMessage));
        
TUPLE(M2S_FT_FRAMEWORK_MESSAGE,
      (std::string, /* FT ID */
       std::string, /* original sender */
       FrameworkID,
       FrameworkMessage));

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
      (int64_t /* TODO(benh): BUG ON 32 BIT! ... pointer to MasterState*/));

TUPLE(M2M_TIMER_TICK,
      ());
       
TUPLE(M2M_SHUTDOWN,
      ());

TUPLE(S2S_GOT_MASTER,
      ());

TUPLE(S2S_GET_STATE,
      ());

TUPLE(S2S_GET_STATE_REPLY,
      (int64_t /* TODO(benh): BUG ON 32 BIT! ... pointer to SlaveState*/));

TUPLE(S2S_CHILD_EXIT,
      (int32_t /*OS PID*/,
       int32_t /*exitStatus*/));

TUPLE(S2S_SHUTDOWN,
      ());


/* Serialization functions for various Nexus data types. */

void operator & (process::serialization::serializer&, const TaskState&);
void operator & (process::serialization::deserializer&, TaskState&);

void operator & (process::serialization::serializer&, const SlaveOffer&);
void operator & (process::serialization::deserializer&, SlaveOffer&);

void operator & (process::serialization::serializer&, const TaskDescription&);
void operator & (process::serialization::deserializer&, TaskDescription&);

void operator & (process::serialization::serializer&, const FrameworkMessage&);
void operator & (process::serialization::deserializer&, FrameworkMessage&);

void operator & (process::serialization::serializer&, const ExecutorInfo&);
void operator & (process::serialization::deserializer&, ExecutorInfo&);

void operator & (process::serialization::serializer&, const Params&);
void operator & (process::serialization::deserializer&, Params&);

void operator & (process::serialization::serializer&, const Resources&);
void operator & (process::serialization::deserializer&, Resources&);

void operator & (process::serialization::serializer&, const Task&);
void operator & (process::serialization::deserializer&, Task&);


/* Serialization functions for STL vectors (TODO(benh): move to libprocess). */

template<typename T>
void operator & (process::serialization::serializer& s, const std::vector<T>& v)
{
  int32_t size = (int32_t) v.size();
  s & size;
  for (size_t i = 0; i < size; i++) {
    s & v[i];
  }
}


template<typename T>
void operator & (process::serialization::deserializer& s, std::vector<T>& v)
{
  int32_t size;
  s & size;
  v.resize(size);
  for (size_t i = 0; i < size; i++) {
    s & v[i];
  }
}


/* Serialization functions for STL maps (TODO(benh): move to libprocess). */

template<typename K, typename V>
void operator & (process::serialization::serializer& s,
		 const std::map<K, V>& m)
{
  int32_t size = (int32_t) m.size();
  s & size;
  foreachpair (const K& k, const V& v, m) {
    s & k;
    s & v;
  }
}


template<typename K, typename V>
void operator & (process::serialization::deserializer& s, std::map<K, V>& m)
{
  m.clear();
  int32_t size;
  s & size;
  K k;
  V v;
  for (size_t i = 0; i < size; i++) {
    s & k;
    s & v;
    m[k] = v;
  }
}


}} /* namespace nexus { namespace internal { */


#endif /* MESSAGES_HPP */
