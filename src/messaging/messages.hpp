#ifndef MESSAGES_HPP
#define MESSAGES_HPP

#include <float.h>

#include <string>

#include <mesos.hpp>
#include <process.hpp>

#include "messaging/messages.pb.h"


namespace mesos { namespace internal {


// TODO(benh): Eliminate versioning once message ids become strings.
const std::string MESOS_MESSAGING_VERSION = "2";


enum MessageType {
  /* From framework to master. */
  F2M_REGISTER_FRAMEWORK = PROCESS_MSGID,
  F2M_REREGISTER_FRAMEWORK,
  F2M_UNREGISTER_FRAMEWORK,
  F2M_RESOURCE_OFFER_REPLY,
  F2M_REVIVE_OFFERS,
  F2M_KILL_TASK,
  F2M_FRAMEWORK_MESSAGE,
  
  /* From master to framework. */
  M2F_REGISTER_REPLY,
  M2F_RESOURCE_OFFER,
  M2F_RESCIND_OFFER,
  M2F_STATUS_UPDATE,
  M2F_LOST_SLAVE,
  M2F_FRAMEWORK_MESSAGE,
  M2F_ERROR,
  
  /* From slave to master. */
  S2M_REGISTER_SLAVE,
  S2M_REREGISTER_SLAVE,
  S2M_UNREGISTER_SLAVE,
  S2M_STATUS_UPDATE,
  S2M_FRAMEWORK_MESSAGE,
  S2M_EXITED_EXECUTOR,

  /* From slave heart to master. */
  SH2M_HEARTBEAT,

  /* From master detector to processes */
  GOT_MASTER_TOKEN,
  NEW_MASTER_DETECTED,
  NO_MASTER_DETECTED,
  
  /* From master to slave. */
  M2S_REGISTER_REPLY,
  M2S_REREGISTER_REPLY,
  M2S_RUN_TASK,
  M2S_KILL_TASK,
  M2S_KILL_FRAMEWORK,
  M2S_FRAMEWORK_MESSAGE,
  M2S_UPDATE_FRAMEWORK,
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

  /* Internal to framework. */
  F2F_RESOURCE_OFFER_REPLY,
  F2F_FRAMEWORK_MESSAGE,

  /* Internal to master. */
  M2M_GET_STATE,         // Used by web UI
  M2M_GET_STATE_REPLY,
  M2M_TIMER_TICK,        // Timer for expiring filters etc
  M2M_FRAMEWORK_EXPIRED, // Timer for expiring frameworks
  M2M_SHUTDOWN,          // Used in tests to shut down master

  /* Internal to slave. */
  S2S_GET_STATE,         // Used by web UI
  S2S_GET_STATE_REPLY,
  S2S_SHUTDOWN,          // Used in tests to shut down slave

  /* Generic. */
  TERMINATE,

  // TODO(benh): Put these all in their right place.
  MASTER_DETECTION_FAILURE, 
  F2M_STATUS_UPDATE_ACK,
  M2S_STATUS_UPDATE_ACK,

  MESOS_MSGID,
};


/**
 * To couple a MSGID with a protocol buffer we use a templated class
 * that extends the necessary protocol buffer type (this also allows
 * the code to be better isolated from protocol buffer naming). While
 * protocol buffers are allegedly not meant to be inherited, we
 * decided this was an acceptable option since we don't add any new
 * functionality (or do any thing with the existing functionality).
 *
 * To add another message that uses a protocol buffer you need to
 * provide a specialization of the Message class (i.e., using the
 * MESSAGE macro defined below).
 */
template <MSGID ID>
class Message;


#define MESSAGE(ID, T)                          \
  template <>                                   \
  class Message<ID> : public T {}


class AnyMessage
{
public:
  AnyMessage(const std::string& data_)
    : data(data_) {}

  template <MSGID ID>
  operator Message<ID> () const
  {
    Message<ID> msg;
    msg.ParseFromString(data);
    return msg;
  }

private:
  std::string data;
};


class MesosProcess : public Process
{
public:
  static void post(const PID &to, MSGID id)
  {
    const std::string &data = MESOS_MESSAGING_VERSION + "|";
    Process::post(to, id, data.data(), data.size());
  }

  template <MSGID ID>
  static void post(const PID &to, const Message<ID> &msg)
  {
    std::string data;
    msg.SerializeToString(&data);
    data = MESOS_MESSAGING_VERSION + "|" + data;
    Process::post(to, ID, data.data(), data.size());
  }

protected:
  AnyMessage message() const
  {
    return AnyMessage(body());
  }

  std::string body() const
  {
    size_t size;
    const char *s = Process::body(&size);
    const std::string data(s, size);
    size_t index = data.find('|');
    CHECK(index != std::string::npos);
    return data.substr(index + 1);
  }

  virtual void send(const PID &to, MSGID id)
  {
    const std::string &data = MESOS_MESSAGING_VERSION + "|";
    Process::send(to, id, data.data(), data.size());
  }

  template <MSGID ID>
  void send(const PID &to, const Message<ID> &msg)
  {
    std::string data;
    msg.SerializeToString(&data);
    data = MESOS_MESSAGING_VERSION + "|" + data;
    Process::send(to, ID, data.data(), data.size());
  }

  virtual MSGID receive(double secs = 0)
  {
    bool indefinite = secs == 0;
    double now = elapsed();
    MSGID id = Process::receive(secs);
    if (PROCESS_MSGID < id && id < MESOS_MSGID) {
      size_t size;
      const char *s = Process::body(&size);
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
#endif /* __sun__ */

MESSAGE(M2M_GET_STATE_REPLY, StateMessage);
MESSAGE(M2M_FRAMEWORK_EXPIRED, FrameworkExpiredMessage);

MESSAGE(S2S_GET_STATE_REPLY, StateMessage);

MESSAGE(NEW_MASTER_DETECTED, NewMasterDetectedMessage);
MESSAGE(GOT_MASTER_TOKEN, GotMasterTokenMessage);

}} /* namespace mesos { namespace internal { */


#endif /* MESSAGES_HPP */
