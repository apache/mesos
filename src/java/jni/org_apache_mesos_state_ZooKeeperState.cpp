#include <jni.h>

#include <string>

#include <process/future.hpp>

#include "state/state.hpp"
#include "state/zookeeper.hpp"

#include "construct.hpp"

using namespace mesos::internal::state;

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
  std::string servers = construct<std::string>(env, jservers);

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  seconds timeout(jseconds);

  std::string znode = construct<std::string>(env, jznode);

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
  std::string servers = construct<std::string>(env, jservers);

  jclass clazz = env->GetObjectClass(junit);

  // long seconds = unit.toSeconds(time);
  jmethodID toSeconds = env->GetMethodID(clazz, "toSeconds", "(J)J");

  jlong jseconds = env->CallLongMethod(junit, toSeconds, jtimeout);

  seconds timeout(jseconds);

  std::string znode = construct<std::string>(env, jznode);

  // Create the C++ State.
  State<>* state = NULL;
  if (jscheme != NULL && jcredentials != NULL) {
    std::string scheme = construct<std::string>(env, jscheme);

    jbyte* temp = env->GetByteArrayElements(jcredentials, NULL);
    jsize length = env->GetArrayLength(jcredentials);

    std::string credentials((char*) temp, (size_t) length);

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
 * Signature: (Ljava/lang/String;)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1get
  (JNIEnv* env, jobject thiz, jstring jname)
{
  std::string name = construct<std::string>(env, jname);

  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State<>* state = (State<>*) env->GetLongField(thiz, __state);

  process::Future<Variable<std::string> > future =
    state->get<std::string>(name);

  future.await();

  if (future.isFailed()) {
    clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future.failure().c_str());
  } else if (future.isDiscarded()) {
    clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
  }

  CHECK(future.isReady());

  Variable<std::string>* variable = new Variable<std::string>(future.get());

  // Variable variable = new Variable();
   clazz = env->FindClass("org/apache/mesos/state/Variable");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject jvariable = env->NewObject(clazz, _init_);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");
  env->SetLongField(jvariable, __variable, (jlong) variable);

  return jvariable;
}


/*
 * Class:     org_apache_mesos_state_ZooKeeperState
 * Method:    __set
 * Signature: (Lorg/apache/mesos/state/Variable;)B
 */
JNIEXPORT jboolean JNICALL Java_org_apache_mesos_state_ZooKeeperState__1_1set
  (JNIEnv* env, jobject thiz, jobject jvariable)
{
  jclass clazz = env->GetObjectClass(jvariable);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable<std::string>* variable =
    (Variable<std::string>*) env->GetLongField(jvariable, __variable);

  clazz = env->GetObjectClass(thiz);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");

  State<>* state = (State<>*) env->GetLongField(thiz, __state);

  process::Future<bool> future = state->set(variable);

  future.await();

  if (future.isFailed()) {
    jclass clazz = env->FindClass("java/util/concurrent/ExecutionException");
    env->ThrowNew(clazz, future.failure().c_str());
  } else if (future.isDiscarded()) {
    jclass clazz = env->FindClass("java/util/concurrent/CancellationException");
    env->ThrowNew(clazz, "Future was discarded");
  }

  CHECK(future.isReady());

  return (jboolean) future.get();
}

} // extern "C" {
