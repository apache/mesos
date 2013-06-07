#ifndef __PROCESS_HELP_HPP__
#define __PROCESS_HELP_HPP__

#include <map>
#include <string>
#include <vector>

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
//     ### TL;DR; ###
//     tldr
//
//     ### USAGE ###
//     usage
//
//     ### DESCRIPTION ###
//     description
//
//     references
//
// See the 'TLDR', 'USAGE', 'DESCRIPTION', and 'REFERENCES' helpers
// below to more easily construct your help pages.
inline std::string HELP(
    std::string tldr,
    std::string usage,
    std::string description,
    const Option<std::string>& references = None())
{
  // Make sure 'tldr', 'usage', and 'description' end with a newline.
  if (!strings::endsWith(tldr, "\n")) {
    tldr += "\n";
  }

  if (!strings::endsWith(usage, "\n")) {
    usage += "\n";
  }

  if (!strings::endsWith(description, "\n")) {
    description += "\n";
  }

  // Construct the help string.
  std::string help =
    "### TL;DR; ###\n" +
    tldr +
    "\n" +
    "### USAGE ###\n" +
    usage +
    "\n" +
    "### DESCRIPTION ###\n" +
    description;

  if (references.isSome()) {
    help += "\n";
    help += references.get();
  }

  return help;
}


// Helper for single-line TL;DR; that adds a newline.
inline std::string TLDR(const std::string& tldr)
{
  return tldr + "\n";
}


// Helper for single-line usage that puts it in a blockquote as code
// and adds a newline.
inline std::string USAGE(const std::string& usage)
{
  return ">        " + usage + "\n";
}


// Helpers for adding newlines to each line of a multi-line
// description or references.
#define LINE_TEMPLATE(Z, N, DATA) + CAT(line, N) + "\n"
#define TEMPLATE(Z, N, DATA)                                            \
  inline std::string DESCRIPTION(                                       \
      ENUM_PARAMS(N, const std::string& line))                          \
  {                                                                     \
    return                                                              \
      ""                                                                \
      REPEAT_FROM_TO(0, N, LINE_TEMPLATE, _);                           \
  }                                                                     \
                                                                        \
                                                                        \
  inline std::string REFERENCES(                                        \
      ENUM_PARAMS(N, const std::string& line))                          \
  {                                                                     \
    return                                                              \
      ""                                                                \
      REPEAT_FROM_TO(0, N, LINE_TEMPLATE, _);                           \
  }

  REPEAT_FROM_TO(1, 201, TEMPLATE, _) // Lines 1 -> 200.
#undef TEMPLATE
#undef LINE_TEMPLATE


// Help process for serving /help, /help/id, and /help/id/name (see
// Help::help below for more information).
class Help : public Process<Help>
{
public:
  Help() : ProcessBase("help") {}

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
           const Option<std::string>& help)
  {
    if (id != "help") { // TODO(benh): Enable help for help.
      if (help.isSome()) {
        helps[id][name] = help.get();
      } else {
        helps[id][name] = "## No help page for `/" + id + name + "`\n";
      }
      route("/" + id, "Help for " + id, &Help::help);
    }
  }

protected:
  virtual void initialize()
  {
    route("/", None(), &Help::help);
  }

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
  Future<http::Response> help(const http::Request& request)
  {
    // Split the path by '/'.
    std::vector<std::string> tokens = strings::tokenize(request.path, "/");

    Option<std::string> id = None();
    Option<std::string> name = None();

    if (tokens.size() > 3) {
      return http::BadRequest("Malformed URL, expecting '/help/id/name/'\n");
    } else if (tokens.size() == 3) {
      id = tokens[1];
      name = tokens[2];
    } else if (tokens.size() > 1) {
      id = tokens[1];
    }

    std::string document;
    std::string references;

    if (id.isNone()) {             // http://ip:port/help
      document += "## HELP\n";
      foreachkey (const std::string& id, helps) {
        document += "> [/" + id + "][" + id + "]\n";
        references += "[" + id + "]: /help/" + id + "\n";
      }
    } else if (name.isNone()) {    // http://ip:port/help/id
      if (helps.count(id.get()) == 0) {
        return http::BadRequest(
            "No help available for '/" + id.get() + "'.\n");
      }

      document += "## `/" + id.get() + "` ##\n";
      foreachkey (const std::string& name, helps[id.get()]) {
        const std::string& path = id.get() + name;
        document += "> [/" +  path + "][" + path + "]\n";
        references += "[" + path + "]: /help/" + path + "\n";
      }
    } else {                       // http://ip:port/help/id/name
      if (helps.count(id.get()) == 0) {
        return http::BadRequest(
            "No help available for '/" + id.get() + "'.\n");
      } else if (helps[id.get()].count("/" + name.get()) == 0) {
        return http::BadRequest(
            "No help available for '/" + id.get() + "/" + name.get() + "'.\n");
      }

      document += helps[id.get()]["/" + name.get()];
    }

    // Final Markdown is 'document' followed by the 'references'.
    std::string markdown = document + "\n" + references;

    // Just send the Markdown if we aren't speaking to a browser. For
    // now we only check for the 'curl' or 'http' utilities.
    Option<std::string> agent = request.headers.get("User-Agent");

    if (agent.isSome() &&
        (strings::startsWith(agent.get(), "curl") ||
         strings::startsWith(agent.get(), "HTTPie"))) {
      http::Response response = http::OK(markdown);
      response.headers["Content-Type"] = "text/x-markdown";
      return response;
    }

    // Need to JSONify the markdown for embedding into JavaScript.
    markdown = stringify(JSON::String(markdown));

    // URL for jQuery.
    const std::string jquery =
      "https://ajax.googleapis.com/ajax/libs/jquery/1.10.1/jquery.min.js";

    // Assuming client has Internet access, provide some JavaScript to
    // render the Markdown into some aesthetically pleasing HTML. ;)
    // This currently uses GitHub to render the Markdown instead of
    // doing it client-side in the browser (e.g., using something like
    // 'showdown.js').
    return http::OK(
        "<html>"
        "<head>"
        "<title>Help</title>"
        "<script src=\"" + jquery + "\"></script>"
        "<script>"
        "  function loaded() {"
        "    var markdown = " + markdown + ";"
        "    if (typeof $ === 'undefined') {"
        "      document.body.innerHTML = '<pre>' + markdown + '</pre>';"
        "    } else {"
        "      var data = { text: markdown, mode: 'gfm' };"
        "      $.ajax({"
        "        type: 'POST',"
        "        url: 'https://api.github.com/markdown',"
        "        data: JSON.stringify(data),"
        "        success: function(data) {"
        "          document.body.innerHTML = data;"
        "        }"
        "      });"
        "    }"
        "  }"
        "</script>"
        "<style>"
        "body {"
        "  font-family: Helvetica, arial, sans-serif;"
        "  font-size: 14px;"
        "  line-height: 1.6;"
        "  padding-top: 10px;"
        "  padding-bottom: 10px;"
        "  background-color: white;"
        "  padding: 30px;"
        "}"
        "blockquote {"
        "  border-left: 5px solid #dddddd;"
        "  padding: 0 10px;"
        "  color: #777777;"
        "  margin: 0 0 20px;"
        "}"
        "a {"
        "  color: #0088cc;"
        "  text-decoration: none;"
        "}"
        "</style>"
        "</head>"
        "<body onload=\"loaded()\">"
        "</body>"
        "</html>");
  }

  std::map<std::string, std::map<std::string, std::string> > helps;
};

} // namespace process {

#endif // __PROCESS_HELP_HPP__
