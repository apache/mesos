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

#include <mesos.h>

#include "configuration.hpp"


using namespace mesos;
using namespace mesos::internal;


int params_get_int(const char *params, const char *key, int defVal)
{
  return Params(params).getInt(key, defVal);
}


int32_t params_get_int32(const char *params, const char *key, int32_t defVal)
{
  return Params(params).getInt32(key, defVal);
}


int64_t params_get_int64(const char *params, const char *key, int64_t defVal)
{
  return Params(params).getInt64(key, defVal);
}
