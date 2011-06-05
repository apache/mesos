#include <string>

#include "construct.hpp"
#include "convert.hpp"
#include "mesos_MesosExecutorDriver.h"
#include "mesos_exec.hpp"

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
  virtual void frameworkMessage(ExecutorDriver* driver, const FrameworkMessage& message);
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

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lmesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.init(driver);
  jmethodID init =
    env->GetMethodID(clazz, "init", "(Lmesos/ExecutorDriver;Lmesos/Protos$ExecutorArgs;)V");

  jobject jargs = convert<ExecutorArgs>(env, args);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, init, jdriver, jargs);

  if (!env->ExceptionOccurred()) {
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }
}


void JNIExecutor::launchTask(ExecutorDriver* driver, const TaskDescription& desc)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lmesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.launchTask(driver, desc);
  jmethodID launchTask = env->GetMethodID(clazz, "launchTask",
    "(Lmesos/ExecutorDriver;Lmesos/Protos$TaskDescription;)V");

  jobject jdesc = convert<TaskDescription>(env, desc);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, launchTask, jdriver, jdesc);

  if (!env->ExceptionOccurred()) {
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }
}


void JNIExecutor::killTask(ExecutorDriver* driver, const TaskID& taskId)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lmesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.killTask(driver, taskId);
  jmethodID killTask = env->GetMethodID(clazz, "killTask",
    "(Lmesos/ExecutorDriver;Lmesos/Protos$TaskID;)V");

  jobject jtaskId = convert<TaskID>(env, taskId);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, killTask, jdriver, jtaskId);

  if (!env->ExceptionOccurred()) {
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }
}


void JNIExecutor::frameworkMessage(ExecutorDriver* driver, const FrameworkMessage& message)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lmesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.frameworkMessage(driver, message);
  jmethodID frameworkMessage = env->GetMethodID(clazz, "frameworkMessage",
    "(Lmesos/ExecutorDriver;Lmesos/Protos$FrameworkMessage;)V");

  jobject jmessage = convert<FrameworkMessage>(env, message);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, frameworkMessage, jdriver, jmessage);

  if (!env->ExceptionOccurred()) {
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }
}


void JNIExecutor::shutdown(ExecutorDriver* driver)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lmesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.shutdown(driver);
  jmethodID shutdown =
    env->GetMethodID(clazz, "shutdown", "(Lmesos/ExecutorDriver;)V");

  env->ExceptionClear();

  env->CallVoidMethod(jexec, shutdown, jdriver);

  if (!env->ExceptionOccurred()) {
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }
}


void JNIExecutor::error(ExecutorDriver* driver, int code, const string& message)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID exec = env->GetFieldID(clazz, "exec", "Lmesos/Executor;");
  jobject jexec = env->GetObjectField(jdriver, exec);

  clazz = env->GetObjectClass(jexec);

  // exec.error(driver, code, message);
  jmethodID error = env->GetMethodID(clazz, "error",
    "(Lmesos/ExecutorDriver;ILjava/lang/String;)V");

  jint jcode = code;
  jobject jmessage = convert<string>(env, message);

  env->ExceptionClear();

  env->CallVoidMethod(jexec, error, jdriver, jcode, jmessage);

  if (!env->ExceptionOccurred()) {
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }
}


#ifdef __cplusplus
extern "C" {
#endif

/*
 * Class:     mesos_MesosExecutorDriver
 * Method:    initialize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_mesos_MesosExecutorDriver_initialize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  // Create a global reference to the MesosExecutorDriver instance.
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
 * Class:     mesos_MesosExecutorDriver
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_mesos_MesosExecutorDriver_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver = (MesosExecutorDriver*)
    env->GetLongField(thiz, __driver);

  // Call stop just in case.
  driver->stop();
  driver->join();

  delete driver;

  jfieldID __exec = env->GetFieldID(clazz, "__exec", "J");
  JNIExecutor* exec = (JNIExecutor*)
    env->GetLongField(thiz, __exec);

  env->DeleteWeakGlobalRef(exec->jdriver);

  delete exec;
}


/*
 * Class:     mesos_MesosExecutorDriver
 * Method:    start
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_mesos_MesosExecutorDriver_start
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver = (MesosExecutorDriver*)
    env->GetLongField(thiz, __driver);

  return driver->start();
}


/*
 * Class:     mesos_MesosExecutorDriver
 * Method:    stop
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_mesos_MesosExecutorDriver_stop
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver = (MesosExecutorDriver*)
    env->GetLongField(thiz, __driver);

  return driver->stop();
}


/*
 * Class:     mesos_MesosExecutorDriver
 * Method:    join
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_mesos_MesosExecutorDriver_join
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver = (MesosExecutorDriver*)
    env->GetLongField(thiz, __driver);

  return driver->join();
}


/*
 * Class:     mesos_MesosExecutorDriver
 * Method:    sendStatusUpdate
 * Signature: (Lmesos/Protos$TaskStatus;)I
 */
JNIEXPORT jint JNICALL Java_mesos_MesosExecutorDriver_sendStatusUpdate
  (JNIEnv* env, jobject thiz, jobject jstatus)
{
  // Construct a C++ TaskStatus from the Java TaskStatus.
  const TaskStatus& status = construct<TaskStatus>(env, jstatus);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver = (MesosExecutorDriver*)
    env->GetLongField(thiz, __driver);

  return driver->sendStatusUpdate(status);
}


/*
 * Class:     mesos_MesosExecutorDriver
 * Method:    sendFrameworkMessage
 * Signature: (Lmesos/Protos$FrameworkMessage;)I
 */
JNIEXPORT jint JNICALL Java_mesos_MesosExecutorDriver_sendFrameworkMessage
  (JNIEnv* env, jobject thiz, jobject jmessage)
{
  // Construct a C++ FrameworkMessage from the Java FrameworkMessage.
  const FrameworkMessage& message = construct<FrameworkMessage>(env, jmessage);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosExecutorDriver* driver = (MesosExecutorDriver*)
    env->GetLongField(thiz, __driver);

  return driver->sendFrameworkMessage(message);
}


#ifdef __cplusplus
}
#endif
