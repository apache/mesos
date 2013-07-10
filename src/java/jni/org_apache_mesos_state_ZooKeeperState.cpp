#include <jni.h>

#include <string>

#include <stout/duration.hpp>

#include "state/state.hpp"
#include "state/zookeeper.hpp"

#include "construct.hpp"
#include "convert.hpp"

using namespace mesos::internal::state;

using std::string;

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
  Storage* storage = NULL;
  if (jscheme != NULL && jcredentials != NULL) {
    string scheme = construct<string>(env, jscheme);

    jbyte* temp = env->GetByteArrayElements(jcredentials, NULL);
    jsize length = env->GetArrayLength(jcredentials);

    string credentials((char*) temp, (size_t) length);

    env->ReleaseByteArrayElements(jcredentials, temp, 0);

    zookeeper::Authentication authentication(scheme, credentials);

    storage = new ZooKeeperStorage(servers, timeout, znode, authentication);
  } else {
    storage = new ZooKeeperStorage(servers, timeout, znode);
  }

  CHECK(storage != NULL);

  State* state = new State(storage);

  // Initialize the __storage and __state variables.
  clazz = env->GetObjectClass(thiz);

  jfieldID __storage = env->GetFieldID(clazz, "__storage", "J");
  env->SetLongField(thiz, __storage, (jlong) storage);

  jfieldID __state = env->GetFieldID(clazz, "__state", "J");
  env->SetLongField(thiz, __state, (jlong) state);
}

} // extern "C" {
