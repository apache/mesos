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

#include <string>
#include <map>
#include <vector>

#include <mesos/scheduler.hpp>

#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "jvm/jvm.hpp"

#include "construct.hpp"
#include "convert.hpp"
#include "org_apache_mesos_MesosSchedulerDriver.h"

using namespace mesos;

using std::string;
using std::map;
using std::vector;


class JNIScheduler : public Scheduler
{
public:
  JNIScheduler(JNIEnv* _env, jweak _jdriver)
    : jvm(nullptr), env(_env), jdriver(_jdriver)
  {
    env->GetJavaVM(&jvm);
  }

  ~JNIScheduler() override {}

  void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId,
                          const MasterInfo& masterInfo) override;
  void reregistered(SchedulerDriver*, const MasterInfo& masterInfo) override;
  void disconnected(SchedulerDriver* driver) override;
  void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers) override;
  void offerRescinded(SchedulerDriver* driver, const OfferID& offerId) override;
  void statusUpdate(SchedulerDriver* driver, const TaskStatus& status) override;
  void frameworkMessage(SchedulerDriver* driver,
                                const ExecutorID& executorId,
                                const SlaveID& slaveId,
                                const string& data) override;
  void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId) override;
  void executorLost(SchedulerDriver* driver,
                            const ExecutorID& executorId,
                            const SlaveID& slaveId,
                            int status) override;
  void error(SchedulerDriver* driver, const string& message) override;

  JavaVM* jvm;
  JNIEnv* env;
  jweak jdriver;
};


void JNIScheduler::registered(SchedulerDriver* driver,
                              const FrameworkID& frameworkId,
                              const MasterInfo& masterInfo)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // sched.registered(driver, frameworkId, masterInfo);
  jmethodID registered = env->GetMethodID(
      clazz,
      "registered",
      "(Lorg/apache/mesos/SchedulerDriver;"
      "Lorg/apache/mesos/Protos$FrameworkID;"
      "Lorg/apache/mesos/Protos$MasterInfo;)V");

  jobject jframeworkId = convert<FrameworkID>(env, frameworkId);

  jobject jmasterInfo = convert<MasterInfo>(env, masterInfo);

  env->ExceptionClear();

  env->CallVoidMethod(
      jscheduler, registered, jdriver, jframeworkId, jmasterInfo);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::reregistered(SchedulerDriver* driver,
                                const MasterInfo& masterInfo)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.reregistered(driver, masterInfo);
  jmethodID reregistered =
    env->GetMethodID(clazz, "reregistered",
         "(Lorg/apache/mesos/SchedulerDriver;"
         "Lorg/apache/mesos/Protos$MasterInfo;)V");

  jobject jmasterInfo = convert<MasterInfo>(env, masterInfo);

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, reregistered, jdriver, jmasterInfo);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::disconnected(SchedulerDriver* driver)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.disconnected(driver);
  jmethodID disconnected =
    env->GetMethodID(clazz, "disconnected",
         "(Lorg/apache/mesos/SchedulerDriver;)V");

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, disconnected, jdriver);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::resourceOffers(SchedulerDriver* driver,
                                  const vector<Offer>& offers)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.resourceOffers(driver, offers);
  jmethodID resourceOffers = env->GetMethodID(
      clazz,
      "resourceOffers",
      "(Lorg/apache/mesos/SchedulerDriver;"
      "Ljava/util/List;)V");

  // List offers = new ArrayList();
  clazz = env->FindClass("java/util/ArrayList");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject joffers = env->NewObject(clazz, _init_);

  jmethodID add = env->GetMethodID(clazz, "add", "(Ljava/lang/Object;)Z");

  // Loop through C++ vector and add each offer to the Java list.
  foreach (const Offer& offer, offers) {
    jobject joffer = convert<Offer>(env, offer);
    env->CallBooleanMethod(joffers, add, joffer);
  }

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, resourceOffers, jdriver, joffers);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::offerRescinded(SchedulerDriver* driver,
                                  const OfferID& offerId)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.offerRescinded(driver, offerId);
  jmethodID offerRescinded = env->GetMethodID(
      clazz,
      "offerRescinded",
      "(Lorg/apache/mesos/SchedulerDriver;"
      "Lorg/apache/mesos/Protos$OfferID;)V");

  jobject jofferId = convert<OfferID>(env, offerId);

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, offerRescinded, jdriver, jofferId);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::statusUpdate(SchedulerDriver* driver,
                                const TaskStatus& status)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.statusUpdate(driver, status);
  jmethodID statusUpdate =
    env->GetMethodID(clazz, "statusUpdate",
                     "(Lorg/apache/mesos/SchedulerDriver;"
                     "Lorg/apache/mesos/Protos$TaskStatus;)V");

  jobject jstatus = convert<TaskStatus>(env, status);

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, statusUpdate, jdriver, jstatus);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::frameworkMessage(SchedulerDriver* driver,
                                    const ExecutorID& executorId,
                                    const SlaveID& slaveId,
                                    const string& data)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.frameworkMessage(driver, executorId, slaveId, data);
  jmethodID frameworkMessage = env->GetMethodID(
      clazz,
      "frameworkMessage",
      "(Lorg/apache/mesos/SchedulerDriver;"
      "Lorg/apache/mesos/Protos$ExecutorID;"
      "Lorg/apache/mesos/Protos$SlaveID;[B)V");

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  jobject jexecutorId = convert<ExecutorID>(env, executorId);
  jobject jslaveId = convert<SlaveID>(env, slaveId);

  env->ExceptionClear();

  env->CallVoidMethod(
      jscheduler, frameworkMessage, jdriver, jexecutorId, jslaveId, jdata);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::slaveLost(SchedulerDriver* driver, const SlaveID& slaveId)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.slaveLost(driver, slaveId);
  jmethodID slaveLost = env->GetMethodID(
      clazz,
      "slaveLost",
      "(Lorg/apache/mesos/SchedulerDriver;"
      "Lorg/apache/mesos/Protos$SlaveID;)V");

  jobject jslaveId = convert<SlaveID>(env, slaveId);

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, slaveLost, jdriver, jslaveId);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::executorLost(SchedulerDriver* driver,
                                const ExecutorID& executorId,
                                const SlaveID& slaveId,
                                int status)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.executorLost(driver, slaveId, executorId, status);
  jmethodID executorLost =
    env->GetMethodID(clazz, "executorLost",
         "(Lorg/apache/mesos/SchedulerDriver;"
         "Lorg/apache/mesos/Protos$ExecutorID;"
         "Lorg/apache/mesos/Protos$SlaveID;"
         "I)V");

  jobject jexecutorId = convert<ExecutorID>(env, executorId);

  jobject jslaveId = convert<SlaveID>(env, slaveId);

  jint jstatus = status;

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, executorLost,
                      jdriver, jexecutorId, jslaveId, jstatus);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::error(SchedulerDriver* driver, const string& message)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID scheduler =
    env->GetFieldID(clazz, "scheduler", "Lorg/apache/mesos/Scheduler;");
  jobject jscheduler = env->GetObjectField(jdriver, scheduler);

  clazz = env->GetObjectClass(jscheduler);

  // scheduler.error(driver, message);
  jmethodID error = env->GetMethodID(
      clazz,
      "error",
      "(Lorg/apache/mesos/SchedulerDriver;"
      "Ljava/lang/String;)V");

  jobject jmessage = convert<string>(env, message);

  env->ExceptionClear();

  env->CallVoidMethod(jscheduler, error, jdriver, jmessage);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


extern "C" {

/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    initialize
 * Signature: ()V
 *
 * TODO(vinod): Deprecate this in favor of
 * 'Java_org_apache_mesos_MesosSchedulerDriver_init' below.
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_MesosSchedulerDriver_initialize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  // Create a weak global reference to the MesosSchedulerDriver
  // instance (we want a global reference so the GC doesn't collect
  // the instance but we make it weak so the JVM can exit).
  jweak jdriver = env->NewWeakGlobalRef(thiz);

  // Create the C++ scheduler and initialize the __scheduler variable.
  JNIScheduler* scheduler = new JNIScheduler(env, jdriver);

  jfieldID __scheduler = env->GetFieldID(clazz, "__scheduler", "J");
  env->SetLongField(thiz, __scheduler, (jlong) scheduler);

  // Get out the FrameworkInfo passed into the constructor.
  jfieldID framework = env->GetFieldID(
      clazz, "framework", "Lorg/apache/mesos/Protos$FrameworkInfo;");
  jobject jframework = env->GetObjectField(thiz, framework);

  // Get out the master passed into the constructor.
  jfieldID master = env->GetFieldID(clazz, "master", "Ljava/lang/String;");
  jobject jmaster = env->GetObjectField(thiz, master);

  // NOTE: Older versions (< 0.22.0) of MesosSchedulerDriver.java
  // do not have the 'implicitAcknowledgements' field, so when None()
  // is returned we default to the old behavior: implicit
  // acknowledgements.
  Result<jfieldID> implicitAcknowledgements =
    getFieldID(env, clazz, "implicitAcknowledgements", "Z");

  if (implicitAcknowledgements.isError()) {
    return; // Exception has been thrown.
  }

  // Default to implicit acknowledgements, as done before 0.22.0.
  jboolean jimplicitAcknowledgements = JNI_TRUE;
  if (implicitAcknowledgements.isSome()) {
    jimplicitAcknowledgements =
      env->GetBooleanField(thiz, implicitAcknowledgements.get());
  }

  // Get out the Credential passed into the constructor.
  // NOTE: Older versions (< 0.15.0) of MesosSchedulerDriver do not set
  // 'credential' field. To be backwards compatible we should safely
  // handle this case.
  Result<jfieldID> credential = getFieldID(
      env, clazz, "credential", "Lorg/apache/mesos/Protos$Credential;");
  if (credential.isError()) {
    return; // Exception has been thrown.
  }

  jobject jcredential = nullptr;
  if (credential.isSome()) {
    // Credential might exist but set to 'null'.
    jcredential = env->GetObjectField(thiz, credential.get());
  }

  // Get out the suppressedRoles passed into the constructor.
  //
  // NOTE: Older versions (< 1.9.0) of MesosSchedulerDriver do not set the
  // 'suppressedRoles' field. To be backwards compatible, we should use an empty
  // list of suppressed roles if the field is not set.

  Result<jfieldID> suppressedRolesFieldID = getFieldID(
      env, clazz, "suppressedRoles", "Ljava/util/Collection;");
  if (suppressedRolesFieldID.isError()) {
    return; // Exception has been thrown.
  }

  vector<string> suppressedRoles;
  if (suppressedRolesFieldID.isSome()) {
    jobject jsuppressedRoles =
      env->GetObjectField(thiz, suppressedRolesFieldID.get());
    if (jsuppressedRoles != nullptr) {
      suppressedRoles = constructFromIterable<string>(env, jsuppressedRoles);
    }
  }

  // Create the C++ driver.
  MesosSchedulerDriver* driver = nullptr;
  if (jcredential != nullptr) {
     driver = new MesosSchedulerDriver(
        scheduler,
        construct<FrameworkInfo>(env, jframework),
        suppressedRoles,
        construct<string>(env, jmaster),
        construct(env, jimplicitAcknowledgements),
        construct<Credential>(env, jcredential));
  } else {
    driver = new MesosSchedulerDriver(
       scheduler,
       construct<FrameworkInfo>(env, jframework),
       suppressedRoles,
       construct<string>(env, jmaster),
       construct(env, jimplicitAcknowledgements));
  }

  // Initialize the __driver variable
  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  env->SetLongField(thiz, __driver, (jlong) driver);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_MesosSchedulerDriver_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  // Note that we DO NOT want to call 'abort' or 'stop' as this may be
  // misinterpreted by the scheduler. It is possible, however, that
  // since we haven't called 'abort' or 'stop' there are still threads
  // executing within the scheduler callbacks but the
  // MesosSchedulerDriver destructor will wait until this is not the
  // case before returning.
  delete driver;

  jfieldID __scheduler = env->GetFieldID(clazz, "__scheduler", "J");
  JNIScheduler* scheduler =
    (JNIScheduler*) env->GetLongField(thiz, __scheduler);

  env->DeleteWeakGlobalRef(scheduler->jdriver);

  delete scheduler;
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    start
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_start
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->start();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    stop
 * Signature: (Z)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_stop
  (JNIEnv* env, jobject thiz, jboolean failover)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->stop(failover);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    abort
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_abort
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->abort();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    join
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_join
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->join();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    acknowledgeStatusUpdate
 * Signature: (Lorg/apache/mesos/Protos/TaskStatus;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_acknowledgeStatusUpdate(
    JNIEnv* env, jobject thiz, jobject jtaskStatus)
{
  // Construct a C++ TaskID from the Java TaskId.
  const TaskStatus& taskStatus = construct<TaskStatus>(env, jtaskStatus);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->acknowledgeStatusUpdate(taskStatus);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    sendFrameworkMessage
 * Signature: (Lorg/apache/mesos/Protos/ExecutorID;Lorg/apache/mesos/Protos/SlaveID;[B)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_sendFrameworkMessage(
    JNIEnv* env,
    jobject thiz,
    jobject jexecutorId,
    jobject jslaveId,
    jbyteArray jdata)
{
  // Construct a C++ ExecutorID from the Java ExecutorID.
  const ExecutorID& executorId = construct<ExecutorID>(env, jexecutorId);

  // Construct a C++ SlaveID from the Java SlaveID.
  const SlaveID& slaveId = construct<SlaveID>(env, jslaveId);

  // Construct a C++ string from the Java byte array.
  jbyte* data = env->GetByteArrayElements(jdata, nullptr);
  jsize length = env->GetArrayLength(jdata);

  string temp((char*) data, (size_t) length);

  env->ReleaseByteArrayElements(jdata, data, 0);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->sendFrameworkMessage(executorId, slaveId, temp);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    killTask
 * Signature: (Lorg/apache/mesos/Protos/TaskID;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_killTask
  (JNIEnv* env, jobject thiz, jobject jtaskId)
{
  // Construct a C++ TaskID from the Java TaskId.
  const TaskID& taskId = construct<TaskID>(env, jtaskId);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->killTask(taskId);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    launchTasks
 * Signature: (Lorg/apache/mesos/Protos$OfferID;Ljava/util/Collection;Lorg/apache/mesos/Protos$Filters;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_launchTasks__Lorg_apache_mesos_Protos_00024OfferID_2Ljava_util_Collection_2Lorg_apache_mesos_Protos_00024Filters_2( // NOLINT(whitespace/line_length)
    JNIEnv* env,
    jobject thiz,
    jobject jofferId,
    jobject jtasks,
    jobject jfilters)
{
  // Construct a C++ OfferID from the Java OfferID.
  const OfferID& offerId = construct<OfferID>(env, jofferId);

  // Construct a C++ TaskInfo from each Java TaskInfo.
  vector<TaskInfo> tasks = constructFromIterable<TaskInfo>(env, jtasks);

  // Construct a C++ Filters from the Java Filters.
  const Filters& filters = construct<Filters>(env, jfilters);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  vector<OfferID> offerIds;
  offerIds.push_back(offerId);

  Status status = driver->launchTasks(offerIds, tasks, filters);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    launchTasks
 * Signature: (Ljava/util/Collection;Ljava/util/Collection;Lorg/apache/mesos/Protos$Filters;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_launchTasks__Ljava_util_Collection_2Ljava_util_Collection_2Lorg_apache_mesos_Protos_00024Filters_2( // NOLINT(whitespace/line_length)
    JNIEnv* env,
    jobject thiz,
    jobject jofferIds,
    jobject jtasks,
    jobject jfilters)
{
  // Construct a C++ OfferID from each Java OfferID.
  vector<OfferID> offers = constructFromIterable<OfferID>(env, jofferIds);

  // Construct a C++ TaskInfo from each Java TaskInfo.
  vector<TaskInfo> tasks = constructFromIterable<TaskInfo>(env, jtasks);

  // Construct a C++ Filters from the Java Filters.
  const Filters& filters = construct<Filters>(env, jfilters);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->launchTasks(offers, tasks, filters);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    acceptOffers
 * Signature: (Ljava/util/Collection;Ljava/util/Collection;Lorg/apache/mesos/Protos$Filters;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_acceptOffers__Ljava_util_Collection_2Ljava_util_Collection_2Lorg_apache_mesos_Protos_00024Filters_2( // NOLINT(whitespace/line_length)
    JNIEnv* env,
    jobject thiz,
    jobject jofferIds,
    jobject joperations,
    jobject jfilters)
{
  // Construct C++ OfferIDs from each Java OfferIDs.
  vector<OfferID> offers = constructFromIterable<OfferID>(env, jofferIds);

  // Construct C++ Offer::Operations from each Java Offer.Operations.
  vector<Offer::Operation> operations =
    constructFromIterable<Offer::Operation>(env, joperations);

  // Construct C++ Filters from the Java Filters.
  const Filters& filters = construct<Filters>(env, jfilters);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->acceptOffers(offers, operations, filters);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    declineOffer
 * Signature: (Lorg/apache/mesos/Protos/OfferID;Lorg/apache/mesos/Protos/Filters;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_declineOffer(
    JNIEnv* env, jobject thiz, jobject jofferId, jobject jfilters)
{
  // Construct a C++ OfferID from the Java OfferID.
  const OfferID& offerId = construct<OfferID>(env, jofferId);

  // Construct a C++ Filters from the Java Filters.
  const Filters& filters = construct<Filters>(env, jfilters);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->declineOffer(offerId, filters);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    reviveOffers
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_reviveOffers__(
    JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->reviveOffers();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    reviveOffersForRoles
 * Signature: (Ljava/util/Collection;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_reviveOffers__Ljava_util_Collection_2( // NOLINT(whitespace/line_length)
    JNIEnv* env, jobject thiz, jobject jroles)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status =
    driver->reviveOffers(constructFromIterable<string>(env, jroles));

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    suppressOffers
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_suppressOffers__(
    JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->suppressOffers();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    suppressOffersForRoles
 * Signature: (Ljava/util/Collection;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_suppressOffers__Ljava_util_Collection_2( // NOLINT(whitespace/line_length)
    JNIEnv* env, jobject thiz, jobject jroles)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status =
    driver->suppressOffers(constructFromIterable<string>(env, jroles));

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    requestResources
 * Signature: (Ljava/util/Collection;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_requestResources(
    JNIEnv* env, jobject thiz, jobject jrequests)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  // Construct a C++ Request from each Java Request.
  vector<Request> requests = constructFromIterable<Request>(env, jrequests);

  Status status = driver->requestResources(requests);

  return convert<Status>(env, status);
}

/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    reconcileTasks
 * Signature: (Ljava/util/Collection;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_reconcileTasks(
    JNIEnv* env, jobject thiz, jobject jstatuses)
{
  // Construct a C++ TaskStatus from each Java TaskStatus.
  vector<TaskStatus> statuses =
    constructFromIterable<TaskStatus>(env, jstatuses);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->reconcileTasks(statuses);

  return convert<Status>(env, status);
}

static jobject updateFramework(
    JNIEnv* env,
    jobject thiz,
    jobject jframeworkInfo,
    jobject jsuppressedRoles,
    Option<jobject> jofferConstraints)
{
  using ::mesos::scheduler::OfferConstraints;

  const FrameworkInfo& frameworkInfo =
    construct<FrameworkInfo>(env, jframeworkInfo);

  const vector<string> suppressedRoles =
    constructFromIterable<string>(env, jsuppressedRoles);

  OfferConstraints offerConstraints = jofferConstraints.isSome()
      ? construct<OfferConstraints>(env, *jofferConstraints)
      : OfferConstraints();

  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->updateFramework(
      frameworkInfo, suppressedRoles, std::move(offerConstraints));

  return convert<Status>(env, status);
}

/* Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    updateFramework
 * Signature: (Lorg/apache/mesos/Protos/FrameworkInfo;Ljava/util/Collection;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_updateFramework(
    JNIEnv* env, jobject thiz, jobject jframeworkInfo, jobject jsuppressedRoles)
{
  return updateFramework(env, thiz, jframeworkInfo, jsuppressedRoles, None());
}

/* Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    updateFrameworkWithConstraints
 * Signature: (Lorg/apache/mesos/Protos/FrameworkInfo;Ljava/util/Collection;Lorg/apache/mesos/scheduler/Protos/OfferConstraints;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_MesosSchedulerDriver_updateFrameworkWithConstraints(
    JNIEnv* env,
    jobject thiz,
    jobject jframeworkInfo,
    jobject jsuppressedRoles,
    jobject jofferConstraints)
{
  return updateFramework(
      env, thiz, jframeworkInfo, jsuppressedRoles, jofferConstraints);
}

} // extern "C" {
