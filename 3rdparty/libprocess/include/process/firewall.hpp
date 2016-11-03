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

#ifndef __PROCESS_FIREWALL_HPP__
#define __PROCESS_FIREWALL_HPP__

#include <string>

#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/error.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>

namespace process {
namespace firewall {

/**
 * A 'FirewallRule' describes an interface which provides control
 * over incoming HTTP requests while also taking the underlying
 * connection into account.
 *
 * Concrete classes based on this interface must implement the
 * 'apply' method.
 *
 * Rules can be installed using the free function
 * 'process::firewall::install()' defined in 'process.hpp'.
 */
class FirewallRule
{
public:
  FirewallRule() {}
  virtual ~FirewallRule() {}

  /**
   * Verify rule by applying it to an HTTP request and its underlying
   * socket connection.
   *
   * @param socket Socket used to deliver the HTTP request.
   * @param request HTTP request made by the client to libprocess.
   * @return If the rule verification fails, i.e. the rule didn't
   *     match, a pointer to a 'http::Response' object containing the
   *     HTTP error code and possibly a message indicating the reason
   *     for failure. Otherwise an unset 'Option' object.
   */
  virtual Option<http::Response> apply(
      const network::inet::Socket& socket,
      const http::Request& request) = 0;
};


/**
 * Simple firewall rule to forbid any HTTP request to a path
 * in the provided list of endpoints.
 *
 * Matches are required to be exact, no substrings nor wildcards are
 * considered for a match.
 */
class DisabledEndpointsFirewallRule : public FirewallRule
{
public:
  explicit DisabledEndpointsFirewallRule(const hashset<std::string>& _paths);

  virtual ~DisabledEndpointsFirewallRule() {}

  virtual Option<http::Response> apply(
      const network::inet::Socket&,
      const http::Request& request)
  {
    if (paths.contains(request.url.path)) {
      return http::Forbidden("Endpoint '" + request.url.path + "' is disabled");
    }

    return None();
  }

private:
  hashset<std::string> paths;
};

} // namespace firewall {
} // namespace process {

#endif // __PROCESS_FIREWALL_HPP__
