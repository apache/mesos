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

#include "authentication/cram_md5/authenticatee.hpp"

#include <stddef.h>   // For size_t needed by sasl.h.

#include <sasl/sasl.h>

#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/once.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/strings.hpp>

#include "logging/logging.hpp"

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

class CRAMMD5AuthenticateeProcess
  : public ProtobufProcess<CRAMMD5AuthenticateeProcess>
{
public:
  CRAMMD5AuthenticateeProcess(
      const Credential& _credential,
      const UPID& _client)
    : ProcessBase(ID::generate("crammd5-authenticatee")),
      credential(_credential),
      client(_client),
      status(READY),
      connection(nullptr)
  {
    const char* data = credential.secret().data();
    size_t length = credential.secret().length();

    // Need to allocate the secret via 'malloc' because SASL is
    // expecting the data appended to the end of the struct. *sigh*
    secret = (sasl_secret_t*) malloc(sizeof(sasl_secret_t) + length);

    CHECK(secret != nullptr) << "Failed to allocate memory for secret";

    memcpy(secret->data, data, length);
    secret->len = static_cast<unsigned long>(length);
  }

  ~CRAMMD5AuthenticateeProcess() override
  {
    if (connection != nullptr) {
      sasl_dispose(&connection);
    }
    free(secret);
  }

  void finalize() override
  {
    discarded(); // Fail the promise.
  }

  Future<bool> authenticate(const UPID& pid)
  {
    static Once* initialize = new Once();
    static bool initialized = false;

    if (!initialize->once()) {
      LOG(INFO) << "Initializing client SASL";
      int result = sasl_client_init(nullptr);
      if (result != SASL_OK) {
        status = ERROR;
        string error(sasl_errstring(result, nullptr, nullptr));
        promise.fail("Failed to initialize SASL: " + error);
        initialize->done();
        return promise.future();
      }

      initialized = true;

      initialize->done();
    }

    if (!initialized) {
      promise.fail("Failed to initialize SASL");
      return promise.future();
    }

    if (status != READY) {
      return promise.future();
    }

    LOG(INFO) << "Creating new client SASL connection";

    callbacks[0].id = SASL_CB_GETREALM;
    callbacks[0].proc = nullptr;
    callbacks[0].context = nullptr;

    callbacks[1].id = SASL_CB_USER;
    callbacks[1].proc = (int(*)()) &user;
    callbacks[1].context = (void*) credential.principal().c_str();

    // NOTE: Some SASL mechanisms do not allow/enable "proxying",
    // i.e., authorization. Therefore, some mechanisms send _only_ the
    // authorization name rather than both the user (authentication
    // name) and authorization name. Thus, for now, we assume
    // authorization is handled out of band. Consider the
    // SASL_NEED_PROXY flag if we want to reconsider this in the
    // future.
    callbacks[2].id = SASL_CB_AUTHNAME;
    callbacks[2].proc = (int(*)()) &user;
    callbacks[2].context = (void*) credential.principal().c_str();

    callbacks[3].id = SASL_CB_PASS;
    callbacks[3].proc = (int(*)()) &pass;
    callbacks[3].context = (void*) secret;

    callbacks[4].id = SASL_CB_LIST_END;
    callbacks[4].proc = nullptr;
    callbacks[4].context = nullptr;

    int result = sasl_client_new(
        "mesos",    // Registered name of service.
        nullptr,    // Server's FQDN.
        nullptr,    // IP Address information string.
        nullptr,    // IP Address information string.
        callbacks,  // Callbacks supported only for this connection.
        0,          // Security flags (security layers are enabled
                    // using security properties, separately).
        &connection);

    if (result != SASL_OK) {
      status = ERROR;
      string error(sasl_errstring(result, nullptr, nullptr));
      promise.fail("Failed to create client SASL connection: " + error);
      return promise.future();
    }

    AuthenticateMessage message;
    message.set_pid(client);
    send(pid, message);

    status = STARTING;

    // Stop authenticating if nobody cares.
    promise.future().onDiscard(defer(self(), &Self::discarded));

    return promise.future();
  }

protected:
  void initialize() override
  {
    // Anticipate mechanisms and steps from the server.
    install<AuthenticationMechanismsMessage>(
        &CRAMMD5AuthenticateeProcess::mechanisms,
        &AuthenticationMechanismsMessage::mechanisms);

    install<AuthenticationStepMessage>(
        &CRAMMD5AuthenticateeProcess::step,
        &AuthenticationStepMessage::data);

    install<AuthenticationCompletedMessage>(
        &CRAMMD5AuthenticateeProcess::completed);

    install<AuthenticationFailedMessage>(
        &CRAMMD5AuthenticateeProcess::failed);

    install<AuthenticationErrorMessage>(
        &CRAMMD5AuthenticateeProcess::error,
        &AuthenticationErrorMessage::error);
  }

  void mechanisms(const std::vector<string>& mechanisms)
  {
    if (status != STARTING) {
      status = ERROR;
      promise.fail("Unexpected authentication 'mechanisms' received");
      return;
    }

    // TODO(benh): Store 'from' in order to ensure we only communicate
    // with the same Authenticator.

    LOG(INFO) << "Received SASL authentication mechanisms: "
              << strings::join(",", mechanisms);

    sasl_interact_t* interact = nullptr;
    const char* output = nullptr;
    unsigned length = 0;
    const char* mechanism = nullptr;

    int result = sasl_client_start(
        connection,
        strings::join(" ", mechanisms).c_str(),
        &interact,     // Set if an interaction is needed.
        &output,       // The output string (to send to server).
        &length,       // The length of the output string.
        &mechanism);   // The chosen mechanism.

    CHECK_NE(SASL_INTERACT, result)
      << "Not expecting an interaction (ID: " << interact->id << ")";

    if (result != SASL_OK && result != SASL_CONTINUE) {
      string error(sasl_errdetail(connection));
      status = ERROR;
      promise.fail("Failed to start the SASL client: " + error);
      return;
    }

    LOG(INFO) << "Attempting to authenticate with mechanism '"
              << mechanism << "'";

    AuthenticationStartMessage message;
    message.set_mechanism(mechanism);
    message.set_data(output, length);

    reply(message);

    status = STEPPING;
  }

  void step(const string& data)
  {
    if (status != STEPPING) {
      status = ERROR;
      promise.fail("Unexpected authentication 'step' received");
      return;
    }

    LOG(INFO) << "Received SASL authentication step";

    sasl_interact_t* interact = nullptr;
    const char* output = nullptr;
    unsigned length = 0;

    int result = sasl_client_step(
        connection,
        data.length() == 0 ? nullptr : data.data(),
        static_cast<unsigned>(data.length()),
        &interact,
        &output,
        &length);

    CHECK_NE(SASL_INTERACT, result)
      << "Not expecting an interaction (ID: " << interact->id << ")";

    if (result == SASL_OK || result == SASL_CONTINUE) {
      // We don't start the client with SASL_SUCCESS_DATA so we may
      // need to send one more "empty" message to the server.
      AuthenticationStepMessage message;
      if (output != nullptr && length > 0) {
        message.set_data(output, length);
      }
      reply(message);
    } else {
      status = ERROR;
      string error(sasl_errdetail(connection));
      promise.fail("Failed to perform authentication step: " + error);
    }
  }

  void completed()
  {
    if (status != STEPPING) {
      status = ERROR;
      promise.fail("Unexpected authentication 'completed' received");
      return;
    }

    LOG(INFO) << "Authentication success";

    status = COMPLETED;
    promise.set(true);
  }

  void failed()
  {
    status = FAILED;
    promise.set(false);
  }

  void error(const string& error)
  {
    status = ERROR;
    promise.fail("Authentication error: " + error);
  }

  void discarded()
  {
    status = DISCARDED;
    promise.fail("Authentication discarded");
  }

private:
  static int user(
      void* context,
      int id,
      const char** result,
      unsigned* length)
  {
    CHECK(SASL_CB_USER == id || SASL_CB_AUTHNAME == id);
    *result = static_cast<const char*>(context);
    if (length != nullptr) {
      *length = static_cast<unsigned>(strlen(*result));
    }
    return SASL_OK;
  }

  static int pass(
      sasl_conn_t* connection,
      void* context,
      int id,
      sasl_secret_t** secret)
  {
    CHECK_EQ(SASL_CB_PASS, id);
    *secret = static_cast<sasl_secret_t*>(context);
    return SASL_OK;
  }

  const Credential credential;

  // PID of the client that needs to be authenticated.
  const UPID client;

  sasl_secret_t* secret;

  sasl_callback_t callbacks[5];

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

  sasl_conn_t* connection;

  Promise<bool> promise;
};


Try<Authenticatee*> CRAMMD5Authenticatee::create()
{
  return new CRAMMD5Authenticatee();
}


CRAMMD5Authenticatee::CRAMMD5Authenticatee() : process(nullptr) {}


CRAMMD5Authenticatee::~CRAMMD5Authenticatee()
{
  if (process != nullptr) {
    terminate(process);
    wait(process);
    delete process;
  }
}


Future<bool> CRAMMD5Authenticatee::authenticate(
  const UPID& pid,
  const UPID& client,
  const mesos::Credential& credential)
{
  if (!credential.has_secret()) {
    LOG(WARNING) << "Authentication failed; secret needed by CRAM-MD5 "
                 << "authenticatee";
    return false;
  }

  CHECK(process == nullptr);
  process = new CRAMMD5AuthenticateeProcess(credential, client);
  spawn(process);

  return dispatch(
      process, &CRAMMD5AuthenticateeProcess::authenticate, pid);
}

} // namespace cram_md5 {
} // namespace internal {
} // namespace mesos {

#ifdef __APPLE__
#pragma GCC diagnostic pop
#endif
