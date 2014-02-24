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

#include <jni.h>
#include <stdint.h>

#include <process/timeout.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>

#include "log/log.hpp"

#include "construct.hpp"
#include "convert.hpp"
#include "org_apache_mesos_Log.h"
#include "org_apache_mesos_Log_Reader.h"
#include "org_apache_mesos_Log_Writer.h"
#include "org_apache_mesos_Log_Writer.h"

using namespace mesos;
using namespace mesos::internal::log;

using namespace process;


std::string identity(JNIEnv* env, jobject jposition)
{
  jclass clazz = env->GetObjectClass(jposition);
  jfieldID value = env->GetFieldID(clazz, "value", "J");
  jlong jvalue = env->GetLongField(jposition, value);

  uint64_t temp = jvalue; // C++ is expecting an unsigned 64-bit int.

  char bytes[8];
  bytes[0] =(0xff & (temp >> 56));
  bytes[1] = (0xff & (temp >> 48));
  bytes[2] = (0xff & (temp >> 40));
  bytes[3] = (0xff & (temp >> 32));
  bytes[4] = (0xff & (temp >> 24));
  bytes[5] = (0xff & (temp >> 16));
  bytes[6] = (0xff & (temp >> 8));
  bytes[7] = (0xff & temp);

  return std::string(bytes, sizeof(bytes));
}


template <>
jobject convert(JNIEnv* env, const Log::Position& position)
{
  const std::string& identity = position.identity();

  const char* bytes = identity.c_str();

  uint64_t temp =
    ((uint64_t) (bytes[0] & 0xff) << 56) |
    ((uint64_t) (bytes[1] & 0xff) << 48) |
    ((uint64_t) (bytes[2] & 0xff) << 40) |
    ((uint64_t) (bytes[3] & 0xff) << 32) |
    ((uint64_t) (bytes[4] & 0xff) << 24) |
    ((uint64_t) (bytes[5] & 0xff) << 16) |
    ((uint64_t) (bytes[6] & 0xff) << 8) |
    ((uint64_t) (bytes[7] & 0xff));

  jlong jvalue = temp; // Java is expecting a long (i.e., signed).

  // We can create a Java Log.Position directly because JNI does not
  // enforce access modifiers (thus breaking encapsulation).
  jclass clazz = env->FindClass("org/apache/mesos/Log$Position");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "(J)V");

  jobject jposition = env->NewObject(clazz, _init_, jvalue);

  return jposition;
}


template <>
jobject convert(JNIEnv* env, const Log::Entry& entry)
{
  jobject jposition = convert<Log::Position>(env, entry.position);

  // byte[] data = ..;
  jbyteArray jdata = env->NewByteArray(entry.data.size());
  env->SetByteArrayRegion(jdata, 0, entry.data.size(),
                          (jbyte*) entry.data.data());

  // We can create a Java Log.Entry directly because JNI does not
  // enforce access modifiers (thus breaking encapsulation).
  jclass clazz = env->FindClass("org/apache/mesos/Log$Entry");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>",
                                      "(Lorg/apache/mesos/Log$Position;"
                                      "[B)V");

  jobject jentry = env->NewObject(clazz, _init_, jposition, jdata);

  return jentry;
}


extern "C" {

/*
 * Class:     org_apache_mesos_Log_Reader
 * Method:    read
 * Signature: (Lorg/apache/mesos/Log/Position;Lorg/apache/mesos/Log/Position;JLjava/util/concurrent/TimeUnit;)Ljava/util/List;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_Log_00024Reader_read
  (JNIEnv* env,
   jobject thiz,
   jobject jfrom,
   jobject jto,
   jlong jtimeout,
   jobject junit)
{
  // Read out __reader.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __reader = env->GetFieldID(clazz, "__reader", "J");

  Log::Reader* reader = (Log::Reader*) env->GetLongField(thiz, __reader);

  // Also need __log.
  jfieldID __log = env->GetFieldID(clazz, "__log", "J");

  Log* log = (Log*) env->GetLongField(thiz, __log);

  Log::Position from = log->position(identity(env, jfrom));
  Log::Position to = log->position(identity(env, jto));

  clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Result<std::list<Log::Entry> > entries =
    reader->read(from, to, Timeout::in(Seconds(jseconds)));

  if (entries.isError()) {
    clazz = env->FindClass("org/apache/mesos/Log$OperationFailedException");
    env->ThrowNew(clazz, entries.error().c_str());
    return NULL;
  } else if (entries.isNone()) {
    clazz = env->FindClass("java/util/concurrent/TimeoutException");
    env->ThrowNew(clazz, "Timed out while attempting to read");
    return NULL;
  }

  CHECK_SOME(entries);

  // List entries = new ArrayList();
  clazz = env->FindClass("java/util/ArrayList");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject jentries = env->NewObject(clazz, _init_);

  jmethodID add = env->GetMethodID(clazz, "add", "(Ljava/lang/Object;)Z");

  // Loop through C++ list and add each entry to the Java list.
  foreach (const Log::Entry& entry, entries.get()) {
    jobject jentry = convert<Log::Entry>(env, entry);
    env->CallBooleanMethod(jentries, add, jentry);
  }

  return jentries;
}


/*
 * Class:     org_apache_mesos_Log_Reader
 * Method:    beginning
 * Signature: ()Lorg/apache/mesos/Log/Position;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_Log_00024Reader_beginning
  (JNIEnv* env, jobject thiz)
{
  // Read out __reader.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __reader = env->GetFieldID(clazz, "__reader", "J");

  Log::Reader* reader = (Log::Reader*) env->GetLongField(thiz, __reader);

  Log::Position position = reader->beginning();

  return convert<Log::Position>(env, position);
}


/*
 * Class:     org_apache_mesos_Log_Reader
 * Method:    ending
 * Signature: ()Lorg/apache/mesos/Log/Position;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_Log_00024Reader_ending
  (JNIEnv* env, jobject thiz)
{
  // Read out __reader.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __reader = env->GetFieldID(clazz, "__reader", "J");

  Log::Reader* reader = (Log::Reader*) env->GetLongField(thiz, __reader);

  Log::Position position = reader->ending();

  return convert(env, position);
}


/*
 * Class:     org_apache_mesos_Log_Reader
 * Method:    initialize
 * Signature: (Lorg/apache/mesos/Log;)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_00024Reader_initialize
  (JNIEnv* env, jobject thiz, jobject jlog)
{
  // Get log.__log out and store it.
  jclass clazz = env->GetObjectClass(jlog);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");

  Log* log = (Log*) env->GetLongField(jlog, __log);

  clazz = env->GetObjectClass(thiz);

  __log = env->GetFieldID(clazz, "__log", "J");
  env->SetLongField(thiz, __log, (jlong) log);

  // Create the C++ Log::Reader and initialize the __reader variable.
  Log::Reader* reader = new Log::Reader(log);

  jfieldID __reader = env->GetFieldID(clazz, "__reader", "J");
  env->SetLongField(thiz, __reader, (jlong) reader);
}


/*
 * Class:     org_apache_mesos_Log_Reader
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_00024Reader_finalize
  (JNIEnv* env, jobject thiz)
{
  // Read out __reader.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __reader = env->GetFieldID(clazz, "__reader", "J");

  Log::Reader* reader = (Log::Reader*) env->GetLongField(thiz, __reader);

  delete reader;
}


/*
 * Class:     org_apache_mesos_Log_Writer
 * Method:    append
 * Signature: ([BJLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/Log/Position;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_Log_00024Writer_append
  (JNIEnv* env, jobject thiz, jbyteArray jdata, jlong jtimeout, jobject junit)
{
  // Read out __writer.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __writer = env->GetFieldID(clazz, "__writer", "J");

  Log::Writer* writer = (Log::Writer*) env->GetLongField(thiz, __writer);

  jbyte* temp = env->GetByteArrayElements(jdata, NULL);
  jsize length = env->GetArrayLength(jdata);

  std::string data((char*) temp, (size_t) length);

  clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Result<Log::Position> position =
    writer->append(data, Timeout::in(Seconds(jseconds)));

  env->ReleaseByteArrayElements(jdata, temp, 0);

  if (position.isError()) {
    clazz = env->FindClass("org/apache/mesos/Log$WriterFailedException");
    env->ThrowNew(clazz, position.error().c_str());
    return NULL;
  } else if (position.isNone()) {
    clazz = env->FindClass("java/util/concurrent/TimeoutException");
    env->ThrowNew(clazz, "Timed out while attempting to append");
    return NULL;
  }

  CHECK_SOME(position);

  jobject jposition = convert(env, position.get());

  return jposition;
}


/*
 * Class:     org_apache_mesos_Log_Writer
 * Method:    truncate
 * Signature: (Lorg/apache/mesos/Log/Position;JLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/Log/Position;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_Log_00024Writer_truncate
  (JNIEnv* env, jobject thiz, jobject jto, jlong jtimeout, jobject junit)
{
  // Read out __writer.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __writer = env->GetFieldID(clazz, "__writer", "J");

  Log::Writer* writer = (Log::Writer*) env->GetLongField(thiz, __writer);

  // Also need __log.
  jfieldID __log = env->GetFieldID(clazz, "__log", "J");

  Log* log = (Log*) env->GetLongField(thiz, __log);

  Log::Position to = log->position(identity(env, jto));

  clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Result<Log::Position> position =
    writer->truncate(to, Timeout::in(Seconds(jseconds)));

  if (position.isError()) {
    clazz = env->FindClass("org/apache/mesos/Log$WriterFailedException");
    env->ThrowNew(clazz, position.error().c_str());
    return NULL;
  } else if (position.isNone()) {
    clazz = env->FindClass("java/util/concurrent/TimeoutException");
    env->ThrowNew(clazz, "Timed out while attempting to truncate");
    return NULL;
  }

  CHECK_SOME(position);

  jobject jposition = convert(env, position.get());

  return jposition;
}


/*
 * Class:     org_apache_mesos_Log_Writer
 * Method:    initialize
 * Signature: (Lorg/apache/mesos/Log;JLjava/util/concurrent/TimeUnit;I)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_00024Writer_initialize
  (JNIEnv* env,
   jobject thiz,
   jobject jlog,
   jlong jtimeout,
   jobject junit,
   jint jretries)
{
  // Get log.__log out and store it.
  jclass clazz = env->GetObjectClass(jlog);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");

  Log* log = (Log*) env->GetLongField(jlog, __log);

  clazz = env->GetObjectClass(thiz);

  __log = env->GetFieldID(clazz, "__log", "J");
  env->SetLongField(thiz, __log, (jlong) log);

  clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds seconds(jseconds);

  int retries = jretries;

  // Create the C++ Log::Writer and initialize the __writer variable.
  Log::Writer* writer = new Log::Writer(log, seconds, retries);

  clazz = env->GetObjectClass(thiz);

  jfieldID __writer = env->GetFieldID(clazz, "__writer", "J");
  env->SetLongField(thiz, __writer, (jlong) writer);
}


/*
 * Class:     org_apache_mesos_Log_Writer
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_00024Writer_finalize
  (JNIEnv* env, jobject thiz)
{
  // Read out __writer.
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __writer = env->GetFieldID(clazz, "__writer", "J");

  Log::Writer* writer = (Log::Writer*) env->GetLongField(thiz, __writer);

  delete writer;
}


/*
 * Class:     org_apache_mesos_Log
 * Method:    initialize
 * Signature: (ILjava/lang/String;Ljava/util/Set;)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_initialize__ILjava_lang_String_2Ljava_util_Set_2
  (JNIEnv* env,
   jobject thiz,
   jint jquorum,
   jstring jpath,
   jobject jpids)
{
  int quorum = jquorum;

  std::string path = construct<std::string>(env, jpath);

  // Construct a C++ PID from each Java String.
  std::set<UPID> pids;

  jclass clazz = env->GetObjectClass(jpids);

  // Iterator iterator = pids.iterator();
  jmethodID iterator =
    env->GetMethodID(clazz, "iterator", "()Ljava/util/Iterator;");
  jobject jiterator = env->CallObjectMethod(jpids, iterator);

  clazz = env->GetObjectClass(jiterator);

  // while (iterator.hasNext()) {
  jmethodID hasNext = env->GetMethodID(clazz, "hasNext", "()Z");

  jmethodID next = env->GetMethodID(clazz, "next", "()Ljava/lang/Object;");

  while (env->CallBooleanMethod(jiterator, hasNext)) {
    // String s = (String) iterator.next();
    jstring js = (jstring) env->CallObjectMethod(jiterator, next);
    std::string s = construct<std::string>(env, js);
    UPID pid(s);
    CHECK(pid) << "Failed to parse '" << s << "'";
    pids.insert(pid);
  }

  // Create the C++ Log and initialize the __log variable.
  Log* log = new Log(quorum, path, pids);

  clazz = env->GetObjectClass(thiz);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");
  env->SetLongField(thiz, __log, (jlong) log);
}


/*
 * Class:     org_apache_mesos_Log
 * Method:    initialize
 * Signature: (ILjava/lang/String;Ljava/lang/String;JLjava/util/concurrent/TimeUnit;Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_initialize__ILjava_lang_String_2Ljava_lang_String_2JLjava_util_concurrent_TimeUnit_2Ljava_lang_String_2
  (JNIEnv* env,
   jobject thiz,
   jint jquorum,
   jstring jpath,
   jstring jservers,
   jlong jtimeout,
   jobject junit,
   jstring jznode)
{
  int quorum = jquorum;

  std::string path = construct<std::string>(env, jpath);

  std::string servers = construct<std::string>(env, jservers);

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds seconds(jseconds);

  std::string znode = construct<std::string>(env, jznode);

   // Create the C++ Log and initialize the __log variable.
  Log* log = new Log(quorum, path, servers, seconds, znode);

  clazz = env->GetObjectClass(thiz);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");
  env->SetLongField(thiz, __log, (jlong) log);
}


/*
 * Class:     org_apache_mesos_Log
 * Method:    initialize
 * Signature: (ILjava/lang/String;Ljava/lang/String;JLjava/util/concurrent/TimeUnit;Ljava/lang/String;Ljava/lang/String;[B)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_initialize__ILjava_lang_String_2Ljava_lang_String_2JLjava_util_concurrent_TimeUnit_2Ljava_lang_String_2Ljava_lang_String_2_3B
  (JNIEnv* env,
   jobject thiz,
   jint jquorum,
   jstring jpath,
   jstring jservers,
   jlong jtimeout,
   jobject junit,
   jstring jznode,
   jstring jscheme,
   jbyteArray jcredentials)
{
  int quorum = jquorum;

  std::string path = construct<std::string>(env, jpath);

  std::string servers = construct<std::string>(env, jservers);

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds seconds(jseconds);

  std::string znode = construct<std::string>(env, jznode);

  // Create the C++ Log.
  Log* log = NULL;

  if (jscheme != NULL && jcredentials != NULL) {
    std::string scheme = construct<std::string>(env, jscheme);

    jbyte* temp = env->GetByteArrayElements(jcredentials, NULL);
    jsize length = env->GetArrayLength(jcredentials);

    std::string credentials((char*) temp, (size_t) length);

    env->ReleaseByteArrayElements(jcredentials, temp, 0);

    zookeeper::Authentication authentication(scheme, credentials);

    log = new Log(quorum, path, servers, seconds, znode, authentication);
  } else {
    log = new Log(quorum, path, servers, seconds, znode);
  }

  CHECK(log != NULL);

  // Initialize the __log variable.
  clazz = env->GetObjectClass(thiz);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");
  env->SetLongField(thiz, __log, (jlong) log);
}


/*
 * Class:     org_apache_mesos_Log
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_Log_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");

  Log* log = (Log*) env->GetLongField(thiz, __log);

  delete log;
}

} // extern "C" {
