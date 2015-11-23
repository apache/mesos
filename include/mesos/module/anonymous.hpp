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

#ifndef __MESOS_MODULE_ANONYMOUS_HPP__
#define __MESOS_MODULE_ANONYMOUS_HPP__

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

namespace mesos {
namespace modules {

// The 'Anonymous' kind of module does __not__ receive any callbacks
// but simply coexists with its parent (OS-)process.
//
// Unlike other named modules, an anonymous module does not directly
// replace or provide essential Mesos functionality (such as an
// Isolator module does). Unlike a decorator module it does not
// directly add or inject data into Mesos core either.
//
// Anonymous modules do not require any specific selectors (flags) as
// they are immediately instantiated when getting loaded via the
// '--modules' flag by the Mesos master or slave.
//
class Anonymous
{
public:
  Anonymous() {}

  virtual ~Anonymous() {}
};


template <>
inline const char* kind<Anonymous>()
{
  return "Anonymous";
}


template <>
struct Module<Anonymous> : ModuleBase
{
  Module(const char* _moduleApiVersion,
         const char* _mesosVersion,
         const char* _authorName,
         const char* _authorEmail,
         const char* _description,
         bool (*_compatible)(),
         Anonymous* (*_create)(const Parameters& parameters))
    : ModuleBase(_moduleApiVersion,
                 _mesosVersion,
                 mesos::modules::kind<Anonymous>(),
                 _authorName,
                 _authorEmail,
                 _description,
                 _compatible),
      create(_create) {}

  Anonymous* (*create)(const Parameters& parameters);
};

} // namespace modules {
} // namespace mesos {

#endif // __MESOS_MODULE_ANONYMOUS_HPP__
