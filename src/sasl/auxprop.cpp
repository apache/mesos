#include "logging/logging.hpp"

#include "sasl/auxprop.hpp"

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace sasl {

// Storage for the static members.
Multimap<string, Property> InMemoryAuxiliaryPropertyPlugin::properties;
sasl_auxprop_plug_t InMemoryAuxiliaryPropertyPlugin::plugin;


int InMemoryAuxiliaryPropertyPlugin::initialize(
    const sasl_utils_t* utils,
    int api,
    int* version,
    sasl_auxprop_plug_t** plug,
    const char* name)
{
  if (version == NULL || plug == NULL) {
    return SASL_BADPARAM;
  }

  // Check if SASL API is older than the one we were compiled against.
  if (api < SASL_AUXPROP_PLUG_VERSION) {
    return SASL_BADVERS;
  }

  *version = SASL_AUXPROP_PLUG_VERSION;

  plugin.features = 0;
  plugin.spare_int1 = 0;
  plugin.glob_context = NULL;
  plugin.auxprop_free = NULL;
  plugin.auxprop_lookup = &InMemoryAuxiliaryPropertyPlugin::lookup;
  plugin.name = const_cast<char*>(InMemoryAuxiliaryPropertyPlugin::name());
  plugin.auxprop_store = NULL;

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
  // properties we get might might need to be skipped depending on the
  // flags (see below).
  const propval* properties = utils->prop_get(sparams->propctx);

  CHECK(properties != NULL)
    << "Invalid auxiliary properties requested for lookup";

  // TODO(benh): Consider "parsing" 'user' if it has an '@' separating
  // the actual user and a realm.

  string realm = sparams->user_realm != NULL
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
  for (; property->name != NULL; property++) {
    const char* name = property->name;

    // Skip properties that don't apply to this lookup given the flags.
    if (flags & SASL_AUXPROP_AUTHZID) {
      if (name[0] == '*') {
        VLOG(1) << "Skipping auxiliary property '" << name
                << "' since SASL_AUXPROP_AUTHZID == true";
        continue;
      }
    } else {
      // Only consider properties that start with '*' if
      // SASL_AUXPROP_AUTHZID is not set but don't include the '*'
      // when looking up the property name.
      if (name[0] != '*') {
        VLOG(1) << "Skipping auxiliary property '" << name
                << "' since SASL_AUXPROP_AUTHZID == false "
                << "but property name starts with '*'";
        continue;
      } else {
        name = name + 1;
      }
    }

    // Don't override already set values unless instructed otherwise.
    if (property->values != NULL && !(flags & SASL_AUXPROP_OVERRIDE)) {
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
    } else if (property->values != NULL) {
      CHECK(flags & SASL_AUXPROP_OVERRIDE);
      VLOG(1) << "Erasing auxiliary property '" << name
              << "' since SASL_AUXPROP_OVERRIDE == true";
      utils->prop_erase(sparams->propctx, property->name);
    }

    VLOG(1) << "Looking up auxiliary property '" << property->name << "'";

    Option<list<string> > values = lookup(user, name);

    if (values.isSome()) {
      if (values.get().empty()) {
        // Add the 'NULL' value to indicate there were no values.
        utils->prop_set(sparams->propctx, property->name, NULL, 0);
      } else {
        // Add all the values. Note that passing NULL as the property
        // name for 'prop_set' will append values to the same name as
        // the previous 'prop_set' calls which is the behavior we want
        // after adding the first value.
        bool append = false;
        foreach (const string& value, values.get()) {
          sparams->utils->prop_set(
              sparams->propctx,
              append ? NULL : property->name,
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

} // namespace sasl {
} // namespace internal {
} // namespace mesos {
