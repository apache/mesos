/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <map>
#include <vector>

#include <mesos/scheduler.hpp>

#include "construct.hpp"
#include "convert.hpp"
#include "org_apache_mesos_MesosSchedulerDriver.h"

#include "common/foreach.hpp"

using namespace mesos;

using std::string;
using std::map;
using std::vector;


class JNIScheduler : public Scheduler
{
public:
  JNIScheduler(JNIEnv* _env, jobject _jdriver)
    : jvm(NULL), env(_env), jdriver(_jdriver)
  {
    env->GetJavaVM(&jvm);
  }

  virtual ~JNIScheduler() {}

  virtual string getFrameworkName(SchedulerDriver* driver);
  virtual ExecutorInfo getExecutorInfo(SchedulerDriver* driver);
  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId);
  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers);
  virtual void offerRescinded(SchedulerDriver* driver, const OfferID& offerId);
  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status);
  virtual void frameworkMessage(SchedulerDriver* driver,
                                const SlaveID& slaveId,
                                const ExecutorID& executorId,
                                const string& data);
  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId);
  virtual void error(SchedulerDriver* driver, int code, const string& message);

  JavaVM* jvm;
  JNIEnv* env;
  jobject jdriver;
};


string JNIScheduler::getFrameworkName(SchedulerDriver* driver)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // String name = sched.getFrameworkName(driver);
  jmethodID getFrameworkName =
    env->GetMethodID(clazz, "getFrameworkName",
		     "(Lorg/apache/mesos/SchedulerDriver;)"
		     "Ljava/lang/String;");

  env->ExceptionClear();

  jobject jname = env->CallObjectMethod(jsched, getFrameworkName, jdriver);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return "";
  }

  string name = construct<string>(env, (jstring) jname);

  jvm->DetachCurrentThread();

  return name;
}


ExecutorInfo JNIScheduler::getExecutorInfo(SchedulerDriver* driver)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // ExecutorInfo executor = sched.getExecutorInfo(driver);
  jmethodID getExecutorInfo =
    env->GetMethodID(clazz, "getExecutorInfo",
		     "(Lorg/apache/mesos/SchedulerDriver;)"
		     "Lorg/apache/mesos/Protos$ExecutorInfo;");

  env->ExceptionClear();

  jobject jexecutor = env->CallObjectMethod(jsched, getExecutorInfo, jdriver);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return ExecutorInfo();
  }

  ExecutorInfo executor = construct<ExecutorInfo>(env, jexecutor);

  jvm->DetachCurrentThread();

  return executor;
}


void JNIScheduler::registered(SchedulerDriver* driver,
                              const FrameworkID& frameworkId)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.registered(driver, frameworkId);
  jmethodID registered =
    env->GetMethodID(clazz, "registered",
		     "(Lorg/apache/mesos/SchedulerDriver;"
		     "Lorg/apache/mesos/Protos$FrameworkID;)V");

  jobject jframeworkId = convert<FrameworkID>(env, frameworkId);

  env->ExceptionClear();

  env->CallVoidMethod(jsched, registered, jdriver, jframeworkId);

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
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.resourceOffers(driver, offers);
  jmethodID resourceOffers =
    env->GetMethodID(clazz, "resourceOffers",
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

  env->CallVoidMethod(jsched, resourceOffers, jdriver, joffers);

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
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.offerRescinded(driver, offerId);
  jmethodID offerRescinded =
    env->GetMethodID(clazz, "offerRescinded",
		     "(Lorg/apache/mesos/SchedulerDriver;"
		     "Lorg/apache/mesos/Protos$OfferID;)V");

  jobject jofferId = convert<OfferID>(env, offerId);

  env->ExceptionClear();

  env->CallVoidMethod(jsched, offerRescinded, jdriver, jofferId);

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
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.statusUpdate(driver, status);
  jmethodID statusUpdate =
    env->GetMethodID(clazz, "statusUpdate",
		     "(Lorg/apache/mesos/SchedulerDriver;"
		     "Lorg/apache/mesos/Protos$TaskStatus;)V");

  jobject jstatus = convert<TaskStatus>(env, status);

  env->ExceptionClear();

  env->CallVoidMethod(jsched, statusUpdate, jdriver, jstatus);

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
				    const SlaveID& slaveId,
				    const ExecutorID& executorId,
                                    const string& data)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.frameworkMessage(driver, slaveId, executorId, data);
  jmethodID frameworkMessage =
    env->GetMethodID(clazz, "frameworkMessage",
		     "(Lorg/apache/mesos/SchedulerDriver;"
		     "Lorg/apache/mesos/Protos$SlaveID;"
		     "Lorg/apache/mesos/Protos$ExecutorID;[B)V");

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  jobject jslaveId = convert<SlaveID>(env, slaveId);
  jobject jexecutorId = convert<ExecutorID>(env, executorId);

  env->ExceptionClear();

  env->CallVoidMethod(jsched, frameworkMessage,
		      jdriver, jslaveId, jexecutorId, jdata);

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
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.slaveLost(driver, slaveId);
  jmethodID slaveLost =
    env->GetMethodID(clazz, "slaveLost",
		     "(Lorg/apache/mesos/SchedulerDriver;"
		     "Lorg/apache/mesos/Protos$SlaveID;)V");

  jobject jslaveId = convert<SlaveID>(env, slaveId);

  env->ExceptionClear();

  env->CallVoidMethod(jsched, slaveLost, jdriver, jslaveId);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIScheduler::error(SchedulerDriver* driver, int code,
                         const string& message)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.error(driver, code, message);
  jmethodID error =
    env->GetMethodID(clazz, "error",
		     "(Lorg/apache/mesos/SchedulerDriver;"
		     "I"
		     "Ljava/lang/String;)V");

  jint jcode = code;
  jobject jmessage = convert<string>(env, message);

  env->ExceptionClear();

  env->CallVoidMethod(jsched, error, jdriver, jcode, jmessage);

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
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_MesosSchedulerDriver_initialize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  // Create a weak global reference to the MesosSchedulerDriver
  // instance (we want a global reference so the GC doesn't collect
  // the instance but we make it weak so the JVM can exit).
  jobject jdriver = env->NewWeakGlobalRef(thiz);

  // Create the C++ scheduler and initialize the __sched variable.
  JNIScheduler* sched = new JNIScheduler(env, jdriver);

  jfieldID __sched = env->GetFieldID(clazz, "__sched", "J");
  env->SetLongField(thiz, __sched, (jlong) sched);

  // Get out the url passed into the constructor.
  jfieldID url = env->GetFieldID(clazz, "url", "Ljava/lang/String;");
  jobject jurl = env->GetObjectField(thiz, url);

  // Get out the framework id possibly passed into the constructor.
  jfieldID frameworkId = env->GetFieldID(clazz, "frameworkId", "Lorg/apache/mesos/Protos$FrameworkID;");
  jobject jframeworkId = env->GetObjectField(thiz, frameworkId);

  // Get out the framework name passed into the constructor.
  jfieldID frameworkName = env->GetFieldID(clazz, "frameworkName", "Ljava/lang/String;");
  jobject jframeworkName = env->GetObjectField(thiz, frameworkName);

  // Get out the executor info passed into the constructor.
  jfieldID executorInfo = env->GetFieldID(clazz, "executorInfo", "Lorg/apache/mesos/Protos$ExecutorInfo;");
  jobject jexecutorInfo = env->GetObjectField(thiz, executorInfo);

  // Create the C++ driver and initialize the __driver variable.
  MesosSchedulerDriver* driver =
    new MesosSchedulerDriver(sched,
                             construct<string>(env, jframeworkName),
                             construct<ExecutorInfo>(env, jexecutorInfo),
                             construct<string>(env, jurl),
                             construct<FrameworkID>(env, jframeworkId));

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

  // Call stop just in case.
  driver->stop();
  driver->join();

  delete driver;

  jfieldID __sched = env->GetFieldID(clazz, "__sched", "J");
  JNIScheduler* sched = (JNIScheduler*) env->GetLongField(thiz, __sched);

  env->DeleteWeakGlobalRef(sched->jdriver);

  delete sched;
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
 * Method:    sendFrameworkMessage
 * Signature: (Lorg/apache/mesos/Protos/SlaveID;Lorg/apache/mesos/Protos/ExecutorID;[B)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_sendFrameworkMessage
  (JNIEnv* env, jobject thiz, jobject jslaveId, jobject jexecutorId, jbyteArray jdata)
{
  // Construct a C++ SlaveID from the Java SlaveID.
  const SlaveID& slaveId = construct<SlaveID>(env, jslaveId);

  // Construct a C++ ExecutorID from the Java ExecutorID.
  const ExecutorID& executorId = construct<ExecutorID>(env, jexecutorId);

  // Construct a C++ string from the Java byte array.
  jbyte* data = env->GetByteArrayElements(jdata, NULL);
  jsize length = env->GetArrayLength(jdata);

  string temp((char*) data, (size_t) length);

  env->ReleaseByteArrayElements(jdata, data, 0);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->sendFrameworkMessage(slaveId, executorId, temp);

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
 * Signature: (Lorg/apache/mesos/Protos/OfferID;Ljava/util/Collection;Lorg/apache/mesos/Protos/Filters;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_launchTasks
  (JNIEnv* env, jobject thiz, jobject jofferId, jobject jtasks, jobject jfilters)
{
  // Construct a C++ OfferID from the Java OfferID.
  const OfferID& offerId = construct<OfferID>(env, jofferId);

  // Construct a C++ TaskDescription from each Java TaskDescription.
  vector<TaskDescription> tasks;

  jclass clazz = env->GetObjectClass(jtasks);

  // Iterator iterator = tasks.iterator();
  jmethodID iterator =
    env->GetMethodID(clazz, "iterator", "()Ljava/util/Iterator;");
  jobject jiterator = env->CallObjectMethod(jtasks, iterator);

  clazz = env->GetObjectClass(jiterator);

  // while (iterator.hasNext()) {
  jmethodID hasNext = env->GetMethodID(clazz, "hasNext", "()Z");

  jmethodID next = env->GetMethodID(clazz, "next", "()Ljava/lang/Object;");

  while (env->CallBooleanMethod(jiterator, hasNext)) {
    // Object task = iterator.next();
    jobject jtask = env->CallObjectMethod(jiterator, next);
    const TaskDescription& task = construct<TaskDescription>(env, jtask);
    tasks.push_back(task);
  }

  // Construct a C++ Filters from the Java Filters.
  const Filters& filters = construct<Filters>(env, jfilters);

  // Now invoke the underlying driver.
  clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->launchTasks(offerId, tasks, filters);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    reviveOffers
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_reviveOffers
  (JNIEnv* env, jobject thiz)
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
 * Method:    requestResources
 * Signature: (Ljava/util/Collection;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosSchedulerDriver_requestResources
  (JNIEnv* env, jobject thiz, jobject jrequests)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver =
    (MesosSchedulerDriver*) env->GetLongField(thiz, __driver);

  // Construct a C++ ResourceRequest from each Java ResourceRequest.
  vector<ResourceRequest> requests;

  clazz = env->GetObjectClass(jrequests);

  // Iterator iterator = requests.iterator();
  jmethodID iterator =
    env->GetMethodID(clazz, "iterator", "()Ljava/util/Iterator;");
  jobject jiterator = env->CallObjectMethod(jrequests, iterator);

  clazz = env->GetObjectClass(jiterator);

  // while (iterator.hasNext()) {
  jmethodID hasNext = env->GetMethodID(clazz, "hasNext", "()Z");

  jmethodID next = env->GetMethodID(clazz, "next", "()Ljava/lang/Object;");

  while (env->CallBooleanMethod(jrequests, hasNext)) {
    // Object task = iterator.next();
    jobject jrequest = env->CallObjectMethod(jiterator, next);
    const ResourceRequest& request = construct<ResourceRequest>(env, jrequest);
    requests.push_back(request);
  }

  Status status = driver->requestResources(requests);

  return convert<Status>(env, status);
}

} // extern "C" {
