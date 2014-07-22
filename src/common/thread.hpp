/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __THREAD_HPP__
#define __THREAD_HPP__

#include <pthread.h>

#include <stout/lambda.hpp>

// Provides a simple threading facility for starting a thread to run
// an arbitrary function. No mechanism for returning a value from the
// function is currently provided (and in the future would probably be
// provided by libprocess anyway).

namespace thread {

// TODO(benh): Provide a version of 'start' that returns a type T (the
// value being a copy or preferablly via move semantics).
bool start(const lambda::function<void(void)>& f, bool detach = false);

} // namespace thread {

#endif // __THREAD_HPP__
