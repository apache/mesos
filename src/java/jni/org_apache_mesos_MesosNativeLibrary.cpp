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

#include <mesos/version.hpp>

extern "C" {

/*
 * Class:     org_apache_mesos_MesosNativeLibrary
 * Method:    _version
 * Signature: ()Lorg/apache/mesos/MesosNativeLibrary$Version;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosNativeLibrary__1version
  (JNIEnv* env)
{
  jclass clazz = env->FindClass("org/apache/mesos/MesosNativeLibrary$Version");
  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "(JJJ)V");
  jobject jversion = env->NewObject(
      clazz,
      _init_,
      (jlong) MESOS_MAJOR_VERSION_NUM,
      (jlong) MESOS_MINOR_VERSION_NUM,
      (jlong) MESOS_PATCH_VERSION_NUM);
  return jversion;
}

} // extern "C" {
