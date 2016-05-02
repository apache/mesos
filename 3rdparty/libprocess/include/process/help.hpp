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

#ifndef __PROCESS_HELP_HPP__
#define __PROCESS_HELP_HPP__

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/preprocessor.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

namespace process {

// Constructs a Markdown based help "page" for a route with the
// following template:
//
//     ### USAGE ###
//     usage
//
//     ### TL;DR; ###
//     tldr
//
//     ### DESCRIPTION ###
//     description
//
//     ### AUTHENTICATION ###
//     authentication requirements
//
//     ### AUTHORIZATION ###
//     authorization requirements and granularity
//
//     references
//
// See the 'USAGE', 'TLDR', 'DESCRIPTION', 'AUTHENTICATION', and
// 'REFERENCES' helpers below to more easily construct your help pages.
std::string HELP(
    const std::string& tldr,
    const Option<std::string>& description = None(),
    const Option<std::string>& authentication = None(),
    const Option<std::string>& authorization = None(),
    const Option<std::string>& references = None());

// Helper for single-line usage that puts it in a blockquote as code
// and adds a newline.
inline std::string USAGE(const std::string& usage)
{
  return ">        " + usage + "\n";
}


// Helper for single-line TL;DR; that adds a newline.
inline std::string TLDR(const std::string& tldr)
{
  return tldr + "\n";
}


template <typename... T>
inline std::string DESCRIPTION(T&&... args)
{
  return strings::join("\n", std::forward<T>(args)..., "\n");
}


// Helper for description of Authentication requirements.
inline std::string AUTHENTICATION(bool required)
{
  if (required) {
    return "This endpoint requires authentication iff HTTP authentication is\n"
           "enabled.\n";
  }
  return "This endpoint does not require authentication.\n";
}


// Helper for description of Authorization requirements.
template <typename... T>
inline std::string AUTHORIZATION(T&&... args)
{
  return strings::join("\n", std::forward<T>(args)..., "\n");
}


template <typename... T>
inline std::string REFERENCES(T&&... args)
{
  return strings::join("\n", std::forward<T>(args)..., "\n");
}


// Help process for serving /help, /help/id, and /help/id/name (see
// Help::help below for more information).
class Help : public Process<Help>
{
public:
  Help(const Option<std::string>& delegate);

  // Adds 'help' for the route 'name' of the process with the
  // specified 'id' (i.e., 'http://ip:port/id/name'). It's expected
  // that 'help' is written using Markdown. When serving help to a
  // browser the Markdown will be rendered into HTML while a tool like
  // 'curl' or 'http' will just be given the Markdown directly (thus
  // making it easy to get help without opening a browser).
  // NOTE: There is no need to dispatch this directly; this gets
  // automagically dispatched by 'ProcessBase::route'.
  void add(const std::string& id,
           const std::string& name,
           const Option<std::string>& help);

  // Remove a previously installed 'help' string for '/id/name'.
  bool remove(const std::string& id, const std::string& name);

  // Remove all previously installed 'help' strings for '/id/*'.
  bool remove(const std::string& id);

  // Allow the global json function to dump this object to its
  // canonical JSON representation.
  friend void json(JSON::ObjectWriter* writer, const Help& help);

protected:
  virtual void initialize();

private:
  // Handles the following:
  //
  //   (1) http://ip:port/help
  //   (2) http://ip:port/help/id
  //   (3) http://ip:port/help/id/name
  //
  // Where 'id' and 'name' are replaced with a process ID and route
  // name respectively. (1) provides a "table of contents" for all
  // available processes while (2) provides a "table of contents" for
  // all endpoints associated with a particular process and (3)
  // provides the help associated with a particular endpoint of a
  // process.
  Future<http::Response> help(const http::Request& request);

  // Helper function to get usage path by process id and endpoint name.
  std::string getUsagePath(const std::string& id, const std::string& name);

  // Delegate process name to receive root HTTP requests.
  const Option<std::string> delegate;

  std::map<std::string, std::map<std::string, std::string>> helps;
};

} // namespace process {

#endif // __PROCESS_HELP_HPP__
