#include <jni.h>

#include <string>

#include "state/state.hpp"

using namespace mesos::internal::state;

extern "C" {

/*
 * Class:     org_apache_mesos_state_Variable
 * Method:    value
 * Signature: ()[B
 */
JNIEXPORT jbyteArray JNICALL Java_org_apache_mesos_state_Variable_value
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable<std::string>* variable =
    (Variable<std::string>*) env->GetLongField(thiz, __variable);

  // byte[] value = ..;
  jbyteArray jvalue = env->NewByteArray((*variable)->size());
  env->SetByteArrayRegion(
      jvalue, 0, (*variable)->size(), (jbyte*) (*variable)->data());

  return jvalue;
}


/*
 * Class:     org_apache_mesos_state_Variable
 * Method:    mutate
 * Signature: ([B)V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_Variable_mutate
  (JNIEnv* env, jobject thiz, jbyteArray jvalue)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable<std::string>* variable =
    (Variable<std::string>*) env->GetLongField(thiz, __variable);

  jbyte* value = env->GetByteArrayElements(jvalue, NULL);
  jsize length = env->GetArrayLength(jvalue);

  (*variable)->assign((const char*) value, length);
}


/*
 * Class:     org_apache_mesos_state_Variable
 * Method:    finalize
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_apache_mesos_state_Variable_finalize
  (JNIEnv* env, jobject thiz)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable<std::string>* variable =
    (Variable<std::string>*) env->GetLongField(thiz, __variable);

  delete variable;
}

} // extern "C" {
