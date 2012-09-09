#include <jni.h>

#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/duration.hpp>

#include "state/state.hpp"
#include "state/zookeeper.hpp"

#include "construct.hpp"
#include "convert.hpp"

using namespace mesos::internal::state;

using process::Future;

using std::string;
using std::vector;

extern "C" {

/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    initialize
 * Signature: (Ljava/lang/String;JLjava/util/concurrent/TimeUnit;Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_ZooKeeperState_initialize__Ljava_lang_String_2JLjava_util_concurrent_TimeUnit_2Ljava_lang_String_2
  (JNIEnv* env,
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

   // Create the C++ State and initialize the __state variable.
  State<>* state = new ZooKeeperState<>(servers, timeout, znode);

  clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");
  env->SetLongField(thiz, __state, (jlong) state);
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    initialize
 * Signature: (Ljava/lang/String;JLjava/util/concurrent/TimeUnit;Ljava/lang/String;Ljava/lang/String;[B)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_ZooKeeperState_initialize__Ljava_lang_String_2JLjava_util_concurrent_TimeUnit_2Ljava_lang_String_2Ljava_lang_String_2_3B
  (JNIEnv* env,
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
  State<>* state = NULL;
  if (jscheme != NULL && jcredentials != NULL) {
    string scheme = construct<string>(env, jscheme);

    jbyte* temp = env->GetByteArrayElements(jcredentials, NULL);
    jsize length = env->GetArrayLength(jcredentials);

    string credentials((char*) temp, (size_t) length);

    env->ReleaseByteArrayElements(jcredentials, temp, 0);

    zookeeper::Authentication authentication(scheme, credentials);

    state = new ZooKeeperState<>(servers, timeout, znode, authentication);
  } else {
    state = new ZooKeeperState<>(servers, timeout, znode);
  }

  CHECK(state != NULL);

  // Initialize the __state variable.
  clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");
  env->SetLongField(thiz, __state, (jlong) state);
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_ZooKeeperState_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State<>* state = (State<>*) env->GetLongField(thiz, __state);

  delete state;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __get
 * Signature: (Ljava/lang/String;)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get
  (JNIEnv* env, jobject thiz, jstring jname)
{
  string name = construct<string>(env, jname);

  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State<>* state = (State<>*) env->GetLongField(thiz, __state);

  Future<Variable<string> >* future =
    new Future<Variable<string> >(state->get<string>(name));

  return (jlong) future;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __get_cancel
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get_1cancel
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable<string> >* future = (Future<Variable<string> >*) jfuture;

  if (!future->isDiscarded()) {
    future->discard();
    return (jboolean) future->isDiscarded();
  }

  return (jboolean) true;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __get_is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get_1is_1cancelled
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable<string> >* future = (Future<Variable<string> >*) jfuture;

  return (jboolean) future->isDiscarded();
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __get_is_done
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get_1is_1done
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable<string> >* future = (Future<Variable<string> >*) jfuture;

  return (jboolean) !future->isPending();
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __get_get
 * Signature: (J)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get_1get
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable<string> >* future = (Future<Variable<string> >*) jfuture;

  future->await();

  if (future->isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future->failure().c_str());
    return NULL;
  } else if (future->isDiscarded()) {
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
    return NULL;
  }

  CHECK(future->isReady());

  Variable<string>* variable = new Variable<string>(future->get());

  // Variable variable = new Variable();
  jclass clazz = env->FindClass("org/apache/mesos/state/Variable");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject jvariable = env->NewObject(clazz, _init_);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
  env->SetLongField(jvariable, __variable, (jlong) variable);

  return jvariable;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __get_get_timeout
 * Signature: (JJLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get_1get_1timeout
  (JNIEnv* env, jobject thiz, jlong jfuture, jlong jtimeout, jobject junit)
{
  Future<Variable<string> >* future = (Future<Variable<string> >*) jfuture;

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds timeout(jseconds);

  if (future->await(timeout.secs())) {
    if (future->isFailed()) {
      clazz = env->FindClass("java/util/concurrent/ExecutionException");
      env->ThrowNew(clazz, future->failure().c_str());
      return NULL;
    } else if (future->isDiscarded()) {
      clazz = env->FindClass("java/util/concurrent/CancellationException");
      env->ThrowNew(clazz, "Future was discarded");
      return NULL;
    }

    CHECK(future->isReady());
    Variable<string>* variable = new Variable<string>(future->get());

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

  return NULL;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __get_finalize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get_1finalize
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Variable<string> >* future = (Future<Variable<string> >*) jfuture;

  delete future;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set
 * Signature: (Lorg/apache/mesos/state/Variable;)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set
  (JNIEnv* env, jobject thiz, jobject jvariable)
{
  jclass clazz = env->GetObjectClass(jvariable);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable<string>* variable = (Variable<string>*)
    env->GetLongField(jvariable, __variable);

  clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State<>* state = (State<>*) env->GetLongField(thiz, __state);

  Future<Option<Variable<string> > >* future =
    new Future<Option<Variable<string> > >(state->set(*variable));

  return (jlong) future;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set_cancel
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set_1cancel
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable<string> > >* future =
    (Future<Option<Variable<string> > >*) jfuture;

  if (!future->isDiscarded()) {
    future->discard();
    return (jboolean) future->isDiscarded();
  }

  return (jboolean) true;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set_is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set_1is_1cancelled
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable<string> > >* future =
    (Future<Option<Variable<string> > >*) jfuture;

  return (jboolean) future->isDiscarded();
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set_is_done
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set_1is_1done
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable<string> > >* future =
    (Future<Option<Variable<string> > >*) jfuture;

  return (jboolean) !future->isPending();
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set_get
 * Signature: (J)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set_1get
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable<string> > >* future =
    (Future<Option<Variable<string> > >*) jfuture;

  future->await();

  if (future->isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future->failure().c_str());
    return NULL;
  } else if (future->isDiscarded()) {
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
    return NULL;
  }

  CHECK(future->isReady());

  if (future->get().isSome()) {
    Variable<string>* variable = new Variable<string>(future->get().get());

    // Variable variable = new Variable();
    jclass clazz = env->FindClass("org/apache/mesos/state/Variable");

    jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
    jobject jvariable = env->NewObject(clazz, _init_);

    jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
    env->SetLongField(jvariable, __variable, (jlong) variable);

    return jvariable;
  }

  return NULL;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set_get_timeout
 * Signature: (JJLjava/util/concurrent/TimeUnit;)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set_1get_1timeout
  (JNIEnv* env, jobject thiz, jlong jfuture, jlong jtimeout, jobject junit)
{
  Future<Option<Variable<string> > >* future =
    (Future<Option<Variable<string> > >*) jfuture;

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds timeout(jseconds);

  if (future->await(timeout.secs())) {
    if (future->isFailed()) {
      clazz = env->FindClass("java/util/concurrent/ExecutionException");
      env->ThrowNew(clazz, future->failure().c_str());
      return NULL;
    } else if (future->isDiscarded()) {
      clazz = env->FindClass("java/util/concurrent/CancellationException");
      env->ThrowNew(clazz, "Future was discarded");
      return NULL;
    }

    CHECK(future->isReady());

    if (future->get().isSome()) {
      Variable<string>* variable = new Variable<string>(future->get().get());

      // Variable variable = new Variable();
      clazz = env->FindClass("org/apache/mesos/state/Variable");

      jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
      jobject jvariable = env->NewObject(clazz, _init_);

      jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
      env->SetLongField(jvariable, __variable, (jlong) variable);

      return jvariable;
    }

    return NULL;
  }

  clazz = env->FindClass("java/util/concurrent/TimeoutException");
  env->ThrowNew(clazz, "Failed to wait for future within timeout");

  return NULL;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set_finalize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set_1finalize
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<Option<Variable<string> > >* future =
    (Future<Option<Variable<string> > >*) jfuture;

  delete future;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __names
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1names
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State<>* state = (State<>*) env->GetLongField(thiz, __state);

  Future<vector<string> >* future = new Future<vector<string> >(state->names());

  return (jlong) future;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __names_cancel
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1names_1cancel
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<vector<string> >* future = (Future<vector<string> >*) jfuture;

  if (!future->isDiscarded()) {
    future->discard();
    return (jboolean) future->isDiscarded();
  }

  return (jboolean) true;
}

/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __names_is_cancelled
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1names_1is_1cancelled
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<vector<string> >* future = (Future<vector<string> >*) jfuture;

  return (jboolean) future->isDiscarded();
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __names_is_done
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1names_1is_1done
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<vector<string> >* future = (Future<vector<string> >*) jfuture;

  return (jboolean) !future->isPending();
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __names_get
 * Signature: (J)Ljava/util/Iterator;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1names_1get
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<vector<string> >* future = (Future<vector<string> >*) jfuture;

  future->await();

  if (future->isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future->failure().c_str());
    return NULL;
  } else if (future->isDiscarded()) {
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
    return NULL;
  }

  CHECK(future->isReady());

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
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __names_get_timeout
 * Signature: (JJLjava/util/concurrent/TimeUnit;)Ljava/util/Iterator;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1names_1get_1timeout
  (JNIEnv* env, jobject thiz, jlong jfuture, jlong jtimeout, jobject junit)
{
  Future<vector<string> >* future = (Future<vector<string> >*) jfuture;

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  Seconds timeout(jseconds);

  if (future->await(timeout.secs())) {
    if (future->isFailed()) {
      clazz = env->FindClass("java/util/concurrent/ExecutionException");
      env->ThrowNew(clazz, future->failure().c_str());
      return NULL;
    } else if (future->isDiscarded()) {
      clazz = env->FindClass("java/util/concurrent/CancellationException");
      env->ThrowNew(clazz, "Future was discarded");
      return NULL;
    }

    CHECK(future->isReady());

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

  return NULL;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __names_finalize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1names_1finalize
  (JNIEnv* env, jobject thiz, jlong jfuture)
{
  Future<vector<string> >* future = (Future<vector<string> >*) jfuture;

  delete future;
}

} // extern "C" {
