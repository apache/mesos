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

#include <stdlib.h> // For atof.

#include <string>

#include <stout/none.hpp>
#include <stout/option.hpp>

#include "common/build.hpp"

// NOTE: On CMake, instead of defining `BUILD_DATE|TIME|USER` as
// compiler flags, we instead emit a header file with the definitions.
// This facilitates incremental builds as the compiler flags will
// no longer change with every invocation of the build.
// TODO(josephw): After deprecating autotools, remove this guard.
#ifdef USE_CMAKE_BUILD_CONFIG
#include "common/build_config.hpp"
#endif // USE_CMAKE_BUILD_CONFIG

#include "common/git_version.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace build {

const string DATE = BUILD_DATE;
const double TIME = atof(BUILD_TIME);

#ifdef BUILD_USER
const string USER = BUILD_USER;
#else
const string USER = "";
#endif

const string FLAGS = BUILD_FLAGS;

#ifdef BUILD_JAVA_JVM_LIBRARY
const string JAVA_JVM_LIBRARY = BUILD_JAVA_JVM_LIBRARY;
#else
const string JAVA_JVM_LIBRARY = "";
#endif

#ifdef BUILD_GIT_SHA
const Option<string> GIT_SHA = string(BUILD_GIT_SHA);
#else
const Option<string> GIT_SHA = None();
#endif

#ifdef BUILD_GIT_BRANCH
const Option<string> GIT_BRANCH = string(BUILD_GIT_BRANCH);
#else
const Option<string> GIT_BRANCH = None();
#endif

#ifdef BUILD_GIT_TAG
const Option<string> GIT_TAG = string(BUILD_GIT_TAG);
#else
const Option<string> GIT_TAG = None();
#endif
} // namespace build {
} // namespace internal {
} // namespace mesos {
