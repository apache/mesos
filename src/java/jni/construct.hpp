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

#ifndef __CONSTRUCT_HPP__
#define __CONSTRUCT_HPP__

#include <vector>

#include <jni.h>

bool construct(JNIEnv* env, jboolean jbool);

template <typename T>
T construct(JNIEnv* env, jobject jobj);

template <typename T>
std::vector<T> constructFromIterable(JNIEnv* env, jobject jiterable)
{
  std::vector<T> result;

  jclass clazz = env->GetObjectClass(jiterable);

  // Iterator iterator = iterable.iterator();
  jmethodID iterator =
    env->GetMethodID(clazz, "iterator", "()Ljava/util/Iterator;");
  jobject jiterator = env->CallObjectMethod(jiterable, iterator);

  jclass iteratorClazz = env->GetObjectClass(jiterator);

  // while (iterator.hasNext()) {
  jmethodID hasNext = env->GetMethodID(iteratorClazz, "hasNext", "()Z");

  jmethodID next =
    env->GetMethodID(iteratorClazz, "next", "()Ljava/lang/Object;");

  while (env->CallBooleanMethod(jiterator, hasNext)) {
    // Object item = iterator.next();
    jobject jitem = env->CallObjectMethod(jiterator, next);
    result.emplace_back(construct<T>(env, jitem));
  }

  return result;
}

#endif // __CONSTRUCT_HPP__
