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
#include <vector>

#include <process/owned.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/scheduler.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "jvm/jvm.hpp"

#include "construct.hpp"
#include "convert.hpp"
#include "org_apache_mesos_v1_scheduler_V1Mesos.h"

using namespace mesos::v1::scheduler;

using mesos::v1::Credential;

using process::Owned;

using std::string;
using std::vector;

namespace v1 {

class JNIMesos
{
public:
  JNIMesos(
      JNIEnv* _env,
      jweak _jmesos,
      const string& master,
      const Option<Credential>& credential)
    : jvm(nullptr), env(_env), jmesos(_jmesos)
  {
    env->GetJavaVM(&jvm);

    mesos.reset(
        new Mesos(master,
            mesos::ContentType::PROTOBUF,
            std::bind(&JNIMesos::connected, this),
            std::bind(&JNIMesos::disconnected, this),
            std::bind(&JNIMesos::received_, this, lambda::_1),
            credential));
  }

  virtual ~JNIMesos() = default;

  virtual void connected();
  virtual void disconnected();
  virtual void received(const Event& event);

  void received_(std::queue<Event> events) {
    while (!events.empty()) {
      received(events.front());
      events.pop();
    }
  }

  JavaVM* jvm;
  JNIEnv* env;
  jweak jmesos;

  Owned<Mesos> mesos;
};


void JNIMesos::connected()
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


void JNIMesos::disconnected()
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


void JNIMesos::received(const Event& event)
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

} // namespace v1 {


extern "C" {

/*
 * Class:     org_apache_mesos_v1_V1Mesos
 * Method:    initialize
 * Signature: ()V
 *
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_v1_scheduler_V1Mesos_initialize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  // Create a weak global reference to the Scheduler
  // instance (we want a global reference so the GC doesn't collect
  // the instance but we make it weak so the JVM can exit).
  jweak jmesos = env->NewWeakGlobalRef(thiz);

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

  // Create the C++ scheduler and initialize `__mesos`.
  v1::JNIMesos* mesos =
    new v1::JNIMesos(env, jmesos, construct<string>(env, jmaster), credential_);

  jfieldID __mesos = env->GetFieldID(clazz, "__mesos", "J");
  env->SetLongField(thiz, __mesos, (jlong) mesos);
}


/*
 * Class:     org_apache_mesos_v1_V1Mesos
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_v1_scheduler_V1Mesos_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __mesos = env->GetFieldID(clazz, "__mesos", "J");
  v1::JNIMesos* mesos =
    (v1::JNIMesos*) env->GetLongField(thiz, __mesos);

  env->DeleteWeakGlobalRef(mesos->jmesos);

  delete mesos;
}


/*
 * Class:     org_apache_mesos_v1_V1Mesos
 * Method:    send
 * Signature: (Lorg/apache/mesos/v1/scheduler/Protos/Call;)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_v1_scheduler_V1Mesos_send
  (JNIEnv* env, jobject thiz, jobject jcall)
{
  // Construct a C++ Call from the Java Call.
  const Call& call = construct<Call>(env, jcall);

  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __mesos = env->GetFieldID(clazz, "__mesos", "J");
  v1::JNIMesos* mesos =
    (v1::JNIMesos*) env->GetLongField(thiz, __mesos);

  // It is possible that `mesos` might not be initialized in some cases due to
  // a possible race condition. See MESOS-5926 for more details.
  if (mesos->mesos.get() == nullptr) {
    LOG(WARNING) << "Ignoring call " << call.type() << " as the library has "
                 << "not been initialized yet";
    return;
  }

  // Destruction of the library is not always under our control. For example,
  // the JVM can call JNI `finalize()` if the Java scheduler nullifies its
  // reference to the V1Mesos library immediately after sending `TEARDOWN`,
  // see MESOS-9274.
  //
  // We want to make sure that the `TEARDOWN` message is sent before the
  // scheduler and the Mesos library are destructed (garbage collected).
  // However we don't want to block forever if the message is dropped.
  //
  // TODO(alexr): Consider adding general support for `call()`.
  if (call.type() == Call::TEARDOWN) {
    const Duration timeout = Minutes(10);
    bool timedout = !mesos->mesos->call(call).await(timeout);
    LOG_IF(ERROR, timedout)
      << "Received no response to call " << call.type() << " for " << timeout;
  } else {
    mesos->mesos->send(call);
  }
}


/*
 * Class:     org_apache_mesos_v1_scheduler_V1Mesos
 * Method:    reconnect
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_v1_scheduler_V1Mesos_reconnect
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __mesos = env->GetFieldID(clazz, "__mesos", "J");
  v1::JNIMesos* mesos =
    (v1::JNIMesos*) env->GetLongField(thiz, __mesos);

  // It is possible that `mesos` might not be initialized in some cases due to
  // a possible race condition. See MESOS-5926 for more details.
  if (mesos->mesos.get() == nullptr) {
    LOG(WARNING) << "Ignoring the reconnect request as the library has not "
                 << "been initialized yet";
    return;
  }

  mesos->mesos->reconnect();
}

} // extern "C" {
