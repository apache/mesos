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

#include <stddef.h>   // For size_t needed by sasl.h.

#include <sasl/sasl.h>
#include <sasl/saslplug.h>

#include <map>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/defer.hpp>
#include <process/once.hpp>
#include <process/owned.hpp>
#include <process/protobuf.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>

#include "authenticator.hpp"

#include "authentication/cram_md5/auxprop.hpp"

#include "messages/messages.hpp"

// We need to disable the deprecation warnings as Apple has decided
// to deprecate all of CyrusSASL's functions with OS 10.11
// (see MESOS-3030). We are using GCC pragmas also for covering clang.
#ifdef __APPLE__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace mesos {
namespace internal {
namespace cram_md5 {

using namespace process;
using std::string;

class CRAMMD5AuthenticatorSessionProcess :
  public ProtobufProcess<CRAMMD5AuthenticatorSessionProcess>
{
public:
  explicit CRAMMD5AuthenticatorSessionProcess(const UPID& _pid)
    : ProcessBase(ID::generate("crammd5-authenticator-session")),
      status(READY),
      pid(_pid),
      connection(nullptr) {}

  ~CRAMMD5AuthenticatorSessionProcess() override
  {
    if (connection != nullptr) {
      sasl_dispose(&connection);
    }
  }

  void finalize() override
  {
    discarded(); // Fail the promise.
  }

  Future<Option<string>> authenticate()
  {
    if (status != READY) {
      return promise.future();
    }

    callbacks[0].id = SASL_CB_GETOPT;
    callbacks[0].proc = (int(*)()) &getopt;
    callbacks[0].context = nullptr;

    callbacks[1].id = SASL_CB_CANON_USER;
    callbacks[1].proc = (int(*)()) &canonicalize;
    // Pass in the principal so we can set it in canon_user().
    callbacks[1].context = &principal;

    callbacks[2].id = SASL_CB_LIST_END;
    callbacks[2].proc = nullptr;
    callbacks[2].context = nullptr;

    LOG(INFO) << "Creating new server SASL connection";

    int result = sasl_server_new(
        "mesos",    // Registered name of service.
        nullptr,    // Server's FQDN; nullptr uses gethostname().
        nullptr,    // The user realm used for password lookups;
                    // nullptr means default to FQDN.
                    // NOTE: This does not affect Kerberos.
        nullptr,    // IP address information string.
        nullptr,    // IP address information string.
        callbacks,  // Callbacks supported only for this connection.
        0,          // Security flags (security layers are enabled
                    // using security properties, separately).
        &connection);

    if (result != SASL_OK) {
      string error = "Failed to create server SASL connection: ";
      error += sasl_errstring(result, nullptr, nullptr);
      LOG(ERROR) << error;
      AuthenticationErrorMessage message;
      message.set_error(error);
      send(pid, message);
      status = ERROR;
      promise.fail(error);
      return promise.future();
    }

    // Get the list of mechanisms.
    const char* output = nullptr;
    unsigned length = 0;
    int count = 0;

    result = sasl_listmech(
        connection,  // The context for this connection.
        nullptr,     // Not supported.
        "",          // What to prepend to the output string.
        ",",         // What to separate mechanisms with.
        "",          // What to append to the output string.
        &output,     // The output string.
        &length,     // The length of the output string.
        &count);     // The count of the mechanisms in output.

    if (result != SASL_OK) {
      string error = "Failed to get list of mechanisms: ";
      LOG(WARNING) << error << sasl_errstring(result, nullptr, nullptr);
      AuthenticationErrorMessage message;
      error += sasl_errdetail(connection);
      message.set_error(error);
      send(pid, message);
      status = ERROR;
      promise.fail(error);
      return promise.future();
    }

    std::vector<string> mechanisms = strings::tokenize(output, ",");

    // Send authentication mechanisms.
    AuthenticationMechanismsMessage message;
    foreach (const string& mechanism, mechanisms) {
      message.add_mechanisms(mechanism);
    }

    send(pid, message);

    status = STARTING;

    // Stop authenticating if nobody cares.
    promise.future().onDiscard(defer(self(), &Self::discarded));

    return promise.future();
  }

  void initialize() override
  {
    link(pid); // Don't bother waiting for a lost authenticatee.

    // Anticipate start and steps messages from the client.
    install<AuthenticationStartMessage>(
        &CRAMMD5AuthenticatorSessionProcess::start,
        &AuthenticationStartMessage::mechanism,
        &AuthenticationStartMessage::data);

    install<AuthenticationStepMessage>(
        &CRAMMD5AuthenticatorSessionProcess::step,
        &AuthenticationStepMessage::data);
  }

  void exited(const UPID& _pid) override
  {
    if (pid == _pid) {
      status = ERROR;
      promise.fail("Failed to communicate with authenticatee");
    }
  }

  void start(const string& mechanism, const string& data)
  {
    if (status != STARTING) {
      AuthenticationErrorMessage message;
      message.set_error("Unexpected authentication 'start' received");
      send(pid, message);
      status = ERROR;
      promise.fail(message.error());
      return;
    }

    LOG(INFO) << "Received SASL authentication start";

    // Start the server.
    const char* output = nullptr;
    unsigned length = 0;

    int result = sasl_server_start(
        connection,
        mechanism.c_str(),
        data.length() == 0 ? nullptr : data.data(),
        static_cast<unsigned>(data.length()),
        &output,
        &length);

    handle(result, output, length);
  }

  void step(const string& data)
  {
    if (status != STEPPING) {
      AuthenticationErrorMessage message;
      message.set_error("Unexpected authentication 'step' received");
      send(pid, message);
      status = ERROR;
      promise.fail(message.error());
      return;
    }

    LOG(INFO) << "Received SASL authentication step";

    const char* output = nullptr;
    unsigned length = 0;

    int result = sasl_server_step(
        connection,
        data.length() == 0 ? nullptr : data.data(),
        static_cast<unsigned>(data.length()),
        &output,
        &length);

    handle(result, output, length);
  }

  void discarded()
  {
    status = DISCARDED;
    promise.fail("Authentication discarded");
  }

private:
  static int getopt(
      void* context,
      const char* plugin,
      const char* option,
      const char** result,
      unsigned* length)
  {
    bool found = false;
    if (string(option) == "auxprop_plugin") {
      *result = "in-memory-auxprop";
      found = true;
    } else if (string(option) == "mech_list") {
      *result = "CRAM-MD5";
      found = true;
    } else if (string(option) == "pwcheck_method") {
      *result = "auxprop";
      found = true;
    }

    if (found && length != nullptr) {
      *length = static_cast<unsigned>(strlen(*result));
    }

    return SASL_OK;
  }

  // Callback for canonicalizing the username (principal). We use it
  // to record the principal in CRAMMD5Authenticator.
  static int canonicalize(
      sasl_conn_t* connection,
      void* context,
      const char* input,
      unsigned inputLength,
      unsigned flags,
      const char* userRealm,
      char* output,
      unsigned outputMaxLength,
      unsigned* outputLength)
  {
    CHECK_NOTNULL(input);
    CHECK_NOTNULL(context);
    CHECK_NOTNULL(output);

    // Save the input.
    Option<string>* principal =
      static_cast<Option<string>*>(context);
    CHECK(principal->isNone());
    *principal = string(input, inputLength);

    // Tell SASL that the canonical username is the same as the
    // client-supplied username.
    memcpy(output, input, inputLength);
    *outputLength = inputLength;

    return SASL_OK;
  }

  // Helper for handling result of server start and step.
  void handle(int result, const char* output, unsigned length)
  {
    if (result == SASL_OK) {
      // Principal must have been set if authentication succeeded.
      CHECK_SOME(principal);

      LOG(INFO) << "Authentication success";
      // Note that we're not using SASL_SUCCESS_DATA which means that
      // we should not have any data to send when we get a SASL_OK.
      CHECK(output == nullptr);
      send(pid, AuthenticationCompletedMessage());
      status = COMPLETED;
      promise.set(principal);
    } else if (result == SASL_CONTINUE) {
      LOG(INFO) << "Authentication requires more steps";
      AuthenticationStepMessage message;
      message.set_data(CHECK_NOTNULL(output), length);
      send(pid, message);
      status = STEPPING;
    } else if (result == SASL_NOUSER || result == SASL_BADAUTH) {
      LOG(WARNING) << "Authentication failure: "
                   << sasl_errstring(result, nullptr, nullptr);
      send(pid, AuthenticationFailedMessage());
      status = FAILED;
      promise.set(Option<string>::none());
    } else {
      LOG(ERROR) << "Authentication error: "
                 << sasl_errstring(result, nullptr, nullptr);
      AuthenticationErrorMessage message;
      string error(sasl_errdetail(connection));
      message.set_error(error);
      send(pid, message);
      status = ERROR;
      promise.fail(message.error());
    }
  }

  enum
  {
    READY,
    STARTING,
    STEPPING,
    COMPLETED,
    FAILED,
    ERROR,
    DISCARDED
  } status;

  sasl_callback_t callbacks[3];

  const UPID pid;

  sasl_conn_t* connection;

  Promise<Option<string>> promise;

  Option<string> principal;
};


class CRAMMD5AuthenticatorSession
{
public:
  explicit CRAMMD5AuthenticatorSession(const UPID& pid)
  {
    process = new CRAMMD5AuthenticatorSessionProcess(pid);
    spawn(process);
  }

  virtual ~CRAMMD5AuthenticatorSession()
  {
    // TODO(vinod): As a short term fix for the race condition #1 in
    // MESOS-1866, we inject the 'terminate' event at the end of the
    // CRAMMD5AuthenticatorSessionProcess queue instead of at the front.
    // The long term fix for this is https://reviews.apache.org/r/25945/.
    terminate(process, false);
    wait(process);
    delete process;
  }

  virtual Future<Option<string>> authenticate()
  {
    return dispatch(
        process, &CRAMMD5AuthenticatorSessionProcess::authenticate);
  }

private:
  CRAMMD5AuthenticatorSessionProcess* process;
};


class CRAMMD5AuthenticatorProcess :
  public Process<CRAMMD5AuthenticatorProcess>
{
public:
  CRAMMD5AuthenticatorProcess() :
    ProcessBase(ID::generate("crammd5-authenticator")) {}

  ~CRAMMD5AuthenticatorProcess() override {}

  Future<Option<string>> authenticate(const UPID& pid)
  {
    VLOG(1) << "Starting authentication session for " << pid;

    if (sessions.contains(pid)) {
      return Failure("Authentication session already active");
    }

    Owned<CRAMMD5AuthenticatorSession> session(
        new CRAMMD5AuthenticatorSession(pid));

    sessions.put(pid, session);

    return session->authenticate()
      .onAny(defer(self(), &Self::_authenticate, pid));
  }

  virtual void _authenticate(const UPID& pid)
  {
    if (sessions.contains(pid)){
      VLOG(1) << "Authentication session cleanup for " << pid;
      sessions.erase(pid);
  }
}

private:
  hashmap <UPID, Owned<CRAMMD5AuthenticatorSession>> sessions;
};


namespace secrets {

// Loads secrets (principal -> secret) into the in-memory auxiliary
// property plugin that is used by the authenticators.
void load(const std::map<string, string>& secrets)
{
  Multimap<string, Property> properties;

  foreachpair (const string& principal,
               const string& secret, secrets) {
    Property property;
    property.name = SASL_AUX_PASSWORD_PROP;
    property.values.push_back(secret);
    properties.put(principal, property);
  }

  InMemoryAuxiliaryPropertyPlugin::load(properties);
}


void load(const Credentials& credentials)
{
  std::map<string, string> secrets;
  foreach (const Credential& credential, credentials.credentials()) {
    secrets[credential.principal()] = credential.secret();
  }
  load(secrets);
}

} // namespace secrets {


Try<Authenticator*> CRAMMD5Authenticator::create()
{
  return new CRAMMD5Authenticator();
}


CRAMMD5Authenticator::CRAMMD5Authenticator() : process(nullptr) {}


CRAMMD5Authenticator::~CRAMMD5Authenticator()
{
  if (process != nullptr) {
    terminate(process);
    wait(process);
    delete process;
  }
}


Try<Nothing> CRAMMD5Authenticator::initialize(
    const Option<Credentials>& credentials)
{
  static Once* initialize = new Once();

  // The 'error' is set atmost once per os process.
  // To allow subsequent calls to return the possibly set Error
  // object, we make this a static pointer.
  static Option<Error>* error = new Option<Error>();

  if (process != nullptr) {
    return Error("Authenticator initialized already");
  }

  if (credentials.isSome()) {
    // Load the credentials into the auxiliary memory plugin's storage.
    // It is necessary for this to be re-entrant as our tests may
    // re-load credentials.
    secrets::load(credentials.get());
  } else {
    LOG(WARNING) << "No credentials provided, authentication requests will be "
                 << "refused";
  }

  // Initialize SASL and add the auxiliary memory plugin. We must
  // not do this more than once per os-process.
  if (!initialize->once()) {
    LOG(INFO) << "Initializing server SASL";

    int result = sasl_server_init(nullptr, "mesos");

    if (result != SASL_OK) {
      *error = Error(
          string("Failed to initialize SASL: ") +
          sasl_errstring(result, nullptr, nullptr));
    } else {
      result = sasl_auxprop_add_plugin(
          InMemoryAuxiliaryPropertyPlugin::name(),
          &InMemoryAuxiliaryPropertyPlugin::initialize);

      if (result != SASL_OK) {
        *error = Error(
            string("Failed to add in-memory auxiliary property plugin: ") +
            sasl_errstring(result, nullptr, nullptr));
      }
    }

    initialize->done();
  }

  if (error->isSome()) {
    return error->get();
  }

  process = new CRAMMD5AuthenticatorProcess();
  spawn(process);

  return Nothing();
}


Future<Option<string>> CRAMMD5Authenticator::authenticate(
    const UPID& pid)
{
  if (process == nullptr) {
    return Failure("Authenticator not initialized");
  }
  return dispatch(
      process, &CRAMMD5AuthenticatorProcess::authenticate, pid);
}

} // namespace cram_md5 {
} // namespace internal {
} // namespace mesos {

#ifdef __APPLE__
#pragma GCC diagnostic pop
#endif
