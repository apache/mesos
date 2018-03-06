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

#include "authentication/cram_md5/auxprop.hpp"

#include <mutex>

#include <stout/strings.hpp>

#include "logging/logging.hpp"

// We need to disable the deprecation warnings as Apple has decided
// to deprecate all of CyrusSASL's functions with OS 10.11
// (see MESOS-3030). We are using GCC pragmas also for covering clang.
#ifdef __APPLE__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace cram_md5 {

// Storage for the static members.
Multimap<string, Property> InMemoryAuxiliaryPropertyPlugin::properties;
sasl_auxprop_plug_t InMemoryAuxiliaryPropertyPlugin::plugin;
std::mutex InMemoryAuxiliaryPropertyPlugin::mutex;

int InMemoryAuxiliaryPropertyPlugin::initialize(
    const sasl_utils_t* utils,
    int api,
    int* version,
    sasl_auxprop_plug_t** plug,
    const char* name)
{
  if (version == nullptr || plug == nullptr) {
    return SASL_BADPARAM;
  }

  // Check if SASL API is older than the one we were compiled against.
  if (api < SASL_AUXPROP_PLUG_VERSION) {
    return SASL_BADVERS;
  }

  *version = SASL_AUXPROP_PLUG_VERSION;

  plugin.features = 0;
  plugin.spare_int1 = 0;
  plugin.glob_context = nullptr;
  plugin.auxprop_free = nullptr;
  plugin.auxprop_lookup = &InMemoryAuxiliaryPropertyPlugin::lookup;
  plugin.name = const_cast<char*>(InMemoryAuxiliaryPropertyPlugin::name());
  plugin.auxprop_store = nullptr;

  *plug = &plugin;

  VLOG(1) << "Initialized in-memory auxiliary property plugin";

  return SASL_OK;
}


#if SASL_AUXPROP_PLUG_VERSION <= 4
  void InMemoryAuxiliaryPropertyPlugin::lookup(
#else
  int InMemoryAuxiliaryPropertyPlugin::lookup(
#endif
    void* context,
    sasl_server_params_t* sparams,
    unsigned flags,
    const char* user,
    unsigned length)
{
  // Pull out the utils.
  const sasl_utils_t* utils = sparams->utils;

  // We determine the properties we should be looking up by doing a
  // 'prop_get' on the property context. Note that some of the
  // properties we get might need to be skipped depending on the
  // flags (see below).
  const propval* properties = utils->prop_get(sparams->propctx);

  CHECK(properties != nullptr)
    << "Invalid auxiliary properties requested for lookup";

  // TODO(benh): Consider "parsing" 'user' if it has an '@' separating
  // the actual user and a realm.

  string realm = sparams->user_realm != nullptr
    ? sparams->user_realm
    : sparams->serverFQDN;

  VLOG(1)
    << "Request to lookup properties for "
    << "user: '" << user << "' "
    << "realm: '" << realm << "' "
    << "server FQDN: '" << sparams->serverFQDN << "' "
#ifdef SASL_AUXPROP_VERIFY_AGAINST_HASH
    << "SASL_AUXPROP_VERIFY_AGAINST_HASH: "
    << (flags & SASL_AUXPROP_VERIFY_AGAINST_HASH ? "true ": "false ")
#endif
    << "SASL_AUXPROP_OVERRIDE: "
    << (flags & SASL_AUXPROP_OVERRIDE ? "true ": "false ")
    << "SASL_AUXPROP_AUTHZID: "
    << (flags & SASL_AUXPROP_AUTHZID ? "true ": "false ");

  // Now iterate through each property requested.
  const propval* property = properties;
  for (; property->name != nullptr; property++) {
    const char* name = property->name;

    // Skip properties that don't apply to this lookup given the flags.
    if (flags & SASL_AUXPROP_AUTHZID) {
      if (strings::startsWith(name, '*')) {
        VLOG(1) << "Skipping auxiliary property '" << name
                << "' since SASL_AUXPROP_AUTHZID == true";
        continue;
      }
    } else {
      // Only consider properties that start with '*' if
      // SASL_AUXPROP_AUTHZID is not set but don't include the '*'
      // when looking up the property name.
      if (!strings::startsWith(name, '*')) {
        VLOG(1) << "Skipping auxiliary property '" << name
                << "' since SASL_AUXPROP_AUTHZID == false "
                << "but property name starts with '*'";
        continue;
      } else {
        name = name + 1;
      }
    }

    // Don't override already set values unless instructed otherwise.
    if (property->values != nullptr && !(flags & SASL_AUXPROP_OVERRIDE)) {
#ifdef SASL_AUXPROP_VERIFY_AGAINST_HASH
      // Regardless of SASL_AUXPROP_OVERRIDE we're expected to
      // override property 'userPassword' when the
      // SASL_AUXPROP_VERIFY_AGAINST_HASH flag is set, so we erase it
      // here.
      if (flags & SASL_AUXPROP_VERIFY_AGAINST_HASH &&
          string(name) == string(SASL_AUX_PASSWORD_PROP)) {
        VLOG(1) << "Erasing auxiliary property '" << name
                << "' even though SASL_AUXPROP_OVERRIDE == true "
                << "since SASL_AUXPROP_VERIFY_AGAINST_HASH == true";
        utils->prop_erase(sparams->propctx, property->name);
      } else {
        VLOG(1) << "Skipping auxiliary property '" << name
                << "' since SASL_AUXPROP_OVERRIDE == false "
                << "and value(s) already set";
        continue;
      }
#else
      VLOG(1) << "Skipping auxiliary property '" << name
              << "' since SASL_AUXPROP_OVERRIDE == false "
              << "and value(s) already set";
      continue;
#endif
    } else if (property->values != nullptr) {
      CHECK(flags & SASL_AUXPROP_OVERRIDE);
      VLOG(1) << "Erasing auxiliary property '" << name
              << "' since SASL_AUXPROP_OVERRIDE == true";
      utils->prop_erase(sparams->propctx, property->name);
    }

    VLOG(1) << "Looking up auxiliary property '" << property->name << "'";

    Option<list<string>> values = lookup(user, name);

    if (values.isSome()) {
      if (values->empty()) {
        // Add the 'nullptr' value to indicate there were no values.
        utils->prop_set(sparams->propctx, property->name, nullptr, 0);
      } else {
        // Add all the values. Note that passing nullptr as the property
        // name for 'prop_set' will append values to the same name as
        // the previous 'prop_set' calls which is the behavior we want
        // after adding the first value.
        bool append = false;
        foreach (const string& value, values.get()) {
          sparams->utils->prop_set(
              sparams->propctx,
              append ? nullptr : property->name,
              value.c_str(),
              -1); // Let 'prop_set' use strlen.
          append = true;
        }
      }
    }
  }

#if SASL_AUXPROP_PLUG_VERSION > 4
  return SASL_OK;
#endif
}

} // namespace cram_md5 {
} // namespace internal {
} // namespace mesos {

#ifdef __APPLE__
#pragma GCC diagnostic pop
#endif
