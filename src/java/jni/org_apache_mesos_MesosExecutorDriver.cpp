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

#include <mesos/executor.hpp>

#include "jvm/jvm.hpp"

#include "construct.hpp"
#include "convert.hpp"
#include "org_apache_mesos_MesosExecutorDriver.h"

using namespace mesos;

using std::string;


class JNIExecutor : public Executor
{
public:
  JNIExecutor(JNIEnv* _env, jweak _jdriver)
    : jvm(nullptr), env(_env), jdriver(_jdriver)
  {
    env->GetJavaVM(&jvm);
  }

  virtual ~JNIExecutor() {}

  virtual void registered(ExecutorDriver* driver,
                          const ExecutorInfo& executorInfo,
                          const FrameworkInfo& frameworkInfo,
                          const SlaveInfo& slaveInfo);
  virtual void reregistered(ExecutorDriver* driver, const SlaveInfo& slaveInfo);
  virtual void disconnected(ExecutorDriver* driver);
  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task);
  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId);
  virtual void frameworkMessage(ExecutorDriver* driver, const string& data);
  virtual void shutdown(ExecutorDriver* driver);
  virtual void error(ExecutorDriver* driver, const string& message);

  JavaVM* jvm;
  JNIEnv* env;
  jweak jdriver;
};


void JNIExecutor::registered(ExecutorDriver* driver,
                            const ExecutorInfo& executorInfo,
                            const FrameworkInfo& frameworkInfo,
                            const SlaveInfo& slaveInfo)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.registered(driver);
  jmethodID registered =
    env->GetMethodID(clazz, "registered",
                     "(Lorg/apache/mesos/ExecutorDriver;"
                     "Lorg/apache/mesos/Protos$ExecutorInfo;"
                     "Lorg/apache/mesos/Protos$FrameworkInfo;"
                     "Lorg/apache/mesos/Protos$SlaveInfo;)V");

  jobject jexecutorInfo = convert<ExecutorInfo>(env, executorInfo);
  jobject jframeworkInfo = convert<FrameworkInfo>(env, frameworkInfo);
  jobject jslaveInfo = convert<SlaveInfo>(env, slaveInfo);

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, registered, jdriver, jexecutorInfo,
                      jframeworkInfo, jslaveInfo);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::reregistered(ExecutorDriver* driver,
                               const SlaveInfo& slaveInfo)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.reregistered(driver, slaveInfo);
  jmethodID reregistered =
    env->GetMethodID(clazz, "reregistered",
                     "(Lorg/apache/mesos/ExecutorDriver;"
                     "Lorg/apache/mesos/Protos$SlaveInfo;)V");

  jobject jslaveInfo = convert<SlaveInfo>(env, slaveInfo);

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, reregistered, jdriver, jslaveInfo);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::disconnected(ExecutorDriver* driver)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.disconnected(driver);
  jmethodID disconnected =
    env->GetMethodID(clazz, "disconnected",
         "(Lorg/apache/mesos/ExecutorDriver;)V");

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, disconnected, jdriver);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::launchTask(ExecutorDriver* driver, const TaskInfo& desc)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.launchTask(driver, desc);
  jmethodID launchTask =
    env->GetMethodID(clazz, "launchTask",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "Lorg/apache/mesos/Protos$TaskInfo;)V");

  jobject jdesc = convert<TaskInfo>(env, desc);

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, launchTask, jdriver, jdesc);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::killTask(ExecutorDriver* driver, const TaskID& taskId)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.killTask(driver, taskId);
  jmethodID killTask =
    env->GetMethodID(clazz, "killTask",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "Lorg/apache/mesos/Protos$TaskID;)V");

  jobject jtaskId = convert<TaskID>(env, taskId);

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, killTask, jdriver, jtaskId);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::frameworkMessage(ExecutorDriver* driver, const string& data)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.frameworkMessage(driver, data);
  jmethodID frameworkMessage =
    env->GetMethodID(clazz, "frameworkMessage",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "[B)V");

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, frameworkMessage, jdriver, jdata);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::shutdown(ExecutorDriver* driver)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.shutdown(driver);
  jmethodID shutdown =
    env->GetMethodID(clazz, "shutdown",
		     "(Lorg/apache/mesos/ExecutorDriver;)V");

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, shutdown, jdriver);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::error(ExecutorDriver* driver, const string& message)
{
  jvm->AttachCurrentThread(JNIENV_CAST(&env), nullptr);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID executor = env->GetFieldID(clazz, "executor", "Lorg/apache/mesos/Executor;");
  jobject jexecutor = env->GetObjectField(jdriver, executor);

  clazz = env->GetObjectClass(jexecutor);

  // executor.error(driver, message);
  jmethodID error =
    env->GetMethodID(clazz, "error",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "Ljava/lang/String;)V");

  jobject jmessage = convert<string>(env, message);

  env->ExceptionClear();

  env->CallVoidMethod(jexecutor, error, jdriver, jmessage);

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
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    initialize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_MesosExecutorDriver_initialize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  // Create a weak global reference to the MesosExecutorDriver
  // instance (we want a global reference so the GC doesn't collect
  // the instance but we make it weak so the JVM can exit).
  jweak jdriver = env->NewWeakGlobalRef(thiz);

  // Create the C++ executor and initialize the __executor variable.
  JNIExecutor* executor = new JNIExecutor(env, jdriver);

  jfieldID __executor = env->GetFieldID(clazz, "__executor", "J");
  env->SetLongField(thiz, __executor, (jlong) executor);

  // Create the C++ driver and initialize the __driver variable.
  MesosExecutorDriver* driver = new MesosExecutorDriver(executor);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  env->SetLongField(thiz, __driver, (jlong) driver);
}


/*
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_MesosExecutorDriver_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver =
    (MesosExecutorDriver*) env->GetLongField(thiz, __driver);

  // Note that we DO NOT want to call 'abort' or 'stop' as this may be
  // misinterpreted by the executor. It is possible, however, that
  // since we haven't called 'abort' or 'stop' there are still threads
  // executing within the executor callbacks but the
  // MesosExecutorDriver destructor will wait until this is not the
  // case before returning.
  delete driver;

  jfieldID __executor = env->GetFieldID(clazz, "__executor", "J");
  JNIExecutor* executor = (JNIExecutor*) env->GetLongField(thiz, __executor);

  env->DeleteWeakGlobalRef(executor->jdriver);

  delete executor;
}


/*
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    start
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosExecutorDriver_start
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver =
    (MesosExecutorDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->start();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    stop
 * Signature: (Z)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosExecutorDriver_stop
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver =
    (MesosExecutorDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->stop();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    abort
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosExecutorDriver_abort
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver =
    (MesosExecutorDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->abort();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    join
 * Signature: ()Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosExecutorDriver_join
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver =
    (MesosExecutorDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->join();

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    sendStatusUpdate
 * Signature: (Lorg/apache/mesos/Protos/TaskStatus;)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosExecutorDriver_sendStatusUpdate
  (JNIEnv* env, jobject thiz, jobject jstatus)
{
  // Construct a C++ TaskStatus from the Java TaskStatus.
  const TaskStatus& taskStatus = construct<TaskStatus>(env, jstatus);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver =
    (MesosExecutorDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->sendStatusUpdate(taskStatus);

  return convert<Status>(env, status);
}


/*
 * Class:     org_apache_mesos_MesosExecutorDriver
 * Method:    sendFrameworkMessage
 * Signature: ([B)Lorg/apache/mesos/Protos/Status;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosExecutorDriver_sendFrameworkMessage
  (JNIEnv* env, jobject thiz, jbyteArray jdata)
{
  // Construct a C++ string from the Java byte array.
  jbyte* data = env->GetByteArrayElements(jdata, nullptr);
  jsize length = env->GetArrayLength(jdata);

  string temp((char*) data, (size_t) length);

  env->ReleaseByteArrayElements(jdata, data, 0);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver =
    (MesosExecutorDriver*) env->GetLongField(thiz, __driver);

  Status status = driver->sendFrameworkMessage(temp);

  return convert<Status>(env, status);
}

} // extern "C" {
