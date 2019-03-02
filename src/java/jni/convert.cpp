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

#include <jni.h>

#include <string>
#include <assert.h>

#include <mesos/mesos.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "construct.hpp"
#include "convert.hpp"

#include "jvm/jvm.hpp"

#include "logging/logging.hpp"

using namespace mesos;

using std::string;

// Facilities for loading Mesos-related classes with the correct
// ClassLoader. Unfortunately, JNI's FindClass uses the system
// ClassLoader when it is called from a C++ thread, but in Scala (and
// probably other Java environments too), this ClassLoader is not
// enough to locate the Mesos JAR. Instead, we try to capture
// Thread.currentThread()'s context ClassLoader when the Mesos library
// is initialized, in case it has more paths that we can search. We
// store this in mesosClassLoader and access it through
// FindMesosClass(). We initialize the mesosClassLoader variable in
// JNI_OnLoad and uninitialize it in JNI_OnUnLoad (see below).
//
// This code is based on Apache 2 licensed Android code obtained from
// http://android.git.kernel.org/?p=platform/frameworks/base.git;a=blob;f=core/jni/AndroidRuntime.cpp;h=f61e2476c71191aa6eabc93bcb26b3c15ccf6136;hb=HEAD
namespace {

// Initialized in JNI_OnLoad later in this file.
jweak mesosClassLoader = nullptr;

jclass FindMesosClass(JNIEnv* env, const char* className)
{
  if (env->ExceptionCheck()) {
      fprintf(stderr, "ERROR: exception pending on entry to "
                      "FindMesosClass()\n");
      return nullptr;
  }

  if (mesosClassLoader == nullptr) {
    return env->FindClass(className);
  }

  // JNI FindClass uses class names with slashes, but
  // ClassLoader.loadClass uses the dotted "binary name"
  // format. Convert formats.
  string convName = className;
  for (uint32_t i = 0; i < convName.size(); i++) {
    if (convName[i] == '/')
      convName[i] = '.';
  }

  jclass javaLangClassLoader = env->FindClass("java/lang/ClassLoader");
  assert(javaLangClassLoader != nullptr);
  jmethodID loadClass =
    env->GetMethodID(javaLangClassLoader,
                     "loadClass",
                     "(Ljava/lang/String;)Ljava/lang/Class;");
  assert(loadClass != nullptr);

  // Create an object for the class name string; alloc could fail.
  jstring strClassName = env->NewStringUTF(convName.c_str());
  if (env->ExceptionCheck()) {
    fprintf(stderr, "ERROR: unable to convert '%s' to string\n",
            convName.c_str());
    return nullptr;
  }

  // Try to find the named class.
  jclass cls = (jclass) env->CallObjectMethod(mesosClassLoader,
                                              loadClass,
                                              strClassName);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    fprintf(stderr, "ERROR: unable to load class '%s' from %p\n",
            className, mesosClassLoader);
    return nullptr;
  }

  return cls;
}

} // namespace {


// Called by JVM when it loads our library.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* jvm, void* reserved)
{
  // Grab the context ClassLoader of the current thread, if any.
  JNIEnv* env;
  if (jvm->GetEnv(JNIENV_CAST(&env), JNI_VERSION_1_2) != JNI_OK) {
    return JNI_ERR; // JNI version not supported.
  }

  // Find thread's context class loader.
  jclass javaLangThread = env->FindClass("java/lang/Thread");
  assert(javaLangThread != nullptr);

  jclass javaLangClassLoader = env->FindClass("java/lang/ClassLoader");
  assert(javaLangClassLoader != nullptr);

  jmethodID currentThread = env->GetStaticMethodID(
      javaLangThread, "currentThread", "()Ljava/lang/Thread;");
  assert(currentThread != nullptr);

  jmethodID getContextClassLoader = env->GetMethodID(
      javaLangThread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
  assert(getContextClassLoader != nullptr);

  jobject thread = env->CallStaticObjectMethod(javaLangThread, currentThread);
  assert(thread != nullptr);

  jobject classLoader = env->CallObjectMethod(thread, getContextClassLoader);

  if (classLoader != nullptr) {
    mesosClassLoader = env->NewWeakGlobalRef(classLoader);
  }

  // Set the 'loaded' property so we don't try and load it again. This
  // is necessary because a native library can be loaded either with
  // 'System.load' or 'System.loadLibrary' and while redundant calls
  // to 'System.loadLibrary' will be ignored one call of each could
  // cause an error.
  jclass clazz = FindMesosClass(env, "org/apache/mesos/MesosNativeLibrary");
  jfieldID loaded = env->GetStaticFieldID(clazz, "loaded", "Z");
  env->SetStaticBooleanField(clazz, loaded, (jboolean) true);

  return JNI_VERSION_1_2;
}


// Called by JVM when it unloads our library.
JNIEXPORT void JNICALL JNI_OnUnLoad(JavaVM* jvm, void* reserved)
{
  JNIEnv* env;
  if (jvm->GetEnv(JNIENV_CAST(&env), JNI_VERSION_1_2) != JNI_OK) {
    return;
  }

  // TODO(benh): Must we set 'MesosNativeLibrary.loaded' to false?

  if (mesosClassLoader != nullptr) {
    env->DeleteWeakGlobalRef(mesosClassLoader);
    mesosClassLoader = nullptr;
  }
}


template <>
jobject convert(JNIEnv* env, const string& s)
{
  return env->NewStringUTF(s.c_str());
}


template <>
jobject convert(JNIEnv* env, const FrameworkID& frameworkId)
{
  string data;
  frameworkId.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // FrameworkID frameworkId = FrameworkID.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$FrameworkID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$FrameworkID;");

  jobject jframeworkId = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jframeworkId;
}


template <>
jobject convert(JNIEnv* env, const FrameworkInfo& frameworkInfo)
{
  string data;
  frameworkInfo.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // FrameworkInfo frameworkInfo = FrameworkInfo.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$FrameworkInfo");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$FrameworkInfo;");

  jobject jframeworkInfo = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jframeworkInfo;
}


template <>
jobject convert(JNIEnv* env, const MasterInfo& masterInfo)
{
  string data;
  masterInfo.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // MasterInfo masterInfo = MasterInfo.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$MasterInfo");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$MasterInfo;");

  jobject jmasterInfo = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jmasterInfo;
}


template <>
jobject convert(JNIEnv* env, const ExecutorID& executorId)
{
  string data;
  executorId.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // ExecutorID executorId = ExecutorID.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$ExecutorID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$ExecutorID;");

  jobject jexecutorId = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jexecutorId;
}


template <>
jobject convert(JNIEnv* env, const TaskID& taskId)
{
  string data;
  taskId.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // TaskID taskId = TaskID.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$TaskID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$TaskID;");

  jobject jtaskId = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jtaskId;
}


template <>
jobject convert(JNIEnv* env, const SlaveID& slaveId)
{
  string data;
  slaveId.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // SlaveID slaveId = SlaveID.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$SlaveID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$SlaveID;");

  jobject jslaveId = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jslaveId;
}


template <>
jobject convert(JNIEnv* env, const SlaveInfo& slaveInfo)
{
  string data;
  slaveInfo.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // SlaveInfo slaveInfo = SlaveInfo.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$SlaveInfo");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$SlaveInfo;");

  jobject jslaveInfo = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jslaveInfo;
}


template <>
jobject convert(JNIEnv* env, const OfferID& offerId)
{
  string data;
  offerId.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // OfferID offerId = OfferID.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$OfferID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$OfferID;");

  jobject jofferId = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jofferId;
}


template <>
jobject convert(JNIEnv* env, const TaskState& state)
{
  jint jvalue = state;

  // TaskState state = TaskState.valueOf(value);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$TaskState");

  jmethodID valueOf =
    env->GetStaticMethodID(clazz, "valueOf",
                           "(I)Lorg/apache/mesos/Protos$TaskState;");

  jobject jstate = env->CallStaticObjectMethod(clazz, valueOf, jvalue);

  return jstate;
}


template <>
jobject convert(JNIEnv* env, const TaskInfo& task)
{
  string data;
  task.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // TaskInfo task = TaskInfo.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$TaskInfo");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$TaskInfo;");

  jobject jtask = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jtask;
}


template <>
jobject convert(JNIEnv* env, const TaskStatus& status)
{
  string data;
  status.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // TaskStatus status = TaskStatus.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$TaskStatus");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$TaskStatus;");

  jobject jstatus = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jstatus;
}


template <>
jobject convert(JNIEnv* env, const Offer& offer)
{
  string data;
  offer.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // Offer offer = Offer.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$Offer");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$Offer;");

  jobject joffer = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return joffer;
}


template <>
jobject convert(JNIEnv* env, const ExecutorInfo& executor)
{
  string data;
  executor.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // ExecutorInfo executor = ExecutorInfo.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$ExecutorInfo");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$ExecutorInfo;");

  jobject jexecutor = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jexecutor;
}


template <>
jobject convert(JNIEnv* env, const Status& status)
{
  jint jvalue = status;

  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$Status");

  jmethodID valueOf =
    env->GetStaticMethodID(clazz, "valueOf",
                           "(I)Lorg/apache/mesos/Protos$Status;");

  jobject jstate = env->CallStaticObjectMethod(clazz, valueOf, jvalue);

  return jstate;
}


template <>
jobject convert(JNIEnv* env, const v1::scheduler::Event& event)
{
  string data;
  event.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // Event event = Event.parseFrom(data);
  jclass clazz =
    FindMesosClass(env, "org/apache/mesos/v1/scheduler/Protos$Event");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/v1/scheduler/Protos$Event;");

  jobject jevent = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jevent;
}


// Helper to safely return the 'jfieldID' of the given 'name'
// and 'signature'. If the field doesn't exist 'None' is
// returned. If any other JVM Exception is encountered an 'Error'
// is returned and the JVM exception rethrown.
Result<jfieldID> getFieldID(
    JNIEnv* env,
    jclass clazz,
    const char* name,
    const char* signature)
{
  jfieldID jfield = env->GetFieldID(clazz, name, signature);
  jthrowable jexception = env->ExceptionOccurred();
  if (jexception != nullptr) {
    env->ExceptionClear(); // Clear the exception first before proceeding.

    jclass noSuchFieldError = env->FindClass("java/lang/NoSuchFieldError");
    if (env->ExceptionCheck() == JNI_TRUE) {
      return Error("Cannot find NoSuchFieldError class");
    }

    if (!env->IsInstanceOf(jexception, noSuchFieldError)) {
      // We are here if we got a different exception than
      // 'NoSuchFieldError'. Rethrow and bail.
      env->Throw(jexception);
      return Error("Unexpected exception");
    }
    return None(); // The field doesn't exist.
  }

  return jfield;
}
