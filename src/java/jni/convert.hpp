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

#ifndef __CONVERT_HPP__
#define __CONVERT_HPP__

#include <jni.h>

#include <stout/result.hpp>

template <typename T>
jobject convert(JNIEnv* env, const T& t);

Result<jfieldID> getFieldID(
    JNIEnv* env,
    jclass clazz,
    const char* name,
    const char* signature);

#endif // __CONVERT_HPP__
