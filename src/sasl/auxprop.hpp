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

#ifndef __SASL_AUXPROP_HPP__
#define __SASL_AUXPROP_HPP__

#include <sasl/sasl.h>
#include <sasl/saslplug.h>

#include <string>

#include <stout/foreach.hpp>
#include <stout/multimap.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {
namespace sasl {

struct Property
{
  std::string name;
  std::list<std::string> values;
};


class InMemoryAuxiliaryPropertyPlugin
{
public:
  static const char* name() { return "in-memory-auxprop"; }

  static void load(const Multimap<std::string, Property>& _properties)
  {
    properties = _properties;
  }

  static Option<std::list<std::string> > lookup(
      const std::string& user,
      const std::string& name)
  {
    if (properties.contains(user)) {
      foreach (const Property& property, properties.get(user)) {
        if (property.name == name) {
          return property.values;
        }
      }
    }
    return None();
  }

  // SASL plugin initialize entry.
  static int initialize(
      const sasl_utils_t* utils,
      int api,
      int* version,
      sasl_auxprop_plug_t** plug,
      const char* name);

private:
#if SASL_AUXPROP_PLUG_VERSION <= 4
  static void lookup(
#else
  static int lookup(
#endif
      void* context,
      sasl_server_params_t* sparams,
      unsigned flags,
      const char* user,
      unsigned length);

  static Multimap<std::string, Property> properties;

  static sasl_auxprop_plug_t plugin;
};

} // namespace sasl {
} // namespace internal {
} // namespace mesos {

#endif // __SASL_AUXPROP_HPP__
