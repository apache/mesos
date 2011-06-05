#ifndef __MESSAGES_HPP__
#define __MESSAGES_HPP__

#include <float.h>

#include <glog/logging.h>

#include <string>

#include <tr1/functional>

#include <mesos.hpp>
#include <process.hpp>

#include <boost/unordered_map.hpp>

#include "messaging/messages.pb.h"


namespace mesos { namespace internal {

enum MSGID {
  // Artifacts from libprocess.
  PROCESS_TIMEOUT,
  PROCESS_EXIT,

  // From framework to master.
  F2M_REGISTER_FRAMEWORK,
  F2M_REREGISTER_FRAMEWORK,
  F2M_UNREGISTER_FRAMEWORK,
  F2M_RESOURCE_OFFER_REPLY,
  F2M_REVIVE_OFFERS,
  F2M_KILL_TASK,
  F2M_FRAMEWORK_MESSAGE,
  F2M_STATUS_UPDATE_ACK,
  
  // From master to framework.
  M2F_REGISTER_REPLY,
  M2F_RESOURCE_OFFER,
  M2F_RESCIND_OFFER,
  M2F_STATUS_UPDATE,
  M2F_LOST_SLAVE,
  M2F_FRAMEWORK_MESSAGE,
  M2F_ERROR,
  
  // From slave to master.
  S2M_REGISTER_SLAVE,
  S2M_REREGISTER_SLAVE,
  S2M_UNREGISTER_SLAVE,
  S2M_STATUS_UPDATE,
  S2M_FRAMEWORK_MESSAGE,
  S2M_EXITED_EXECUTOR,

  // From slave heart to master.
  SH2M_HEARTBEAT,
  
  // From master to slave.
  M2S_REGISTER_REPLY,
  M2S_REREGISTER_REPLY,
  M2S_RUN_TASK,
  M2S_KILL_TASK,
  M2S_KILL_FRAMEWORK,
  M2S_FRAMEWORK_MESSAGE,
  M2S_UPDATE_FRAMEWORK,
  M2S_STATUS_UPDATE_ACK,
  M2S_SHUTDOWN, // Used in unit tests to shut down cluster

  // From executor to slave.
  E2S_REGISTER_EXECUTOR,
  E2S_STATUS_UPDATE,
  E2S_FRAMEWORK_MESSAGE,

  // From slave to executor.
  S2E_REGISTER_REPLY,
  S2E_RUN_TASK,
  S2E_KILL_TASK,
  S2E_FRAMEWORK_MESSAGE,
  S2E_KILL_EXECUTOR,

#ifdef __sun__
  // From projd to slave.
  PD2S_REGISTER_PROJD,
  PD2S_PROJECT_READY,

  // From slave to projd.
  S2PD_UPDATE_RESOURCES,
  S2PD_KILL_ALL,
#endif // __sun__

  // Internal to master.
  M2M_GET_STATE,         // Used by web UI
  M2M_GET_STATE_REPLY,
  M2M_TIMER_TICK,        // Timer for expiring filters etc
  M2M_FRAMEWORK_EXPIRED, // Timer for expiring frameworks
  M2M_SHUTDOWN,          // Used in tests to shut down master

  // Internal to slave.
  S2S_GET_STATE,         // Used by web UI
  S2S_GET_STATE_REPLY,
  S2S_SHUTDOWN,          // Used in tests to shut down slave

  // From master detector to processes.
  GOT_MASTER_TOKEN,
  NEW_MASTER_DETECTED,
  NO_MASTER_DETECTED,
  MASTER_DETECTION_FAILURE,

  // HTTP messages.
  vars,

  MESOS_MSGID
};


// To couple a MSGID with a protocol buffer we use a templated class
// that extends the necessary protocol buffer type (this also allows
// the code to be better isolated from protocol buffer naming). While
// protocol buffers are allegedly not meant to be inherited, we
// decided this was an acceptable option since we don't add any new
// functionality (or do any thing with the existing functionality).
//
// To add another message that uses a protocol buffer you need to
// provide a specialization of the Message class (i.e., using the
// MESSAGE macro defined below).
template <MSGID ID>
class MSG;

#define MESSAGE(ID, T)                          \
  template <>                                   \
  class MSG<ID> : public T {}


class AnyMessage
{
public:
  AnyMessage(const std::string& data_)
    : data(data_) {}

  template <MSGID ID>
  operator MSG<ID> () const
  {
    MSG<ID> msg;
    msg.ParseFromString(data);
    return msg;
  }

private:
  std::string data;
};


class MesosProcess : public Process
{
public:
  MesosProcess(const std::string& id = "") : Process(id) {}

  virtual ~MesosProcess() {}

  static void post(const PID &to, MSGID id)
  {
    CHECK(names.count(id) > 0) << "Missing name for MSGID " << id;
    Process::post(to, names[id]);
  }

  template <MSGID ID>
  static void post(const PID &to, const MSG<ID> &msg)
  {
    CHECK(names.count(ID) > 0) << "Missing name for MSGID " << ID;
    std::string data;
    msg.SerializeToString(&data);
    Process::post(to, names[ID], data.data(), data.size());
  }

  static boost::unordered_map<std::string, MSGID> ids;
  static boost::unordered_map<MSGID, std::string> names;

protected:
  AnyMessage message() const
  {
    return AnyMessage(body());
  }

  MSGID msgid() const
  {
    CHECK(ids.count(name()) > 0) << "Missing MSGID for '" << name() << "'";
    return ids[name()];
  }

  std::string body() const
  {
    size_t size;
    const char *data = Process::body(&size);
    return std::string(data, size);
  }

  void send(const PID &to, MSGID id)
  {
    CHECK(names.count(id) > 0) << "Missing name for MSGID " << id;
    Process::send(to, names[id]);
  }

  template <MSGID ID>
  void send(const PID &to, const MSG<ID> &msg)
  {
    CHECK(names.count(ID) > 0) << "Missing name for MSGID " << ID;
    std::string data;
    msg.SerializeToString(&data);
    Process::send(to, names[ID], data.data(), data.size());
  }

  MSGID receive(double secs = 0)
  {
    while (true) {
      Process::receive(secs);
      if (ids.count(name()) > 0) {
        // Check if this has been bound and invoke the handler.
        if (handlers.count(name()) > 0) {
          size_t length;
          const char* data = Process::body(&length);
          handlers[name()](data, length);
        } else {
          return ids[name()];
        }
      } else {
        LOG(WARNING) << "Dropping unknown message '" << name() << "'"
                     << " from: " << from() << " to: " << self();
      }
    }
  }

  MSGID serve(double secs = 0)
  {
    while (true) {
      Process::serve(secs);
      if (ids.count(name()) > 0) {
        // Check if this has been bound and invoke the handler.
        if (handlers.count(name()) > 0) {
          size_t length;
          const char* data = Process::body(&length);
          handlers[name()](data, length);
        } else {
          return ids[name()];
        }
      } else {
        LOG(WARNING) << "Dropping unknown message '" << name() << "'"
                     << " from: " << from() << " to: " << self();
      }
    }
  }

  template <typename T>
  void handle(MSGID id, void (T::*method)())
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&MesosProcess::handler0<T>, t,
                     method, std::tr1::placeholders::_1,
                     std::tr1::placeholders::_2);
  }

  template <typename T,
            typename PB,
            typename P1>
  void handle(MSGID id, void (T::*method)(P1),
              P1 (PB::*param1)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&MesosProcess::handler1<T, PB, P1>, t,
                     method, param1,
                     std::tr1::placeholders::_1,
                     std::tr1::placeholders::_2);
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2>
  void handle(MSGID id, void (T::*method)(P1, P2),
              P1 (PB::*p1)() const,
              P2 (PB::*p2)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&MesosProcess::handler2<T, PB, P1, P2>, t,
                     method, p1, p2,
                     std::tr1::placeholders::_1,
                     std::tr1::placeholders::_2);
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2,
            typename P3>
  void handle(MSGID id,
              void (T::*method)(P1, P2, P3),
              P1 (PB::*p1)() const,
              P2 (PB::*p2)() const,
              P3 (PB::*p3)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&MesosProcess::handler3<T, PB, P1, P2, P3>, t,
                     method, p1, p2, p3,
                     std::tr1::placeholders::_1,
                     std::tr1::placeholders::_2);
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2,
            typename P3,
            typename P4>
  void handle(MSGID id,
              void (T::*method)(P1, P2, P3, P4),
              P1 (PB::*p1)() const,
              P2 (PB::*p2)() const,
              P3 (PB::*p3)() const,
              P4 (PB::*p4)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&MesosProcess::handler4<T, PB, P1, P2, P3, P4>, t,
                     method, p1, p2, p3, p4,
                     std::tr1::placeholders::_1,
                     std::tr1::placeholders::_2);
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2,
            typename P3,
            typename P4,
            typename P5>
  void handle(MSGID id,
              void (T::*method)(P1, P2, P3, P4, P5),
              P1 (PB::*p1)() const,
              P2 (PB::*p2)() const,
              P3 (PB::*p3)() const,
              P4 (PB::*p4)() const,
              P5 (PB::*p5)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&MesosProcess::handler5<T, PB, P1, P2, P3, P4, P5>, t,
                     method, p1, p2, p3, p4, p5,
                     std::tr1::placeholders::_1,
                     std::tr1::placeholders::_2);
  }

private:
  template <typename T>
  static void handler0(T* t, void (T::*method)(),
                       const char* data, size_t length)
  {
    (t->*method)();
  }

  template <typename T,
            typename PB,
            typename P1>
  static void handler1(T* t, void (T::*method)(P1),
                       P1 (PB::*p1)() const,
                       const char* data, size_t length)
  {
    PB pb;
    pb.ParseFromArray(data, length);
    if (pb.IsInitialized()) {
      (t->*method)((&pb->*p1)());
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2>
  static void handler2(T* t, void (T::*method)(P1, P2),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       const char* data, size_t length)
  {
    PB pb;
    pb.ParseFromArray(data, length);
    if (pb.IsInitialized()) {
      (t->*method)((&pb->*p1)(), (&pb->*p2)());
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2,
            typename P3>
  static void handler3(T* t, void (T::*method)(P1, P2, P3),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       P3 (PB::*p3)() const,
                       const char* data, size_t length)
  {
    PB pb;
    pb.ParseFromArray(data, length);
    if (pb.IsInitialized()) {
      (t->*method)((&pb->*p1)(), (&pb->*p2)(), (&pb->*p3)());
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2,
            typename P3,
            typename P4>
  static void handler4(T* t, void (T::*method)(P1, P2, P3, P4),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       P3 (PB::*p3)() const,
                       P4 (PB::*p4)() const,
                       const char* data, size_t length)
  {
    PB pb;
    pb.ParseFromArray(data, length);
    if (pb.IsInitialized()) {
      (t->*method)((&pb->*p1)(), (&pb->*p2)(), (&pb->*p3)(), (&pb->*p4)());
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename T,
            typename PB,
            typename P1,
            typename P2,
            typename P3,
            typename P4,
            typename P5>
  static void handler5(T* t, void (T::*method)(P1, P2, P3, P4, P5),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       P3 (PB::*p3)() const,
                       P4 (PB::*p4)() const,
                       P5 (PB::*p5)() const,
                       const char* data, size_t length)
  {
    PB pb;
    pb.ParseFromArray(data, length);
    if (pb.IsInitialized()) {
      (t->*method)((&pb->*p1)(), (&pb->*p2)(), (&pb->*p3)(), (&pb->*p4)(),
                   (&pb->*p5)());
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  boost::unordered_map<std::string, std::tr1::function<void (const char*, size_t)> > handlers;
};


MESSAGE(F2M_REGISTER_FRAMEWORK, RegisterFrameworkMessage);
MESSAGE(F2M_REREGISTER_FRAMEWORK, ReregisterFrameworkMessage);
MESSAGE(F2M_UNREGISTER_FRAMEWORK, UnregisterFrameworkMessage);
MESSAGE(F2M_RESOURCE_OFFER_REPLY, ResourceOfferReplyMessage);
MESSAGE(F2M_REVIVE_OFFERS, ReviveOffersMessage);
MESSAGE(F2M_KILL_TASK, KillTaskMessage);
MESSAGE(F2M_FRAMEWORK_MESSAGE, FrameworkMessageMessage);
MESSAGE(F2M_STATUS_UPDATE_ACK, StatusUpdateAckMessage);

MESSAGE(M2F_REGISTER_REPLY, FrameworkRegisteredMessage);
MESSAGE(M2F_RESOURCE_OFFER, ResourceOfferMessage);
MESSAGE(M2F_RESCIND_OFFER, RescindResourceOfferMessage);
MESSAGE(M2F_STATUS_UPDATE, StatusUpdateMessage);
MESSAGE(M2F_LOST_SLAVE, LostSlaveMessage);
MESSAGE(M2F_FRAMEWORK_MESSAGE, FrameworkMessageMessage);
MESSAGE(M2F_ERROR, FrameworkErrorMessage);

MESSAGE(S2M_REGISTER_SLAVE, RegisterSlaveMessage);
MESSAGE(S2M_REREGISTER_SLAVE, ReregisterSlaveMessage);
MESSAGE(S2M_UNREGISTER_SLAVE, UnregisterSlaveMessage);
MESSAGE(S2M_STATUS_UPDATE, StatusUpdateMessage);
MESSAGE(S2M_FRAMEWORK_MESSAGE, FrameworkMessageMessage);
MESSAGE(S2M_EXITED_EXECUTOR, ExitedExecutorMessage);

MESSAGE(SH2M_HEARTBEAT, HeartbeatMessage);
  
MESSAGE(M2S_REGISTER_REPLY, SlaveRegisteredMessage);
MESSAGE(M2S_REREGISTER_REPLY, SlaveRegisteredMessage);
MESSAGE(M2S_RUN_TASK, RunTaskMessage);
MESSAGE(M2S_KILL_TASK, KillTaskMessage);
MESSAGE(M2S_KILL_FRAMEWORK, KillFrameworkMessage);
MESSAGE(M2S_FRAMEWORK_MESSAGE, FrameworkMessageMessage);
MESSAGE(M2S_UPDATE_FRAMEWORK, UpdateFrameworkMessage);
MESSAGE(M2S_STATUS_UPDATE_ACK, StatusUpdateAckMessage);

MESSAGE(E2S_REGISTER_EXECUTOR, RegisterExecutorMessage);
MESSAGE(E2S_STATUS_UPDATE, StatusUpdateMessage);
MESSAGE(E2S_FRAMEWORK_MESSAGE, FrameworkMessageMessage);

MESSAGE(S2E_REGISTER_REPLY, ExecutorRegisteredMessage);
MESSAGE(S2E_RUN_TASK, RunTaskMessage);
MESSAGE(S2E_KILL_TASK, KillTaskMessage);
MESSAGE(S2E_FRAMEWORK_MESSAGE, FrameworkMessageMessage);

#ifdef __sun__
MESSAGE(PD2S_REGISTER_PROJD, RegisterProjdMessage);
MESSAGE(PD2S_PROJD_READY, ProjdReadyMessage);
MESSAGE(S2PD_UPDATE_RESOURCES, ProjdUpdateResourcesMessage);
#endif // __sun__

MESSAGE(M2M_GET_STATE_REPLY, StateMessage);
MESSAGE(M2M_FRAMEWORK_EXPIRED, FrameworkExpiredMessage);

MESSAGE(S2S_GET_STATE_REPLY, StateMessage);

MESSAGE(NEW_MASTER_DETECTED, NewMasterDetectedMessage);
MESSAGE(GOT_MASTER_TOKEN, GotMasterTokenMessage);

}} // namespace mesos { namespace internal {


#endif // __MESSAGES_HPP__
