#include <jni.h>

#include <string>
#include <assert.h>

#include <mesos/mesos.hpp>

#include "convert.hpp"

using namespace mesos;

using std::string;

// Facilities for loading Mesos-related classes with the correct ClassLoader.
// Unfortunately, JNI's FindClass uses the system ClassLoader when it is
// called from a C++ thread, but in Scala (and probably other Java
// environments too), this ClassLoader is not enough to locate mesos.jar.
// Instead, we try to capture Thread.currentThread()'s context ClassLoader
// when the Mesos library is initialized, in case it has more paths that
// we can search. We store this in mesosClassLoader and access it through
// FindMesosClass(). We initialize the mesosClassLoader variable in
// JNI_OnLoad and uninitialize it in JNI_OnUnLoad (see below).
//
// This code is based on Apache 2 licensed Android code obtained from
// http://android.git.kernel.org/?p=platform/frameworks/base.git;a=blob;f=core/jni/AndroidRuntime.cpp;h=f61e2476c71191aa6eabc93bcb26b3c15ccf6136;hb=HEAD
namespace {

jweak mesosClassLoader = NULL; // Initialized in JNI_OnLoad later in this file.


jclass FindMesosClass(JNIEnv* env, const char* className)
{
  if (env->ExceptionCheck()) {
      fprintf(stderr, "ERROR: exception pending on entry to "
                      "FindMesosClass()\n");
      return NULL;
  }

  if (mesosClassLoader == NULL) {
    return env->FindClass(className);
  }

  /*
   * JNI FindClass uses class names with slashes, but ClassLoader.loadClass
   * uses the dotted "binary name" format. Convert formats.
   */
  std::string convName = className;
  for (int i = 0; i < convName.size(); i++) {
    if (convName[i] == '/')
      convName[i] = '.';
  }

  jclass javaLangClassLoader = env->FindClass("java/lang/ClassLoader");
  assert(javaLangClassLoader != NULL);
  jmethodID loadClass = env->GetMethodID(javaLangClassLoader,
    "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  assert(loadClass != NULL);
  jclass cls = NULL;

  /* create an object for the class name string; alloc could fail */
  jstring strClassName = env->NewStringUTF(convName.c_str());
  if (env->ExceptionCheck()) {
    fprintf(stderr, "ERROR: unable to convert '%s' to string\n", convName.c_str());
    goto bail;
  }

  /* try to find the named class */
  cls = (jclass) env->CallObjectMethod(mesosClassLoader, loadClass,
                                       strClassName);
  if (env->ExceptionCheck()) {
    fprintf(stderr, "ERROR: unable to load class '%s' from %p\n",
      className, mesosClassLoader);
    cls = NULL;
    goto bail;
  }

bail:
  return cls;
}

} /* namespace { */


// Called by JVM when it loads our library
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* jvm, void* reserved)
{
  // Grab the context ClassLoader of the current thread, if any

  JNIEnv* env;
  if (jvm->GetEnv((void **)&env, JNI_VERSION_1_2)) {
    return JNI_ERR; /* JNI version not supported */
  }

  jclass javaLangThread, javaLangClassLoader;
  jmethodID currentThread, getContextClassLoader, loadClass;
  jobject thread, classLoader;

  /* find this thread's context class loader; none of this is expected to fail */
  javaLangThread = env->FindClass("java/lang/Thread");
  assert(javaLangThread != NULL);
  javaLangClassLoader = env->FindClass("java/lang/ClassLoader");
  assert(javaLangClassLoader != NULL);
  currentThread = env->GetStaticMethodID(javaLangThread,
    "currentThread", "()Ljava/lang/Thread;");
  getContextClassLoader = env->GetMethodID(javaLangThread,
    "getContextClassLoader", "()Ljava/lang/ClassLoader;");
  assert(currentThread != NULL);
  assert(getContextClassLoader != NULL);
  thread = env->CallStaticObjectMethod(javaLangThread, currentThread);
  assert(thread != NULL);
  classLoader = env->CallObjectMethod(thread, getContextClassLoader);
  if (classLoader != NULL) {
    mesosClassLoader = env->NewWeakGlobalRef(classLoader);
  }

  return JNI_VERSION_1_2;
}


// Called by JVM when it unloads our library
JNIEXPORT void JNICALL JNI_OnUnLoad(JavaVM* jvm, void* reserved)
{
  JNIEnv *env;
  if (jvm->GetEnv((void **)&env, JNI_VERSION_1_2)) {
    return;
  }
  if (mesosClassLoader != NULL) {
    env->DeleteWeakGlobalRef(mesosClassLoader);
    mesosClassLoader = NULL;
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
jobject convert(JNIEnv* env, const TaskDescription& task)
{
  string data;
  task.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // TaskDescription task = TaskDescription.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$TaskDescription");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$TaskDescription;");

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
jobject convert(JNIEnv* env, const SlaveOffer& offer)
{
  string data;
  offer.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // SlaveOffer offer = SlaveOffer.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$SlaveOffer");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$SlaveOffer;");

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
jobject convert(JNIEnv* env, const ExecutorArgs& args)
{
  string data;
  args.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // ExecutorArgs args = ExecutorArgs.parseFrom(data);
  jclass clazz = FindMesosClass(env, "org/apache/mesos/Protos$ExecutorArgs");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom",
                           "([B)Lorg/apache/mesos/Protos$ExecutorArgs;");

  jobject jargs = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jargs;
}
