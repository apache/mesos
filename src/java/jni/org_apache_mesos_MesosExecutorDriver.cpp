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

#include <mesos/executor.hpp>

#include "construct.hpp"
#include "convert.hpp"
#include "org_apache_mesos_MesosExecutorDriver.h"

using namespace mesos;

using std::string;


class JNIExecutor : public Executor
{
public:
  JNIExecutor(JNIEnv* _env, jobject _jdriver)
    : jvm(NULL), env(_env), jdriver(_jdriver)
  {
    env->GetJavaVM(&jvm);
  }

  virtual ~JNIExecutor() {}

  virtual void init(ExecutorDriver* driver, const ExecutorArgs& args);
  virtual void launchTask(ExecutorDriver* driver, const TaskDescription& task);
  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId);
  virtual void frameworkMessage(ExecutorDriver* driver, const string& data);
  virtual void shutdown(ExecutorDriver* driver);
  virtual void error(ExecutorDriver* driver, int code, const string& message);

  JavaVM* jvm;
  JNIEnv* env;
  jobject jdriver;
};


void JNIExecutor::init(ExecutorDriver* driver, const ExecutorArgs& args)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lorg/apache/mesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.init(driver);
  jmethodID init =
    env->GetMethodID(clazz, "init",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "Lorg/apache/mesos/Protos$ExecutorArgs;)V");

  jobject jargs = convert<ExecutorArgs>(env, args);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, init, jdriver, jargs);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::launchTask(ExecutorDriver* driver, const TaskDescription& desc)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lorg/apache/mesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.launchTask(driver, desc);
  jmethodID launchTask =
    env->GetMethodID(clazz, "launchTask",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "Lorg/apache/mesos/Protos$TaskDescription;)V");

  jobject jdesc = convert<TaskDescription>(env, desc);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, launchTask, jdriver, jdesc);

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
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lorg/apache/mesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.killTask(driver, taskId);
  jmethodID killTask =
    env->GetMethodID(clazz, "killTask",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "Lorg/apache/mesos/Protos$TaskID;)V");

  jobject jtaskId = convert<TaskID>(env, taskId);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, killTask, jdriver, jtaskId);

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
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lorg/apache/mesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.frameworkMessage(driver, data);
  jmethodID frameworkMessage =
    env->GetMethodID(clazz, "frameworkMessage",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "[B)V");

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  env->ExceptionClear();

  env->CallVoidMethod(jexec, frameworkMessage, jdriver, jdata);

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
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lorg/apache/mesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.shutdown(driver);
  jmethodID shutdown =
    env->GetMethodID(clazz, "shutdown",
		     "(Lorg/apache/mesos/ExecutorDriver;)V");

  env->ExceptionClear();

  env->CallVoidMethod(jexec, shutdown, jdriver);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->abort();
    return;
  }

  jvm->DetachCurrentThread();
}


void JNIExecutor::error(ExecutorDriver* driver, int code, const string& message)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lorg/apache/mesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.error(driver, code, message);
  jmethodID error =
    env->GetMethodID(clazz, "error",
		     "(Lorg/apache/mesos/ExecutorDriver;"
		     "I"
		     "Ljava/lang/String;)V");

  jint jcode = code;
  jobject jmessage = convert<string>(env, message);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, error, jdriver, jcode, jmessage);

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
  jobject jdriver = env->NewWeakGlobalRef(thiz);

  // Create the C++ executor and initialize the __exec variable.
  JNIExecutor* exec = new JNIExecutor(env, jdriver);

  jfieldID __exec = env->GetFieldID(clazz, "__exec", "J");
  env->SetLongField(thiz, __exec, (jlong) exec);

  // Create the C++ driver and initialize the __driver variable.
  MesosExecutorDriver* driver = new MesosExecutorDriver(exec);

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

  // Call stop just in case.
  driver->stop();
  driver->join();

  delete driver;

  jfieldID __exec = env->GetFieldID(clazz, "__exec", "J");
  JNIExecutor* exec = (JNIExecutor*) env->GetLongField(thiz, __exec);

  env->DeleteWeakGlobalRef(exec->jdriver);

  delete exec;
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
  jbyte* data = env->GetByteArrayElements(jdata, NULL);
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
