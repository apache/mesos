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

#ifndef __SASL_AUTHENTICATEE_HPP__
#define __SASL_AUTHENTICATEE_HPP__

#include <sasl/sasl.h>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/once.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/strings.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace sasl {

// Forward declaration.
class AuthenticateeProcess;


class Authenticatee
{
public:
  // 'credential' is used to authenticate the 'client'.
  Authenticatee(const Credential& credential, const process::UPID& client);
  ~Authenticatee();

  // Returns true if successfully authenticated otherwise false or an
  // error. Note that we distinguish authentication failure (false)
  // from a failed future in the event the future failed due to a
  // transient error and authentication can (should) be retried.
  process::Future<bool> authenticate(const process::UPID& pid);

private:
  AuthenticateeProcess* process;
};


class AuthenticateeProcess : public ProtobufProcess<AuthenticateeProcess>
{
public:
  AuthenticateeProcess(const Credential& _credential,
                       const process::UPID& _client)
    : ProcessBase(process::ID::generate("authenticatee")),
      credential(_credential),
      client(_client),
      status(READY),
      connection(NULL)
  {
    const char* data = credential.secret().data();
    size_t length = credential.secret().length();

    // Need to allocate the secret via 'malloc' because SASL is
    // expecting the data appended to the end of the struct. *sigh*
    secret = (sasl_secret_t*) malloc(sizeof(sasl_secret_t) + length);

    CHECK(secret != NULL) << "Failed to allocate memory for secret";

    memcpy(secret->data, data, length);
    secret->len = length;
  }

  virtual ~AuthenticateeProcess()
  {
    if (connection != NULL) {
      sasl_dispose(&connection);
    }
    free(secret);
  }

  process::Future<bool> authenticate(const process::UPID& pid)
  {
    static process::Once* initialize = new process::Once();
    static bool initialized = false;

    if (!initialize->once()) {
      LOG(INFO) << "Initializing client SASL";
      int result = sasl_client_init(NULL);
      if (result != SASL_OK) {
        status = ERROR;
        std::string error(sasl_errstring(result, NULL, NULL));
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
    callbacks[0].proc = NULL;
    callbacks[0].context = NULL;

    callbacks[1].id = SASL_CB_USER;
    callbacks[1].proc = (int(*)()) &user;
    callbacks[2].context = (void*) credential.principal().c_str();

    // NOTE: Some SASL mechanisms do not allow/enable "proxying",
    // i.e., authorization. Therefore, some mechanisms send _only_ the
    // authoriation name rather than both the user (authentication
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
    callbacks[4].proc = NULL;
    callbacks[4].context = NULL;

    int result = sasl_client_new(
        "mesos",    // Registered name of service.
        NULL,       // Server's FQDN.
        NULL, NULL, // IP Address information strings.
        callbacks,  // Callbacks supported only for this connection.
        0,          // Security flags (security layers are enabled
                    // using security properties, separately).
        &connection);

    if (result != SASL_OK) {
      status = ERROR;
      std::string error(sasl_errstring(result, NULL, NULL));
      promise.fail("Failed to create client SASL connection: " + error);
      return promise.future();
    }

    AuthenticateMessage message;
    message.set_pid(client);
    send(pid, message);

    status = STARTING;

    return promise.future();
  }

protected:
  virtual void initialize()
  {
    // Anticipate mechanisms and steps from the server.
    install<AuthenticationMechanismsMessage>(
        &AuthenticateeProcess::mechanisms,
        &AuthenticationMechanismsMessage::mechanisms);

    install<AuthenticationStepMessage>(
        &AuthenticateeProcess::step,
        &AuthenticationStepMessage::data);

    install<AuthenticationCompletedMessage>(
        &AuthenticateeProcess::completed);

    install<AuthenticationFailedMessage>(
        &AuthenticateeProcess::failed);

    install<AuthenticationErrorMessage>(
        &AuthenticateeProcess::error,
        &AuthenticationErrorMessage::error);
  }

  void mechanisms(const std::vector<std::string>& mechanisms)
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

    sasl_interact_t* interact = NULL;
    const char* output = NULL;
    unsigned length = 0;
    const char* mechanism = NULL;

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
      std::string error(sasl_errdetail(connection));
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

  void step(const std::string& data)
  {
    if (status != STEPPING) {
      status = ERROR;
      promise.fail("Unexpected authentication 'step' received");
      return;
    }

    LOG(INFO) << "Received SASL authentication step";

    sasl_interact_t* interact = NULL;
    const char* output = NULL;
    unsigned length = 0;

    int result = sasl_client_step(
        connection,
        data.length() == 0 ? NULL : data.data(),
        data.length(),
        &interact,
        &output,
        &length);

    CHECK_NE(SASL_INTERACT, result)
      << "Not expecting an interaction (ID: " << interact->id << ")";

    if (result == SASL_OK || result == SASL_CONTINUE) {
      // We don't start the client with SASL_SUCCESS_DATA so we may
      // need to send one more "empty" message to the server.
      AuthenticationStepMessage message;
      if (output != NULL && length > 0) {
        message.set_data(output, length);
      }
      reply(message);
    } else {
      status = ERROR;
      std::string error(sasl_errdetail(connection));
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

  void error(const std::string& error)
  {
    status = ERROR;
    promise.fail("Authentication error: " + error);
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
    if (length != NULL) {
      *length = strlen(*result);
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
  const process::UPID client;

  sasl_secret_t* secret;

  sasl_callback_t callbacks[5];

  enum {
    READY,
    STARTING,
    STEPPING,
    COMPLETED,
    FAILED,
    ERROR
  } status;

  sasl_conn_t* connection;

  process::Promise<bool> promise;
};


Authenticatee::Authenticatee(
    const Credential& credential,
    const process::UPID& client)
{
  process = new AuthenticateeProcess(credential, client);
  process::spawn(process);
}


Authenticatee::~Authenticatee()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


process::Future<bool> Authenticatee::authenticate(const process::UPID& pid)
{
  return process::dispatch(process, &AuthenticateeProcess::authenticate, pid);
}

} // namespace sasl {
} // namespace internal {
} // namespace mesos {

#endif //__SASL_AUTHENTICATEE_HPP__
