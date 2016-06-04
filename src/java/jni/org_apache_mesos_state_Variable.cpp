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

using mesos::state::Variable;

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

  Variable* variable = (Variable*) env->GetLongField(thiz, __variable);

  const std::string& value = variable->value();

  // byte[] value = ..;
  jbyteArray jvalue = env->NewByteArray(value.size());
  env->SetByteArrayRegion(jvalue, 0, value.size(), (jbyte*) value.data());

  return jvalue;
}


/*
 * Class:     org_apache_mesos_state_Variable
 * Method:    mutate
 * Signature: ([B)Lorg/apache/mesos/state/Variable;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_state_Variable_mutate
  (JNIEnv* env, jobject thiz, jbyteArray jvalue)
{
  jclass clazz = env->GetObjectClass(thiz);

  jfieldID __variable = env->GetFieldID(clazz, "__variable", "J");

  Variable* variable = (Variable*) env->GetLongField(thiz, __variable);

  jbyte* value = env->GetByteArrayElements(jvalue, nullptr);
  jsize length = env->GetArrayLength(jvalue);

  // Mutate the variable and save a copy of the result.
  variable =
    new Variable(variable->mutate(std::string((const char*) value, length)));

  env->ReleaseByteArrayElements(jvalue, value, 0);

  // Variable variable = new Variable();
  clazz = env->FindClass("org/apache/mesos/state/Variable");

  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "()V");
  jobject jvariable = env->NewObject(clazz, _init_);

  env->SetLongField(jvariable, __variable, (jlong) variable);

  return jvariable;
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

  Variable* variable = (Variable*) env->GetLongField(thiz, __variable);

  delete variable;
}

} // extern "C" {
