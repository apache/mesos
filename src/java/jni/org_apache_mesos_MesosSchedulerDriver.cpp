#include <string>
#include <map>
#include <vector>

#include "construct.hpp"
#include "convert.hpp"
#include "foreach.hpp"
#include "org_apache_mesos_MesosSchedulerDriver.h"
#include "mesos_sched.hpp"

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
  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const vector<SlaveOffer>& offers);
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

  string name;

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

  if (!env->ExceptionOccurred()) {
    name = construct<string>(env, (jstring) jname);
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }

  return name;
}


ExecutorInfo JNIScheduler::getExecutorInfo(SchedulerDriver* driver)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  ExecutorInfo executor;

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

  if (!env->ExceptionOccurred()) {
    executor = construct<ExecutorInfo>(env, jexecutor);
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    this->error(driver, -1, "Java exception caught");
  }

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


void JNIScheduler::resourceOffer(SchedulerDriver* driver,
                                 const OfferID& offerId,
                                 const vector<SlaveOffer>& offers)
{
  jvm->AttachCurrentThread((void**) &env, NULL);

  jclass clazz = env->GetObjectClass(jdriver);

  jfieldID sched = env->GetFieldID(clazz, "sched", "Lorg/apache/mesos/Scheduler;");
  jobject jsched = env->GetObjectField(jdriver, sched);

  clazz = env->GetObjectClass(jsched);

  // sched.resourceOffer(driver, offerId, offers);
  jmethodID resourceOffer =
    env->GetMethodID(clazz, "resourceOffer",
		     "(Lorg/apache/mesos/SchedulerDriver;"
		     "Lorg/apache/mesos/Protos$OfferID;"
		     "Ljava/util/List;)V");

  jobject jofferId = convert<OfferID>(env, offerId);

  // List offers = new ArrayList();
  clazz = env->FindClass("java/util/ArrayList");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject joffers = env->NewObject(clazz, _init_);

  jmethodID add = env->GetMethodID(clazz, "add", "(Ljava/lang/Object;)Z");
    
  // Loop through C++ vector and add each offer to the Java vector.
  foreach (const SlaveOffer& offer, offers) {
    jobject joffer = convert<SlaveOffer>(env, offer);
    env->CallBooleanMethod(joffers, add, joffer);
  }

  env->ExceptionClear();

  env->CallVoidMethod(jsched, resourceOffer, jdriver, jofferId, joffers);

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

  if (!env->ExceptionOccurred()) {
    jvm->DetachCurrentThread();
  } else {
    env->ExceptionDescribe();
    env->ExceptionClear();
    jvm->DetachCurrentThread();
    driver->stop();
    // Don't call error recursively here!
  }
}


#ifdef __cplusplus
extern "C" {
#endif

/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    initialize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_MesosSchedulerDriver_initialize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  // Create a global reference to the MesosSchedulerDriver instance.
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
  
  // Create the C++ driver and initialize the __driver variable.
  MesosSchedulerDriver* driver =
    new MesosSchedulerDriver(sched,
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
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  // Call stop just in case.
  driver->stop();
  driver->join();

  delete driver;

  jfieldID __sched = env->GetFieldID(clazz, "__sched", "J");
  JNIScheduler* sched = (JNIScheduler*)
    env->GetLongField(thiz, __sched);

  env->DeleteWeakGlobalRef(sched->jdriver);

  delete sched;
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    start
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_org_apache_mesos_MesosSchedulerDriver_start
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  return driver->start();
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    stop
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_org_apache_mesos_MesosSchedulerDriver_stop
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  return driver->stop();
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    join
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_org_apache_mesos_MesosSchedulerDriver_join
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  return driver->join();
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    sendFrameworkMessage
 * Signature: (Lorg/apache/mesos/Protos/SlaveID;Lorg/apache/mesos/Protos/ExecutorID;[B)I
 */
JNIEXPORT jint JNICALL Java_org_apache_mesos_MesosSchedulerDriver_sendFrameworkMessage
  (JNIEnv* env, jobject thiz, jobject jslaveId, jobject jexecutorId, jbyteArray jdata)
{
  // Construct a C++ SlaveID from the Java SlaveID.
  const SlaveID& slaveId = construct<SlaveID>(env, jslaveId);

  // Construct a C++ ExecutorID from the Java ExecutorID.
  const ExecutorID& executorId = construct<ExecutorID>(env, jexecutorId);

  // Construct a C++ string from the Java byte array.
  string data((char*) env->GetByteArrayElements(jdata, NULL),
	      (size_t) env->GetArrayLength(jdata));

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  return driver->sendFrameworkMessage(slaveId, executorId, data);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    killTask
 * Signature: (Lorg/apache/mesos/Protos$TaskID;)I
 */
JNIEXPORT jint JNICALL Java_org_apache_mesos_MesosSchedulerDriver_killTask
  (JNIEnv* env, jobject thiz, jobject jtaskId)
{
  // Construct a C++ TaskID from the Java TaskId.
  const TaskID& taskId = construct<TaskID>(env, jtaskId);

  // Now invoke the underlying driver.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  return driver->killTask(taskId);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    replyToOffer
 * Signature: (Lorg/apache/mesos/Protos$OfferID;Ljava/util/Collection;Ljava/util/Map;)I
 */
JNIEXPORT jint JNICALL Java_org_apache_mesos_MesosSchedulerDriver_replyToOffer
  (JNIEnv* env, jobject thiz, jobject jofferId, jobject jtasks, jobject jparams)
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

  // Construct a C++ map from the Java map.
  const map<string, string>& params =
    construct< map<string, string> >(env, jparams);

  // Now invoke the underlying driver.
  clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  return driver->replyToOffer(offerId, tasks, params);
}


/*
 * Class:     org_apache_mesos_MesosSchedulerDriver
 * Method:    reviveOffers
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_org_apache_mesos_MesosSchedulerDriver_reviveOffers
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __driver = env->GetFieldID(clazz, "__driver", "J");
  MesosSchedulerDriver* driver = (MesosSchedulerDriver*)
    env->GetLongField(thiz, __driver);

  return driver->reviveOffers();
}


#ifdef __cplusplus
}
#endif
