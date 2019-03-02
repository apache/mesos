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

#include <set>
#include <string>

#include <mesos/state/state.hpp>

#include <process/check.hpp>
#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>

#include "construct.hpp"
#include "convert.hpp"

using process::Future;

using std::set;
using std::string;

using mesos::state::State;
using mesos::state::Storage;
using mesos::state::Variable;

extern "C" {

// TODO(jmlvanre): Deprecate the JNI functions that are in the AbstractState
// scope that we have replaced with the ones in the names classes 'FetchFuture',
// 'StoreFuture', 'ExpungeFuture', and 'NamesFuture'. (MESOS-2161). The
// anonymous futures that used these function accidentally captured the 'thiz'
// for the 'AbstractState' class, which was not intended and caused the C++
// Future to be prematurely deleted as a result of the anonymous Future getting
// garbage collected by the JVM and invoking the finalizer which deleted the
// C++ Future. The intent was to capture the 'thiz' for the 'Future'. This is
// done correctly by using the named inner classes.

/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_AbstractState_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State* state = (State*) env->GetLongField(thiz, __state);

  delete state;

  jfieldID __storage = env->GetFieldID(clazz, "__storage", "J");

  Storage* storage = (Storage*) env->GetLongField(thiz, __storage);

  delete storage;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __fetch
 * Signature: (Ljava/lang/String;)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_mesos_state_AbstractState__1_1fetch
  (JNIEnv* env, jobject thiz, jstring jname)
{
  string name = construct<string>(env, jname);

  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State* state = (State*) env->GetLongField(thiz, __state);

  Future<Variable>* future = new Future<Variable>(state->fetch(name));

  return (jlong) future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __fetch_cancel
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1fetch_1cancel(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable>* future = (Future<Variable>*) jfuture;

  // We'll initiate a discard but we won't consider it cancelled since
  // we don't know if/when the future will get discarded.
  future->discard();

  return (jboolean) false;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __fetch_is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1fetch_1is_1cancelled(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  // We always return false since while we might discard the future in
  // 'cancel' we don't know if it has really been discarded and we
  // don't want this function to block. We choose to be deterministic
  // here and always return false rather than sometimes returning true
  // if the future has completed (been discarded or otherwise).
  return (jboolean) false;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __fetch_is_done
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1fetch_1is_1done(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable>* future = (Future<Variable>*) jfuture;

  return (jboolean) !future->isPending() || future->hasDiscard();
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __fetch_get
 * Signature: (J)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1fetch_1get(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable>* future = (Future<Variable>*) jfuture;

  future->await();

  if (future->isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future->failure().c_str());
    return nullptr;
  } else if (future->isDiscarded()) {
    // TODO(benh): Consider throwing an ExecutionException since we
    // never return true for 'isCancelled'.
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
    return nullptr;
  }

  CHECK_READY(*future);

  Variable* variable = new Variable(future->get());

  // Variable variable = new Variable();
  jclass clazz = env->FindClass("org/apache/mesos/state/Variable");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject jvariable = env->NewObject(clazz, _init_);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
  env->SetLongField(jvariable, __variable, (jlong) variable);

  return jvariable;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __fetch_get_timeout
 * Signature: (JJLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1fetch_1get_1timeout(
    JNIEnv* env, jobject thiz, jlong jfuture, jlong jtimeout, jobject junit)
{
  Future<Variable>* future = (Future<Variable>*) jfuture;

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds seconds(jseconds);

  if (future->await(seconds)) {
    if (future->isFailed()) {
      clazz = env->FindClass("java/util/concurrent/ExecutionException");
      env->ThrowNew(clazz, future->failure().c_str());
      return nullptr;
    } else if (future->isDiscarded()) {
      // TODO(benh): Consider throwing an ExecutionException since we
      // never return true for 'isCancelled'.
      clazz = env->FindClass("java/util/concurrent/CancellationException");
      env->ThrowNew(clazz, "Future was discarded");
      return nullptr;
    }

    CHECK_READY(*future);
    Variable* variable = new Variable(future->get());

    // Variable variable = new Variable();
    clazz = env->FindClass("org/apache/mesos/state/Variable");

    jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
    jobject jvariable = env->NewObject(clazz, _init_);

    jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
    env->SetLongField(jvariable, __variable, (jlong) variable);

    return jvariable;
  }

  clazz = env->FindClass("java/util/concurrent/TimeoutException");
  env->ThrowNew(clazz, "Failed to wait for future within timeout");

  return nullptr;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __fetch_finalize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState__1_1fetch_1finalize(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable>* future = (Future<Variable>*) jfuture;

  delete future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState$FetchFuture
 * Method:    cancel
 * Signature: (Z)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024FetchFuture_cancel(
    JNIEnv* env, jobject thiz, jboolean mayInterruptIfRunning)
{
  if (mayInterruptIfRunning) {
    static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
    static jfieldID field = env->GetFieldID(clazz, "future", "J");
    jlong jfuture = env->GetLongField(thiz, field);

    // See TODO at top of file for why we proxy this call.
    return Java_org_apache_mesos_state_AbstractState__1_1fetch_1cancel(
        env,
        thiz,
        jfuture);
  }

  return false; // Should not interrupt and already running (or finished).
}


/*
 * Class:     org_apache_mesos_state_AbstractState$FetchFuture
 * Method:    is_cancelled
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024FetchFuture_is_1cancelled(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1fetch_1is_1cancelled(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$FetchFuture
 * Method:    is_done
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024FetchFuture_is_1done(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1fetch_1is_1done(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$FetchFuture
 * Method:    get
 * Signature: ()Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024FetchFuture_get(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1fetch_1get(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$FetchFuture
 * Method:    get_timeout
 * Signature: (JLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024FetchFuture_get_1timeout(
    JNIEnv* env, jobject thiz, jlong jtimeout, jobject junit)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1fetch_1get_1timeout(
      env,
      thiz,
      jfuture,
      jtimeout,
      junit);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$FetchFuture
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState_00024FetchFuture_finalize(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  Java_org_apache_mesos_state_AbstractState__1_1fetch_1finalize(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __store
 * Signature: (Lorg/apache/mesos/state/Variable;)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_mesos_state_AbstractState__1_1store
  (JNIEnv* env, jobject thiz, jobject jvariable)
{
  jclass clazz = env->GetObjectClass(jvariable);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable* variable = (Variable*) env->GetLongField(jvariable, __variable);

  clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State* state = (State*) env->GetLongField(thiz, __state);

  Future<Option<Variable>>* future =
    new Future<Option<Variable>>(state->store(*variable));

  return (jlong) future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __store_cancel
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1store_1cancel(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable>>* future = (Future<Option<Variable>>*) jfuture;

  // We'll initiate a discard but we won't consider it cancelled since
  // we don't know if/when the future will get discarded.
  future->discard();

  return (jboolean) false;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __store_is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1store_1is_1cancelled(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  // We always return false since while we might discard the future in
  // 'cancel' we don't know if it has really been discarded and we
  // don't want this function to block. We choose to be deterministic
  // here and always return false rather than sometimes returning true
  // if the future has completed (been discarded or otherwise).
  return (jboolean) false;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __store_is_done
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1store_1is_1done(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable>>* future = (Future<Option<Variable>>*) jfuture;

  return (jboolean) !future->isPending() || future->hasDiscard();
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __store_get
 * Signature: (J)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1store_1get(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable>>* future = (Future<Option<Variable>>*) jfuture;

  future->await();

  if (future->isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future->failure().c_str());
    return nullptr;
  } else if (future->isDiscarded()) {
    // TODO(benh): Consider throwing an ExecutionException since we
    // never return true for 'isCancelled'.
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
    return nullptr;
  }

  CHECK_READY(*future);

  if (future->get().isSome()) {
    Variable* variable = new Variable(future->get().get());

    // Variable variable = new Variable();
    jclass clazz = env->FindClass("org/apache/mesos/state/Variable");

    jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
    jobject jvariable = env->NewObject(clazz, _init_);

    jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
    env->SetLongField(jvariable, __variable, (jlong) variable);

    return jvariable;
  }

  return nullptr;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __store_get_timeout
 * Signature: (JJLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1store_1get_1timeout(
    JNIEnv* env, jobject thiz, jlong jfuture, jlong jtimeout, jobject junit)
{
  Future<Option<Variable>>* future = (Future<Option<Variable>>*) jfuture;

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds seconds(jseconds);

  if (future->await(seconds)) {
    if (future->isFailed()) {
      clazz = env->FindClass("java/util/concurrent/ExecutionException");
      env->ThrowNew(clazz, future->failure().c_str());
      return nullptr;
    } else if (future->isDiscarded()) {
      // TODO(benh): Consider throwing an ExecutionException since we
      // never return true for 'isCancelled'.
      clazz = env->FindClass("java/util/concurrent/CancellationException");
      env->ThrowNew(clazz, "Future was discarded");
      return nullptr;
    }

    CHECK_READY(*future);

    if (future->get().isSome()) {
      Variable* variable = new Variable(future->get().get());

      // Variable variable = new Variable();
      clazz = env->FindClass("org/apache/mesos/state/Variable");

      jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
      jobject jvariable = env->NewObject(clazz, _init_);

      jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
      env->SetLongField(jvariable, __variable, (jlong) variable);

      return jvariable;
    }

    return nullptr;
  }

  clazz = env->FindClass("java/util/concurrent/TimeoutException");
  env->ThrowNew(clazz, "Failed to wait for future within timeout");

  return nullptr;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __store_finalize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState__1_1store_1finalize(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable>>* future = (Future<Option<Variable>>*) jfuture;

  delete future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState$StoreFuture
 * Method:    cancel
 * Signature: (Z)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024StoreFuture_cancel(
    JNIEnv* env, jobject thiz, jboolean mayInterruptIfRunning)
{
  if (mayInterruptIfRunning) {
    static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
    static jfieldID field = env->GetFieldID(clazz, "future", "J");
    jlong jfuture = env->GetLongField(thiz, field);

    // See TODO at top of file for why we proxy this call.
    return Java_org_apache_mesos_state_AbstractState__1_1store_1cancel(
        env,
        thiz,
        jfuture);
  }

  return false; // Should not interrupt and already running (or finished).
}


/*
 * Class:     org_apache_mesos_state_AbstractState$StoreFuture
 * Method:    is_cancelled
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024StoreFuture_is_1cancelled(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1store_1is_1cancelled(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$StoreFuture
 * Method:    is_done
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024StoreFuture_is_1done(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1store_1is_1done(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$StoreFuture
 * Method:    get
 * Signature: ()Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024StoreFuture_get(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1store_1get(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$StoreFuture
 * Method:    get_timeout
 * Signature: (JLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024StoreFuture_get_1timeout(
    JNIEnv* env, jobject thiz, jlong jtimeout, jobject junit)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1store_1get_1timeout(
      env,
      thiz,
      jfuture,
      jtimeout,
      junit);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$StoreFuture
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState_00024StoreFuture_finalize(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  Java_org_apache_mesos_state_AbstractState__1_1store_1finalize(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __expunge
 * Signature: (Lorg/apache/mesos/state/Variable;)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_mesos_state_AbstractState__1_1expunge
  (JNIEnv* env, jobject thiz, jobject jvariable)
{
  jclass clazz = env->GetObjectClass(jvariable);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable* variable = (Variable*) env->GetLongField(jvariable, __variable);

  clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State* state = (State*) env->GetLongField(thiz, __state);

  Future<bool>* future = new Future<bool>(state->expunge(*variable));

  return (jlong) future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __expunge_cancel
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1expunge_1cancel(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<bool>* future = (Future<bool>*) jfuture;

  // We'll initiate a discard but we won't consider it cancelled since
  // we don't know if/when the future will get discarded.
  future->discard();

  return (jboolean) false;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __expunge_is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1expunge_1is_1cancelled(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  // We always return false since while we might discard the future in
  // 'cancel' we don't know if it has really been discarded and we
  // don't want this function to block. We choose to be deterministic
  // here and always return false rather than sometimes returning true
  // if the future has completed (been discarded or otherwise).
  return (jboolean) false;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __expunge_is_done
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1expunge_1is_1done(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<bool>* future = (Future<bool>*) jfuture;

  return (jboolean) !future->isPending() || future->hasDiscard();
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __expunge_get
 * Signature: (J)Ljava/lang/Boolean;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1expunge_1get(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<bool>* future = (Future<bool>*) jfuture;

  future->await();

  if (future->isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future->failure().c_str());
    return nullptr;
  } else if (future->isDiscarded()) {
    // TODO(benh): Consider throwing an ExecutionException since we
    // never return true for 'isCancelled'.
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
    return nullptr;
  }

  CHECK_READY(*future);

  if (future->get()) {
    jclass clazz = env->FindClass("java/lang/Boolean");
    return env->GetStaticObjectField(
        clazz, env->GetStaticFieldID(clazz, "TRUE", "Ljava/lang/Boolean;"));
  }

  jclass clazz = env->FindClass("java/lang/Boolean");
  return env->GetStaticObjectField(
      clazz, env->GetStaticFieldID(clazz, "FALSE", "Ljava/lang/Boolean;"));
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __expunge_get_timeout
 * Signature: (JJLjava/util/concurrent/TimeUnit;)Ljava/lang/Boolean;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1expunge_1get_1timeout(
    JNIEnv* env, jobject thiz, jlong jfuture, jlong jtimeout, jobject junit)
{
  Future<bool>* future = (Future<bool>*) jfuture;

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds seconds(jseconds);

  if (future->await(seconds)) {
    if (future->isFailed()) {
      clazz = env->FindClass("java/util/concurrent/ExecutionException");
      env->ThrowNew(clazz, future->failure().c_str());
      return nullptr;
    } else if (future->isDiscarded()) {
      // TODO(benh): Consider throwing an ExecutionException since we
      // never return true for 'isCancelled'.
      clazz = env->FindClass("java/util/concurrent/CancellationException");
      env->ThrowNew(clazz, "Future was discarded");
      return nullptr;
    }

    CHECK_READY(*future);

    if (future->get()) {
      jclass clazz = env->FindClass("java/lang/Boolean");
      return env->GetStaticObjectField(
          clazz, env->GetStaticFieldID(clazz, "TRUE", "Ljava/lang/Boolean;"));
    }

    jclass clazz = env->FindClass("java/lang/Boolean");
    return env->GetStaticObjectField(
        clazz, env->GetStaticFieldID(clazz, "FALSE", "Ljava/lang/Boolean;"));
  }

  clazz = env->FindClass("java/util/concurrent/TimeoutException");
  env->ThrowNew(clazz, "Failed to wait for future within timeout");

  return nullptr;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __expunge_finalize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState__1_1expunge_1finalize(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<bool>* future = (Future<bool>*) jfuture;

  delete future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState$ExpungeFuture
 * Method:    cancel
 * Signature: (Z)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024ExpungeFuture_cancel(
    JNIEnv* env, jobject thiz, jboolean mayInterruptIfRunning)
{
  if (mayInterruptIfRunning) {
    static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
    static jfieldID field = env->GetFieldID(clazz, "future", "J");
    jlong jfuture = env->GetLongField(thiz, field);

    // See TODO at top of file for why we proxy this call.
    return Java_org_apache_mesos_state_AbstractState__1_1expunge_1cancel(
        env,
        thiz,
        jfuture);
  }

  return false; // Should not interrupt and already running (or finished).
}


/*
 * Class:     org_apache_mesos_state_AbstractState$ExpungeFuture
 * Method:    is_cancelled
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024ExpungeFuture_is_1cancelled(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1expunge_1is_1cancelled(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$ExpungeFuture
 * Method:    is_done
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024ExpungeFuture_is_1done(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1expunge_1is_1done(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$ExpungeFuture
 * Method:    __get
 * Signature: ()Ljava/lang/Boolean;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024ExpungeFuture_get(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1expunge_1get(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$ExpungeFuture
 * Method:    get_timeout
 * Signature: (JLjava/util/concurrent/TimeUnit;)Ljava/lang/Boolean;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024ExpungeFuture_get_1timeout(
    JNIEnv* env, jobject thiz, jlong jtimeout, jobject junit)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1expunge_1get_1timeout(
      env,
      thiz,
      jfuture,
      jtimeout,
      junit);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$ExpungeFuture
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState_00024ExpungeFuture_finalize(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  Java_org_apache_mesos_state_AbstractState__1_1expunge_1finalize(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __names
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL Java_org_apache_mesos_state_AbstractState__1_1names
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State* state = (State*) env->GetLongField(thiz, __state);

  Future<set<string>>* future =
    new Future<set<string>>(state->names());

  return (jlong) future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __names_cancel
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1names_1cancel(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<set<string>>* future = (Future<set<string>>*) jfuture;

  // We'll initiate a discard but we won't consider it cancelled since
  // we don't know if/when the future will get discarded.
  future->discard();

  return (jboolean) false;
}

/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __names_is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1names_1is_1cancelled(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  // We always return false since while we might discard the future in
  // 'cancel' we don't know if it has really been discarded and we
  // don't want this function to block. We choose to be deterministic
  // here and always return false rather than sometimes returning true
  // if the future has completed (been discarded or otherwise).
  return (jboolean) false;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __names_is_done
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState__1_1names_1is_1done(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<set<string>>* future = (Future<set<string>>*) jfuture;

  return (jboolean) !future->isPending() || future->hasDiscard();
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __names_get
 * Signature: (J)Ljava/util/Iterator;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1names_1get(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<set<string>>* future = (Future<set<string>>*) jfuture;

  future->await();

  if (future->isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future->failure().c_str());
    return nullptr;
  } else if (future->isDiscarded()) {
    // TODO(benh): Consider throwing an ExecutionException since we
    // never return true for 'isCancelled'.
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
    return nullptr;
  }

  CHECK_READY(*future);

  // List names = new ArrayList();
  jclass clazz = env->FindClass("java/util/ArrayList");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject jnames = env->NewObject(clazz, _init_);

  jmethodID add = env->GetMethodID(clazz, "add", "(Ljava/lang/Object;)Z");

  foreach (const string& name, future->get()) {
    jobject jname = convert<string>(env, name);
    env->CallBooleanMethod(jnames, add, jname);
  }

  // Iterator iterator = jnames.iterator();
  jmethodID iterator =
    env->GetMethodID(clazz, "iterator", "()Ljava/util/Iterator;");
  jobject jiterator = env->CallObjectMethod(jnames, iterator);

  return jiterator;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __names_get_timeout
 * Signature: (JJLjava/util/concurrent/TimeUnit;)Ljava/util/Iterator;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState__1_1names_1get_1timeout(
    JNIEnv* env, jobject thiz, jlong jfuture, jlong jtimeout, jobject junit)
{
  Future<set<string>>* future = (Future<set<string>>*) jfuture;

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds seconds(jseconds);

  if (future->await(seconds)) {
    if (future->isFailed()) {
      clazz = env->FindClass("java/util/concurrent/ExecutionException");
      env->ThrowNew(clazz, future->failure().c_str());
      return nullptr;
    } else if (future->isDiscarded()) {
      // TODO(benh): Consider throwing an ExecutionException since we
      // never return true for 'isCancelled'.
      clazz = env->FindClass("java/util/concurrent/CancellationException");
      env->ThrowNew(clazz, "Future was discarded");
      return nullptr;
    }

    CHECK_READY(*future);

    // List names = new ArrayList();
    clazz = env->FindClass("java/util/ArrayList");

    jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
    jobject jnames = env->NewObject(clazz, _init_);

    jmethodID add = env->GetMethodID(clazz, "add", "(Ljava/lang/Object;)Z");

    foreach (const string& name, future->get()) {
      jobject jname = convert<string>(env, name);
      env->CallBooleanMethod(jnames, add, jname);
    }

    // Iterator iterator = jnames.iterator();
    jmethodID iterator =
      env->GetMethodID(clazz, "iterator", "()Ljava/util/Iterator;");
    jobject jiterator = env->CallObjectMethod(jnames, iterator);

    return jiterator;
  }

  clazz = env->FindClass("java/util/concurrent/TimeoutException");
  env->ThrowNew(clazz, "Failed to wait for future within timeout");

  return nullptr;
}


/*
 * Class:     org_apache_mesos_state_AbstractState
 * Method:    __names_finalize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState__1_1names_1finalize(
    JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<set<string>>* future = (Future<set<string>>*) jfuture;

  delete future;
}


/*
 * Class:     org_apache_mesos_state_AbstractState$NamesFuture
 * Method:    cancel
 * Signature: (Z)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024NamesFuture_cancel(
    JNIEnv* env, jobject thiz, jboolean mayInterruptIfRunning)
{
  if (mayInterruptIfRunning) {
    static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
    static jfieldID field = env->GetFieldID(clazz, "future", "J");
    jlong jfuture = env->GetLongField(thiz, field);

    // See TODO at top of file for why we proxy this call.
    return Java_org_apache_mesos_state_AbstractState__1_1names_1cancel(
        env,
        thiz,
        jfuture);
  }

  return false; // Should not interrupt and already running (or finished).
}


/*
 * Class:     org_apache_mesos_state_AbstractState$NamesFuture
 * Method:    is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024NamesFuture_is_1cancelled(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1names_1is_1cancelled(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$NamesFuture
 * Method:    is_done
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_apache_mesos_state_AbstractState_00024NamesFuture_is_1done(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1names_1is_1done(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$NamesFuture
 * Method:    get
 * Signature: ()Ljava/util/Iterator;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024NamesFuture_get(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1names_1get(
      env,
      thiz,
      jfuture);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$NamesFuture
 * Method:    get_timeout
 * Signature: (JLjava/util/concurrent/TimeUnit;)Ljava/util/Iterator;
 */
JNIEXPORT jobject JNICALL
Java_org_apache_mesos_state_AbstractState_00024NamesFuture_get_1timeout(
    JNIEnv* env, jobject thiz, jlong jtimeout, jobject junit)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  return Java_org_apache_mesos_state_AbstractState__1_1names_1get_1timeout(
      env,
      thiz,
      jfuture,
      jtimeout,
      junit);
}


/*
 * Class:     org_apache_mesos_state_AbstractState$NamesFuture
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_apache_mesos_state_AbstractState_00024NamesFuture_finalize(
    JNIEnv* env, jobject thiz)
{
  static jclass clazz = (jclass)env->NewGlobalRef(env->GetObjectClass(thiz));
  static jfieldID field = env->GetFieldID(clazz, "future", "J");
  jlong jfuture = env->GetLongField(thiz, field);

  // See TODO at top of file for why we proxy this call.
  Java_org_apache_mesos_state_AbstractState__1_1names_1finalize(
      env,
      thiz,
      jfuture);
}

} // extern "C" {
