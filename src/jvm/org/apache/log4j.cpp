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

#include <jvm/org/apache/log4j.hpp>

namespace org {
namespace apache {
namespace log4j {

// Static storage and initialization.
const char LEVEL_OFF_SIGNATURE[] = "Lorg/apache/log4j/Level;";
const char LEVEL_OFF[] = "OFF";

Jvm::StaticVariable<Level, LEVEL_OFF, LEVEL_OFF_SIGNATURE> Level::OFF =
  Jvm::StaticVariable<Level, LEVEL_OFF, LEVEL_OFF_SIGNATURE>(
      Jvm::Class::named("org/apache/log4j/Level"));

} // namespace log4j {
} // namespace apache {
} // namespace org {
