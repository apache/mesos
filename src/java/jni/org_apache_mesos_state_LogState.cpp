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

#include <mesos/log/log.hpp>

#include <mesos/state/log.hpp>
#include <mesos/state/state.hpp>

#include <stout/duration.hpp>

#include "construct.hpp"
#include "convert.hpp"

using std::string;

using mesos::log::Log;

using mesos::state::LogStorage;
using mesos::state::State;
using mesos::state::Storage;

extern "C" {

/*
 * Class:     org_apache_mesos_state_LogState
 * Method:    initialize
 * Signature: (Ljava/lang/String;JLjava/util/concurrent/TimeUnit;Ljava/lang/String;JLjava/lang/String;I)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_LogState_initialize
  (JNIEnv* env,
   jobject thiz,
   jstring jservers,
   jlong jtimeout,
   jobject junit,
   jstring jznode,
   jlong jquorum,
   jstring jpath,
   jint jdiffsBetweenSnapshots)
{
  string servers = construct<string>(env, jservers);

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds timeout(jseconds);

  string znode = construct<string>(env, jznode);

  long quorum = jquorum;

  string path = construct<string>(env, jpath);

  // Create the Log instance using ZooKeeper.
  Log* log = new Log(quorum, path, servers, timeout, znode);

  // Create the C++ Storage and State instances and initialize the
  // __storage and __state variables.
  int diffsBetweenSnapshots = jdiffsBetweenSnapshots;

  Storage* storage = new LogStorage(log, diffsBetweenSnapshots);
  State* state = new State(storage);

  clazz = env->GetObjectClass(thiz);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");
  env->SetLongField(thiz, __log, (jlong) log);

  clazz = env->GetSuperclass(clazz);

  jfieldID __storage = env->GetFieldID(clazz, "__storage", "J");
  env->SetLongField(thiz, __storage, (jlong) storage);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");
  env->SetLongField(thiz, __state, (jlong) state);
}


/*
 * Class:     org_apache_mesos_state_LogState
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_LogState_finalize
  (JNIEnv* env, jobject thiz)
{
  // TODO(benh): Consider calling 'finalize' on the super class
  // instead of deleting 'state' and 'storage' here manually. We'd
  // still need to delete 'log' ourselves, of course.

  jclass clazz = env->GetObjectClass(thiz);

  clazz = env->GetSuperclass(clazz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State* state = (State*) env->GetLongField(thiz, __state);

  delete state;

  jfieldID __storage = env->GetFieldID(clazz, "__storage", "J");

  Storage* storage = (Storage*) env->GetLongField(thiz, __storage);

  delete storage;

  clazz = env->GetObjectClass(thiz);

  jfieldID __log = env->GetFieldID(clazz, "__log", "J");

  Log* log = (Log*) env->GetLongField(thiz, __log);

  delete log;
}

} // extern "C" {
