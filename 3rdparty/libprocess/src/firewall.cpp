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

#include <string>

#include <process/firewall.hpp>
#include <process/process.hpp>

#include <stout/hashset.hpp>

using std::string;

namespace process {
namespace firewall {

DisabledEndpointsFirewallRule::DisabledEndpointsFirewallRule(
    const hashset<string>& _paths)
{
  foreach (const string& path, _paths) {
    paths.insert(process::absolutePath(path));
  }
}

} // namespace firewall {
} // namespace process {
