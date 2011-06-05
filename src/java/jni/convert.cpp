#include <jni.h>

#include <string>

#include "convert.hpp"
#include "mesos_exec.hpp"
#include "mesos_sched.hpp"

using namespace mesos;

using std::string;


template <>
jobject convert(JNIEnv *env, const string &s)
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
  jclass clazz = env->FindClass("mesos/Protos$FrameworkID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$FrameworkID;");

  jobject jframeworkId = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jframeworkId;
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
  jclass clazz = env->FindClass("mesos/Protos$TaskID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$TaskID;");

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
  jclass clazz = env->FindClass("mesos/Protos$SlaveID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$SlaveID;");

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
  jclass clazz = env->FindClass("mesos/Protos$OfferID");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$OfferID;");

  jobject jofferId = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jofferId;
}


template <>
jobject convert(JNIEnv* env, const TaskState& state)
{
  jint jvalue = state;

  // TaskState state = TaskState.valueOf(value);
  jclass clazz = env->FindClass("mesos/Protos$TaskState");

  jmethodID valueOf =
    env->GetStaticMethodID(clazz, "valueOf", "(I)Lmesos/Protos$TaskState;");

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
  jclass clazz = env->FindClass("mesos/Protos$TaskDescription");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$TaskDescription;");

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
  jclass clazz = env->FindClass("mesos/Protos$TaskStatus");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$TaskStatus;");

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
  jclass clazz = env->FindClass("mesos/Protos$SlaveOffer");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$SlaveOffer;");

  jobject joffer = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return joffer;
}


template <>
jobject convert(JNIEnv* env, const FrameworkMessage& message)
{
  string data;
  message.SerializeToString(&data);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(data.size());
  env->SetByteArrayRegion(jdata, 0, data.size(), (jbyte*) data.data());

  // FrameworkMessage message = FrameworkMessage.parseFrom(data);
  jclass clazz = env->FindClass("mesos/Protos$FrameworkMessage");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$FrameworkMessage;");

  jobject jmessage = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jmessage;
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
  jclass clazz = env->FindClass("mesos/Protos$ExecutorInfo");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$ExecutorInfo;");

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
  jclass clazz = env->FindClass("mesos/Protos$ExecutorArgs");

  jmethodID parseFrom =
    env->GetStaticMethodID(clazz, "parseFrom", "([B)Lmesos/Protos$ExecutorArgs;");

  jobject jargs = env->CallStaticObjectMethod(clazz, parseFrom, jdata);

  return jargs;
}
