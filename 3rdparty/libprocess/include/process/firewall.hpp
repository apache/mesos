/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

#ifndef __PROCESS_FIREWALL_HPP__
#define __PROCESS_FIREWALL_HPP__

#include <string>

#include <process/http.hpp>
#include <process/socket.hpp>

#include <stout/hashset.hpp>

namespace process {
namespace firewall {

/**
 * A 'FirewallRule' is an interface which allows control over which
 * incoming HTTP connections are allowed. Concrete classes based on
 * this interface must implement the 'apply' method; this method
 * receives as parameters the socket where the connection is being
 * initiated, as well as the request itself.
 *
 * Rules can be installed using the free function
 * 'process::firewall::install()' defined in 'process.hpp'
 */
class FirewallRule
{
public:
  FirewallRule() {}
  virtual ~FirewallRule() {}

  /**
   * Method used to do inspection of incoming HTTP requests. It allows
   * implementations to verify conditions on the requests and notify
   * the caller if the rule has been broken.
   *
   * @param socket Socket used to attempt the HTTP connection.
   * @param request HTTP request made by the client to libprocess.
   * @return If the condition verification fails, i.e. the condition
   *     has been broken, the returned Try contains an error.
   */
  virtual Try<Nothing> apply(
      const network::Socket& socket,
      const http::Request& request) = 0;
};


/**
 * Simple firewall rule to reject any connection requesting a path
 * in the provided list of disabled endpoints.
 *
 * Matches are required to be exact, no substrings nor wildcards are
 * considered for a match.
 */
class DisabledEndpointsFirewallRule : public FirewallRule
{
public:
  explicit DisabledEndpointsFirewallRule(const hashset<std::string>& _paths)
    : paths(_paths) {}

  virtual ~DisabledEndpointsFirewallRule() {}

  virtual Try<Nothing> apply(
      const network::Socket&,
      const http::Request& request)
  {
    if (paths.contains(request.path)) {
      return Error("'" + request.path + "' is disabled");
    }

    return Nothing();
  }

private:
  hashset<std::string> paths;
};

} // namespace firewall {
} // namespace process {

#endif // __PROCESS_FIREWALL_HPP__
