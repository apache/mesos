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

#include <stout/lambda.hpp>

#include "common/thread.hpp"

namespace thread {

static void* __run(void* arg)
{
  lambda::function<void(void)>* function =
    reinterpret_cast<lambda::function<void(void)>*>(arg);
  (*function)();
  delete function;
  return 0;
}


bool start(const lambda::function<void(void)>& f, bool detach /*= false*/)
{
  lambda::function<void(void)>* __f = new lambda::function<void(void)>(f);

  pthread_t t;
  if (pthread_create(&t, NULL, __run, __f) != 0) {
    return false;
  }

  if (detach && pthread_detach(t) != 0) {
    return false;
  }

  return true;
}


} // namespace thread {
