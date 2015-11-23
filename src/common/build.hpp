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

#ifndef __BUILD_HPP__
#define __BUILD_HPP__

#include <string>

namespace mesos {
namespace internal {
namespace build {

extern const std::string DATE;
extern const double TIME;
extern const std::string USER;
extern const std::string FLAGS;
extern const std::string JAVA_JVM_LIBRARY;
extern const Option<std::string> GIT_SHA;
extern const Option<std::string> GIT_BRANCH;
extern const Option<std::string> GIT_TAG;

} // namespace build {
} // namespace internal {
} // namespace mesos {

#endif // __BUILD_HPP__
