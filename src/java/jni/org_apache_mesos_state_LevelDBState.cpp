#include <jni.h>

#include <string>

#include "state/leveldb.hpp"
#include "state/state.hpp"

#include "construct.hpp"
#include "convert.hpp"

using namespace mesos::internal::state;

using std::string;

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
