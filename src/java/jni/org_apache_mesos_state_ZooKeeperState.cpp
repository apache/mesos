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

#include <mesos/state/state.hpp>
#include <mesos/state/zookeeper.hpp>

#include <stout/duration.hpp>

#include "construct.hpp"
#include "convert.hpp"

using std::string;

using mesos::state::State;
using mesos::state::Storage;
using mesos::state::ZooKeeperStorage;

extern "C" {

/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    initialize
 * Signature: (Ljava/lang/String;JLjava/util/concurrent/TimeUnit;Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_ZooKeeperState_initialize__Ljava_lang_String_2JLjava_util_concurrent_TimeUnit_2Ljava_lang_String_2( // NOLINT(whitespace/line_length)
    JNIEnv* env,
    jobject thiz,
    jstring jservers,
    jlong jtimeout,
    jobject junit,
    jstring jznode)
{
  string servers = construct<string>(env, jservers);

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds timeout(jseconds);

  string znode = construct<string>(env, jznode);

  // Create the C++ Storage and State instances and initialize the
  // __storage and __state variables.
  Storage* storage = new ZooKeeperStorage(servers, timeout, znode);
  State* state = new State(storage);

  clazz = env->GetObjectClass(thiz);

  clazz = env->GetSuperclass(clazz);

  jfieldID __storage = env->GetFieldID(clazz, "__storage", "J");
  env->SetLongField(thiz, __storage, (jlong) storage);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");
  env->SetLongField(thiz, __state, (jlong) state);
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    initialize
 * Signature: (Ljava/lang/String;JLjava/util/concurrent/TimeUnit;Ljava/lang/String;Ljava/lang/String;[B)V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_ZooKeeperState_initialize__Ljava_lang_String_2JLjava_util_concurrent_TimeUnit_2Ljava_lang_String_2Ljava_lang_String_2_3B( // NOLINT(whitespace/line_length)
    JNIEnv* env,
    jobject thiz,
    jstring jservers,
    jlong jtimeout,
    jobject junit,
    jstring jznode,
    jstring jscheme,
    jbyteArray jcredentials)
{
  string servers = construct<string>(env, jservers);

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds timeout(jseconds);

  string znode = construct<string>(env, jznode);

  // Create the C++ State.
  Storage* storage = nullptr;
  if (jscheme != nullptr && jcredentials != nullptr) {
    string scheme = construct<string>(env, jscheme);

    jbyte* temp = env->GetByteArrayElements(jcredentials, nullptr);
    jsize length = env->GetArrayLength(jcredentials);

    string credentials((char*) temp, (size_t) length);

    env->ReleaseByteArrayElements(jcredentials, temp, 0);

    zookeeper::Authentication authentication(scheme, credentials);

    storage = new ZooKeeperStorage(servers, timeout, znode, authentication);
  } else {
    storage = new ZooKeeperStorage(servers, timeout, znode);
  }

  CHECK(storage != nullptr);

  State* state = new State(storage);

  // Initialize the __storage and __state variables.
  clazz = env->GetObjectClass(thiz);

  jfieldID __storage = env->GetFieldID(clazz, "__storage", "J");
  env->SetLongField(thiz, __storage, (jlong) storage);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");
  env->SetLongField(thiz, __state, (jlong) state);
}

} // extern "C" {
