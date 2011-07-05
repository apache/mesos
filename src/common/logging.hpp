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

#ifndef __LOGGING_HPP__
#define __LOGGING_HPP__

#include <string>

#include "configurator/configurator.hpp"


namespace mesos { namespace internal {

/**
 * Utility functions for configuring and initializing Mesos logging.
 */
class Logging {
public:
  static void registerOptions(Configurator* conf);
  static void init(const char* programName, const Configuration& conf);
  static std::string getLogDir(const Configuration& conf);
  static bool isQuiet(const Configuration& conf);
};

}} /* namespace mesos::internal */

#endif
