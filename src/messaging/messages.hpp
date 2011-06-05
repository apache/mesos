#ifndef __MESSAGES_HPP__
#define __MESSAGES_HPP__

#include <float.h>

#include <glog/logging.h>

#include <string>
#include <vector>

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
  PROCESS_TERMINATE,

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

  // From master detector to processes.
  GOT_MASTER_TOKEN,
  NEW_MASTER_DETECTED,
  NO_MASTER_DETECTED,
  MASTER_DETECTION_FAILURE,

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


// Type conversions helpful for changing between protocol buffer types
// and standard C++ types (for parameters).
template <typename T>
const T& convert(const T& t)
{
  return t;
}


template <typename T>
std::vector<T> convert(const google::protobuf::RepeatedPtrField<T>& items)
{
  std::vector<T> result;
  for (int i = 0; i < items.size(); i++) {
    result.push_back(items.Get(i));
  }

  return result;
}


// Mapping between message names to message ids.
extern boost::unordered_map<std::string, MSGID> ids;
extern boost::unordered_map<MSGID, std::string> names;


template <typename T>
class MesosProcess : public process::Process<T>
{
public:
  MesosProcess(const std::string& id = "")
    : process::Process<T>(id) {}

  virtual ~MesosProcess() {}

  static void post(const process::UPID& to, const std::string& name)
  {
    process::post(to, name);
  }

  static void post(const process::UPID& to, MSGID id)
  {
    CHECK(names.count(id) > 0) << "Missing name for MSGID " << id;
    process::post(to, names[id]);
  }

  template <MSGID ID>
  static void post(const process::UPID& to, const MSG<ID>& msg)
  {
    CHECK(names.count(ID) > 0) << "Missing name for MSGID " << ID;
    std::string data;
    msg.SerializeToString(&data);
    process::post(to, names[ID], data.data(), data.size());
  }

protected:
  AnyMessage message() const
  {
    return AnyMessage(process::Process<T>::body());
  }

  MSGID msgid() const
  {
    CHECK(ids.count(process::Process<T>::name()) > 0)
      << "Missing MSGID for '" << process::Process<T>::name() << "'";
    return ids[process::Process<T>::name()];
  }

  void send(const process::UPID& to, const std::string& name)
  {
    process::Process<T>::send(to, name);
  }

  void send(const process::UPID& to, MSGID id)
  {
    CHECK(names.count(id) > 0) << "Missing name for MSGID " << id;
    process::Process<T>::send(to, names[id]);
  }

  template <MSGID ID>
  void send(const process::UPID& to, const MSG<ID>& msg)
  {
    CHECK(names.count(ID) > 0) << "Missing name for MSGID " << ID;
    std::string data;
    msg.SerializeToString(&data);
    process::Process<T>::send(to, names[ID], data.data(), data.size());
  }

  MSGID receive(double secs = 0)
  {
    while (true) {
      process::Process<T>::receive(secs);
      if (ids.count(process::Process<T>::name()) > 0) {
        return ids[process::Process<T>::name()];
      } else {
        LOG(WARNING) << "Dropping unknown message '"
                     << process::Process<T>::name() << "'"
                     << " from: " << process::Process<T>::from()
                     << " to: " << process::Process<T>::self();
      }
    }
  }

  MSGID serve(double secs = 0)
  {
    while (true) {
      process::Process<T>::serve(secs);
      if (ids.count(process::Process<T>::name()) > 0) {
        // Check if this has been bound and invoke the handler.
        if (handlers.count(process::Process<T>::name()) > 0) {
          handlers[process::Process<T>::name()](process::Process<T>::body());
        } else {
          return ids[process::Process<T>::name()];
        }
      } else {
        LOG(WARNING) << "Dropping unknown message '"
                     << process::Process<T>::name() << "'"
                     << " from: " << process::Process<T>::from()
                     << " to: " << process::Process<T>::self();
      }
    }
  }

  void install(MSGID id, void (T::*method)())
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&MesosProcess<T>::handler0, t,
                     method,
                     std::tr1::placeholders::_1);
  }

  template <typename PB,
            typename P1, typename P1C>
  void install(MSGID id, void (T::*method)(P1C),
               P1 (PB::*param1)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&handler1<PB, P1, P1C>, t,
                     method, param1,
                     std::tr1::placeholders::_1);
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C>
  void install(MSGID id, void (T::*method)(P1C, P2C),
               P1 (PB::*p1)() const,
               P2 (PB::*p2)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&handler2<PB, P1, P1C, P2, P2C>, t,
                     method, p1, p2,
                     std::tr1::placeholders::_1);
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C>
  void install(MSGID id,
               void (T::*method)(P1C, P2C, P3C),
               P1 (PB::*p1)() const,
               P2 (PB::*p2)() const,
               P3 (PB::*p3)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&handler3<PB, P1, P1C, P2, P2C, P3, P3C>, t,
                     method, p1, p2, p3,
                     std::tr1::placeholders::_1);
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C>
  void install(MSGID id,
               void (T::*method)(P1C, P2C, P3C, P4C),
               P1 (PB::*p1)() const,
               P2 (PB::*p2)() const,
               P3 (PB::*p3)() const,
               P4 (PB::*p4)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&handler4<PB, P1, P1C, P2, P2C, P3, P3C, P4, P4C>, t,
                     method, p1, p2, p3, p4,
                     std::tr1::placeholders::_1);
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C>
  void install(MSGID id,
               void (T::*method)(P1C, P2C, P3C, P4C, P5C),
               P1 (PB::*p1)() const,
               P2 (PB::*p2)() const,
               P3 (PB::*p3)() const,
               P4 (PB::*p4)() const,
               P5 (PB::*p5)() const)
  {
    T* t = static_cast<T*>(this);
    CHECK(names.count(id) > 0);
    handlers[names[id]] =
      std::tr1::bind(&handler5<PB, P1, P1C, P2, P2C, P3, P3C, P4, P4C, P5, P5C>, t,
                     method, p1, p2, p3, p4, p5,
                     std::tr1::placeholders::_1);
  }

private:
  static void handler0(T* t, void (T::*method)(),
                       const std::string& data)
  {
    (t->*method)();
  }

  template <typename PB,
            typename P1, typename P1C>
  static void handler1(T* t, void (T::*method)(P1C),
                       P1 (PB::*p1)() const,
                       const std::string& data)
  {
    PB pb;
    pb.ParseFromArray(data.data(), data.size());
    if (pb.IsInitialized()) {
      (t->*method)(convert((&pb->*p1)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C>
  static void handler2(T* t, void (T::*method)(P1C, P2C),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       const std::string& data)
  {
    PB pb;
    pb.ParseFromArray(data.data(), data.size());
    if (pb.IsInitialized()) {
      (t->*method)(convert((&pb->*p1)()), convert((&pb->*p2)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C>
  static void handler3(T* t, void (T::*method)(P1C, P2C, P3C),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       P3 (PB::*p3)() const,
                       const std::string& data)
  {
    PB pb;
    pb.ParseFromArray(data.data(), data.size());
    if (pb.IsInitialized()) {
      (t->*method)(convert((&pb->*p1)()), convert((&pb->*p2)()),
                   convert((&pb->*p3)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C>
  static void handler4(T* t, void (T::*method)(P1C, P2C, P3C, P4C),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       P3 (PB::*p3)() const,
                       P4 (PB::*p4)() const,
                       const std::string& data)
  {
    PB pb;
    pb.ParseFromArray(data.data(), data.size());
    if (pb.IsInitialized()) {
      (t->*method)(convert((&pb->*p1)()), convert((&pb->*p2)()),
                   convert((&pb->*p3)()), convert((&pb->*p4)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  template <typename PB,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C>
  static void handler5(T* t, void (T::*method)(P1C, P2C, P3C, P4C, P5C),
                       P1 (PB::*p1)() const,
                       P2 (PB::*p2)() const,
                       P3 (PB::*p3)() const,
                       P4 (PB::*p4)() const,
                       P5 (PB::*p5)() const,
                       const std::string& data)
  {
    PB pb;
    pb.ParseFromArray(data.data(), data.size());
    if (pb.IsInitialized()) {
      (t->*method)(convert((&pb->*p1)()), convert((&pb->*p2)()),
                   convert((&pb->*p3)()), convert((&pb->*p4)()),
                   convert((&pb->*p5)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << pb.InitializationErrorString();
    }
  }

  boost::unordered_map<std::string, std::tr1::function<void (const std::string&)> > handlers;
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

MESSAGE(NEW_MASTER_DETECTED, NewMasterDetectedMessage);
MESSAGE(GOT_MASTER_TOKEN, GotMasterTokenMessage);

}} // namespace mesos { namespace internal {


#endif // __MESSAGES_HPP__
