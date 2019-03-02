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

#include <mesos/state/leveldb.hpp>
#include <mesos/state/state.hpp>

#include "construct.hpp"
#include "convert.hpp"

using std::string;

using mesos::state::LevelDBStorage;
using mesos::state::State;
using mesos::state::Storage;

extern "C" {

/*
 * Class:     org_apache_mesos_state_LevelDBState
 * Method:    initialize
 * Signature: (Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_LevelDBState_initialize
  (JNIEnv* env, jobject thiz, jstring jpath)
{
  string path = construct<string>(env, jpath);

  // Create the C++ Storage and State instances and initialize the
  // __storage and __state variables.
  Storage* storage = new LevelDBStorage(path);
  State* state = new State(storage);

  jclass clazz = env->GetObjectClass(thiz);

  clazz = env->GetSuperclass(clazz);

  jfieldID __storage = env->GetFieldID(clazz, "__storage", "J");
  env->SetLongField(thiz, __storage, (jlong) storage);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");
  env->SetLongField(thiz, __state, (jlong) state);
}

} // extern "C" {
