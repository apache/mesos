// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <queue>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/v1/scheduler.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/unreachable.hpp>

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "jvm/jvm.hpp"

#include "master/constants.hpp"
#include "master/validation.hpp"

#include "convert.hpp"
#include "construct.hpp"
#include "org_apache_mesos_v1_scheduler_V0Mesos.h"

using namespace mesos;
using namespace mesos::internal::master;

using std::queue;
using std::string;
using std::vector;

using mesos::internal::devolve;
using mesos::internal::evolve;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

using process::Clock;
using process::Owned;
using process::Timer;

class V0ToV1AdapterProcess; // Forward declaration.

// This interface acts as an adapter from the v0 (driver + scheduler) to the
// v1 Mesos scheduler.
class V0ToV1Adapter : public mesos::Scheduler, public v1::scheduler::MesosBase
{
public:
  V0ToV1Adapter(
      JNIEnv* env,
      jweak jmesos,
      const FrameworkInfo& frameworkInfo,
      const string& master,
      const Option<Credential>& credential);

  ~V0ToV1Adapter() override;

  // v0 Scheduler interface overrides.
  void registered(
      SchedulerDriver* driver,
      const FrameworkID& frameworkId,
      const MasterInfo& masterInfo) override;

  void reregistered(
      SchedulerDriver* driver,
      const MasterInfo& masterInfo) override;

  void disconnected(SchedulerDriver* driver) override;

  void resourceOffers(
      SchedulerDriver* driver,
      const vector<Offer>& offers) override;

  void offerRescinded(
      SchedulerDriver* driver,
      const OfferID& offerId) override;

  void statusUpdate(
      SchedulerDriver* driver,
      const TaskStatus& status) override;

  void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data) override;

  void slaveLost(
      SchedulerDriver* driver,
      const SlaveID& slaveId) override;

  void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int status) override;

  void error(
      SchedulerDriver* driver,
      const string& message) override;

  // v1 MesosBase interface overrides.
  void send(const v1::scheduler::Call& call) override;

  void reconnect() override
  {
    // The driver does not support explicit reconnection with the master.
    UNREACHABLE();
  }

  process::Future<v1::scheduler::APIResult> call(
      const v1::scheduler::Call& callMessage) override
  {
    // The driver does not support sending a `v1::scheduler::Call` that returns
    // a `v1::scheduler::Response`.
    UNREACHABLE();
  }

  process::Owned<V0ToV1AdapterProcess> process;

private:
  Owned<MesosSchedulerDriver> driver;
};


// The process (below) is responsible for ensuring synchronized access between
// callbacks received from the driver and calls invoked by the adapter.
class V0ToV1AdapterProcess : public process::Process<V0ToV1AdapterProcess>
{
public:
  V0ToV1AdapterProcess(JNIEnv* env, jweak jmesos);

  ~V0ToV1AdapterProcess() override = default;

  void registered(const FrameworkID& frameworkId, const MasterInfo& masterInfo);

  void reregistered(const MasterInfo& masterInfo);

  void disconnected();

  void resourceOffers(const vector<Offer>& offers);

  void offerRescinded(const OfferID& offerId);

  void statusUpdate(const TaskStatus& status);

  void frameworkMessage(
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data);

  void slaveLost(const SlaveID& slaveId);

  void executorLost(
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int status);

  void error(const string& message);

  void send(SchedulerDriver*, const v1::scheduler::Call& call);

  JavaVM* jvm;
  JNIEnv* env;
  jweak jmesos;

protected:
  void received(const Event& event);

  void _received();

  void __received(const Event& event);

  void connect();

  void heartbeat();

  void disconnect();

private:
  bool subscribeCall;
  const Duration interval;
  queue<Event> pending;
  Option<FrameworkID> frameworkId;
  Option<Timer> heartbeatTimer;
};


V0ToV1Adapter::V0ToV1Adapter(
    JNIEnv* env,
    jweak jmesos,
    const FrameworkInfo& frameworkInfo,
    const string& master,
    const Option<Credential>& credential)
  : process(new V0ToV1AdapterProcess(env, jmesos))
{
  spawn(process.get());

  driver.reset(
      credential.isSome()
        // Disable implicit acks.
        ? new MesosSchedulerDriver(
              this, frameworkInfo, master, false, credential.get())
        : new MesosSchedulerDriver(this, frameworkInfo, master, false));

  driver->start();
}


V0ToV1Adapter::~V0ToV1Adapter()
{
  terminate(process.get());
  wait(process.get());
}


void V0ToV1Adapter::error(
    SchedulerDriver*,
    const string& message)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::error, message);
}


void V0ToV1Adapter::executorLost(
    SchedulerDriver*,
    const ExecutorID& executorId,
    const SlaveID& slaveId,
    int status)
{
  process::dispatch(
      process.get(),
      &V0ToV1AdapterProcess::executorLost,
      executorId,
      slaveId,
      status);
}


void V0ToV1Adapter::slaveLost(
    SchedulerDriver*,
    const SlaveID& slaveId)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::slaveLost, slaveId);
}


void V0ToV1Adapter::frameworkMessage(
    SchedulerDriver*,
    const ExecutorID& executorId,
    const SlaveID& slaveId,
    const string& data)
{
  process::dispatch(
      process.get(),
      &V0ToV1AdapterProcess::frameworkMessage,
      executorId,
      slaveId,
      data);
}


void V0ToV1Adapter::statusUpdate(
    SchedulerDriver*,
    const TaskStatus& status)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::statusUpdate, status);
}


void V0ToV1Adapter::offerRescinded(
    SchedulerDriver*,
    const OfferID& offerId)
{
  process::dispatch(
      process.get(), &V0ToV1AdapterProcess::offerRescinded, offerId);
}


void V0ToV1Adapter::resourceOffers(
    SchedulerDriver*,
    const vector<Offer>& offers)
{
  process::dispatch(
      process.get(), &V0ToV1AdapterProcess::resourceOffers, offers);
}


void V0ToV1Adapter::registered(
    SchedulerDriver*,
    const FrameworkID &frameworkId,
    const MasterInfo& masterInfo)
{
  process::dispatch(
      process.get(),
      &V0ToV1AdapterProcess::registered,
      frameworkId,
      masterInfo);
}


void V0ToV1Adapter::reregistered(
    SchedulerDriver*,
    const MasterInfo& masterInfo)
{
  process::dispatch(
      process.get(), &V0ToV1AdapterProcess::reregistered, masterInfo);
}


void V0ToV1Adapter::disconnected(SchedulerDriver*)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::disconnected);
}


void V0ToV1Adapter::send(const Call& call)
{
  process::dispatch(
      process.get(), &V0ToV1AdapterProcess::send, driver.get(), call);
}


V0ToV1AdapterProcess::V0ToV1AdapterProcess(
    JNIEnv* _env,
    jweak _jmesos)
  : ProcessBase(process::ID::generate("SchedulerV0ToV1Adapter")),
    jvm(nullptr),
    env(_env),
    jmesos(_jmesos),
    subscribeCall(false),
    interval(DEFAULT_HEARTBEAT_INTERVAL)
{
  env->GetJavaVM(&jvm);
}


void V0ToV1AdapterProcess::registered(
    const FrameworkID& _frameworkId,
    const MasterInfo& masterInfo)
{
  LOG(INFO) << "Registered with the Mesos master; invoking connected callback";

  connect();

  // We need this copy to populate the fields in `Event::Subscribed` upon
  // receiving a `reregistered()` callback later.
  frameworkId = _frameworkId;

  // These events are queued and delivered to the scheduler upon receiving the
  // subscribe call later. See comments in `send()` for more details.
  {
    Event event;
    event.set_type(Event::SUBSCRIBED);

    Event::Subscribed* subscribed = event.mutable_subscribed();

    subscribed->mutable_framework_id()->CopyFrom(evolve(frameworkId.get()));
    subscribed->set_heartbeat_interval_seconds(interval.secs());
    subscribed->mutable_master_info()->CopyFrom(evolve(masterInfo));

    received(event);
  }

  {
    Event event;
    event.set_type(Event::HEARTBEAT);

    received(event);
  }
}


void V0ToV1AdapterProcess::reregistered(const MasterInfo& masterInfo)
{
  CHECK_SOME(frameworkId);
  registered(frameworkId.get(), masterInfo);
}


void V0ToV1AdapterProcess::disconnected()
{
  // Upon noticing a disconnection with the master, we drain the pending
  // events in the queue that were waiting to be sent to the scheduler
  // upon receiving the subscribe call.
  // It's fine to do so because:
  // - Any outstanding offers are invalidated by the master upon a scheduler
  //   (re-)registration.
  // - Any task status updates could be reconciled by the scheduler.
  LOG(INFO) << "Dropping " << pending.size() << " pending event(s)"
            << " because master disconnected";

  pending = queue<Event>();
  subscribeCall = false;

  if (heartbeatTimer.isSome()) {
    Clock::cancel(heartbeatTimer.get());
    heartbeatTimer = None();
  }

  LOG(INFO) << "Disconnected with the Mesos master;"
            << " invoking disconnected callback";

  disconnect();
}


void V0ToV1AdapterProcess::resourceOffers(const vector<Offer>& _offers)
{
  Event event;
  event.set_type(Event::OFFERS);

  Event::Offers* offers = event.mutable_offers();

  foreach (const Offer& offer, _offers) {
    offers->add_offers()->CopyFrom(evolve(offer));
  }

  received(event);
}


void V0ToV1AdapterProcess::offerRescinded(const OfferID& offerId)
{
  Event event;
  event.set_type(Event::RESCIND);

  event.mutable_rescind()->mutable_offer_id()->
    CopyFrom(evolve(offerId));

  received(event);
}


void V0ToV1AdapterProcess::statusUpdate(const TaskStatus& status)
{
  Event event;
  event.set_type(Event::UPDATE);

  event.mutable_update()->mutable_status()->
    CopyFrom(mesos::internal::evolve(status));

  received(event);
}


void V0ToV1AdapterProcess::frameworkMessage(
    const ExecutorID& executorId,
    const SlaveID& slaveId,
    const string& data)
{
  Event event;
  event.set_type(Event::MESSAGE);

  event.mutable_message()->mutable_agent_id()->
    CopyFrom(mesos::internal::evolve(slaveId));

  event.mutable_message()->mutable_executor_id()->
    CopyFrom(mesos::internal::evolve(executorId));

  event.mutable_message()->set_data(data.data());

  received(event);
}


void V0ToV1AdapterProcess::slaveLost(const SlaveID& slaveId)
{
  Event event;
  event.set_type(Event::FAILURE);

  event.mutable_failure()->mutable_agent_id()->
    CopyFrom(mesos::internal::evolve(slaveId));

  received(event);
}


void V0ToV1AdapterProcess::executorLost(
    const ExecutorID& executorId,
    const SlaveID& slaveId,
    int status)
{
  Event event;
  event.set_type(Event::FAILURE);

  event.mutable_failure()->mutable_agent_id()->
    CopyFrom(mesos::internal::evolve(slaveId));

  event.mutable_failure()->mutable_executor_id()->
    CopyFrom(mesos::internal::evolve(executorId));

  event.mutable_failure()->set_status(status);

  received(event);
}


void V0ToV1AdapterProcess::error(const string& message)
{
  Event event;
  event.set_type(Event::ERROR);

  event.mutable_error()->set_message(message);

  // There might be an error during the communication with the master or
  // implicit registration happening on driver initialization. Since
  // `Scheduler.connect` is called upon a successful registration only, the
  // scheduler will never try to subscribe and hence will never receive the
  // error. This workaround satisfies the invariant of the v1 interface that
  // a scheduler can receive an event only after successfully connecting with
  // the master.
  if (!subscribeCall) {
    LOG(INFO) << "Implicitly connecting the scheduler to send an error";
    connect();
  }

  received(event);
}


void V0ToV1AdapterProcess::send(SchedulerDriver* driver, const Call& _call)
{
  CHECK_NOTNULL(driver);

  scheduler::Call call = devolve(_call);

  Option<Error> error = validation::scheduler::call::validate(call);
  if (error.isSome()) {
    LOG(WARNING) << "Dropping " << call.type() << ": due to error "
                 << error->message;
    return;
  }

  switch (call.type()) {
    case scheduler::Call::SUBSCRIBE: {
      subscribeCall = true;

      heartbeatTimer = process::delay(interval, self(), &Self::heartbeat);

      // The driver subscribes implicitly with the master upon initialization.
      // For compatibility with the v1 interface, send the already enqueued
      // subscribed event (or subscription error) upon receiving the subscribe
      // request.
      _received();
      break;
    }

    case scheduler::Call::TEARDOWN: {
      driver->stop(false);
      break;
    }

    case scheduler::Call::ACCEPT: {
      vector<OfferID> offerIds;
      foreach (const OfferID& offerId, call.accept().offer_ids()) {
        offerIds.emplace_back(offerId);
      }

      vector<Offer::Operation> operations;
      foreach (const Offer::Operation& operation, call.accept().operations()) {
        operations.emplace_back(operation);
      }

      if (call.accept().has_filters()) {
        driver->acceptOffers(offerIds, operations, call.accept().filters());
      } else {
        driver->acceptOffers(offerIds, operations);
      }

      break;
    }

    case scheduler::Call::ACCEPT_INVERSE_OFFERS:
    case scheduler::Call::DECLINE_INVERSE_OFFERS:
    case scheduler::Call::SHUTDOWN:
    case scheduler::Call::UPDATE_FRAMEWORK: {
      // TODO(anand): Throw java error.
      LOG(ERROR) << "Received an unexpected " << call.type() << " call";
      break;
    }

    case scheduler::Call::DECLINE: {
      foreach (const OfferID& offerId, call.decline().offer_ids()) {
        if (call.decline().has_filters()) {
          driver->declineOffer(offerId, call.decline().filters());
        } else {
          driver->declineOffer(offerId);
        }
      }

      break;
    }

    case scheduler::Call::REVIVE: {
      driver->reviveOffers();
      break;
    }

    case scheduler::Call::KILL: {
      driver->killTask(call.kill().task_id());
      break;
    }

    case scheduler::Call::ACKNOWLEDGE: {
      TaskStatus status;
      status.mutable_task_id()->CopyFrom(call.acknowledge().task_id());
      status.mutable_slave_id()->CopyFrom(call.acknowledge().slave_id());
      status.set_uuid(call.acknowledge().uuid());

      driver->acknowledgeStatusUpdate(status);
      break;
    }

    // TODO(greggomann): Implement operation status acknowledgement.
    case scheduler::Call::ACKNOWLEDGE_OPERATION_STATUS:
      break;

    case scheduler::Call::RECONCILE: {
      vector<TaskStatus> statuses;

      foreach (const scheduler::Call::Reconcile::Task& task,
               call.reconcile().tasks()) {
        TaskStatus status;
        status.mutable_task_id()->CopyFrom(task.task_id());
        statuses.emplace_back(status);
      }

      driver->reconcileTasks(statuses);
      break;
    }

    // TODO(greggomann): Implement operation reconciliation.
    case scheduler::Call::RECONCILE_OPERATIONS:
      break;

    case scheduler::Call::MESSAGE: {
      driver->sendFrameworkMessage(
          call.message().executor_id(),
          call.message().slave_id(),
          string(call.message().data()));
      break;
    }

    case scheduler::Call::REQUEST: {
      vector<Request> requests;

      foreach (const Request& request, call.request().requests()) {
        requests.emplace_back(request);
      }

      driver->requestResources(requests);
      break;
    }

    case scheduler::Call::SUPPRESS: {
      driver->suppressOffers();
      break;
    }

    case scheduler::Call::UNKNOWN: {
      EXIT(EXIT_FAILURE) << "Received an unexpected " << call.type()
                         << " call";
      break;
    }
  }
}


void V0ToV1AdapterProcess::received(const Event& event)
{
  // For compatibility with the v1 interface, we only start sending events
  // once the scheduler has sent the subscribe call. An exception to this
  // is an error event, which can be sent before the subscribe call.
  if (!subscribeCall) {
    pending.push(event);
    return;
  }

  pending.push(event);

  _received();
}


void V0ToV1AdapterProcess::_received()
{
  CHECK(subscribeCall);

  while (!pending.empty()) {
    __received(pending.front());
    pending.pop();
  }
}


void V0ToV1AdapterProcess::__received(const Event& event)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jmesos);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler",
                    "Lorg/apache/mesos/v1/scheduler/Scheduler;");

  jobject jscheduler = env->GetObjectField(jmesos, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.received(mesos, event);
  jmethodID received =
    env->GetMethodID(clazz, "received",
                     "(Lorg/apache/mesos/v1/scheduler/Mesos;"
                     "Lorg/apache/mesos/v1/scheduler/Protos$Event;)V");

  jobject jevent = convert<Event>(env, event);

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, received, jmesos, jevent);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    ABORT("Exception thrown during `received` call");
  }

  jvm->DetachCurrentThread();
}


void V0ToV1AdapterProcess::connect()
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jmesos);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler",
                    "Lorg/apache/mesos/v1/scheduler/Scheduler;");

  jobject jscheduler = env->GetObjectField(jmesos, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.connected(mesos);
  jmethodID connected =
    env->GetMethodID(clazz, "connected",
                     "(Lorg/apache/mesos/v1/scheduler/Mesos;)V");

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, connected, jmesos);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    ABORT("Exception thrown during `connected` call");
  }

  jvm->DetachCurrentThread();
}


void V0ToV1AdapterProcess::heartbeat()
{
  // It is possible that we were unable to cancel this timer upon a
  // disconnection. If this occurs, don't bother sending the heartbeat
  // event.
  if (heartbeatTimer.isNone() || !heartbeatTimer->timeout().expired()) {
    return;
  }

  CHECK(subscribeCall)
    << "Cannot send heartbeat events to the scheduler without receiving a "
    << "subscribe call";

  Event event;
  event.set_type(Event::HEARTBEAT);

  received(event);

  heartbeatTimer = process::delay(interval, self(), &Self::heartbeat);
}


void V0ToV1AdapterProcess::disconnect()
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jmesos);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler",
                    "Lorg/apache/mesos/v1/scheduler/Scheduler;");

  jobject jscheduler = env->GetObjectField(jmesos, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.disconnected(mesos);
  jmethodID disconnected =
    env->GetMethodID(clazz, "disconnected",
                     "(Lorg/apache/mesos/v1/scheduler/Mesos;)V");

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, disconnected, jmesos);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    ABORT("Exception thrown during `disconnected` call");
  }

  jvm->DetachCurrentThread();
}


extern "C" {

/*
 * Class:     org_apache_mesos_v1_scheduler_V0Mesos
 * Method:    initialize
 * Signature: ()V
 *
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_v1_scheduler_V0Mesos_initialize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  // Create a weak global reference to the Scheduler
  // instance (we want a global reference so the GC doesn't collect
  // the instance but we make it weak so the JVM can exit).
  jweak jmesos = env->NewWeakGlobalRef(thiz);

  // Get out the FrameworkInfo passed into the constructor.
  jfieldID framework =
    env->GetFieldID(clazz, "framework",
                    "Lorg/apache/mesos/v1/Protos$FrameworkInfo;");

  jobject jframework = env->GetObjectField(thiz, framework);

  // Get out the master passed into the constructor.
  jfieldID master = env->GetFieldID(clazz, "master", "Ljava/lang/String;");
  jobject jmaster = env->GetObjectField(thiz, master);

  // Get out the credential passed into the constructor.
  jfieldID credential =
    env->GetFieldID(clazz, "credential",
                    "Lorg/apache/mesos/v1/Protos$Credential;");

  jobject jcredential = env->GetObjectField(thiz, credential);

  Option<Credential> credential_;
  if (!env->IsSameObject(jcredential, nullptr)) {
    credential_ = construct<Credential>(env, jcredential);
  }

  // Create the C++ scheduler and initialize the `__mesos` variable.
  V0ToV1Adapter* mesos =
    new V0ToV1Adapter(
        env,
        jmesos,
        devolve(construct<v1::FrameworkInfo>(env, jframework)),
        construct<string>(env, jmaster),
        credential_);

  jfieldID __mesos = env->GetFieldID(clazz, "__mesos", "J");
  env->SetLongField(thiz, __mesos, (jlong) mesos);
}


/*
 * Class:     org_apache_mesos_v1_scheduler_V0Mesos
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_v1_scheduler_V0Mesos_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __mesos = env->GetFieldID(clazz, "__mesos", "J");

  V0ToV1Adapter* mesos =
    (V0ToV1Adapter*) env->GetLongField(thiz, __mesos);

  env->DeleteWeakGlobalRef(mesos->process->jmesos);

  delete mesos;
}


/*
 * Class:     org_apache_mesos_v1_scheduler_V0Mesos
 * Method:    send
 * Signature: (Lorg/apache/mesos/v1/scheduler/Protos/Call;)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_v1_scheduler_V0Mesos_send
  (JNIEnv* env, jobject thiz, jobject jcall)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __mesos = env->GetFieldID(clazz, "__mesos", "J");

  V0ToV1Adapter* mesos =
    (V0ToV1Adapter*) env->GetLongField(thiz, __mesos);

  mesos->send(construct<Call>(env, jcall));
}

} // extern "C" {
