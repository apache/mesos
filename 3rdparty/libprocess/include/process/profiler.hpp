// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_PROFILER_HPP__
#define __PROCESS_PROFILER_HPP__

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

namespace process {

class Profiler : public Process<Profiler>
{
public:
  Profiler(const Option<std::string>& _authenticationRealm)
    : ProcessBase("profiler"),
      authenticationRealm(_authenticationRealm) {}

  ~Profiler() override {}

protected:
  void initialize() override
  {
    route("/start",
          authenticationRealm,
          START_HELP(),
          &Profiler::start);

    route("/stop",
          authenticationRealm,
          STOP_HELP(),
          &Profiler::stop);
  }

private:
  static const std::string START_HELP();
  static const std::string STOP_HELP();

  // HTTP endpoints.

  // Starts the profiler. There are no request parameters.
  Future<http::Response> start(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // Stops the profiler. There are no request parameters.
  // This returns the profile output, it will also remain present
  // in the working directory.
  Future<http::Response> stop(
      const http::Request& request,
      const Option<http::authentication::Principal>&);

  // The authentication realm that the profiler's HTTP endpoints will be
  // installed into.
  Option<std::string> authenticationRealm;
};

} // namespace process {

#endif // __PROCESS_PROCESS_HPP__
