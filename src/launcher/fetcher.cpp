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

#include <string>

#include <mesos/mesos.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "hdfs/hdfs.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

using namespace mesos;

using mesos::fetcher::FetcherInfo;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


const char FILE_URI_PREFIX[] = "file://";
const char FILE_URI_LOCALHOST[] = "file://localhost";

// Try to extract filename into directory. If filename is recognized as an
// archive it will be extracted and true returned; if not recognized then false
// will be returned. An Error is returned if the extraction command fails.
Try<bool> extract(const string& filename, const string& directory)
{
  string command;
  // Extract any .tgz, tar.gz, tar.bz2 or zip files.
  if (strings::endsWith(filename, ".tgz") ||
      strings::endsWith(filename, ".tar.gz") ||
      strings::endsWith(filename, ".tbz2") ||
      strings::endsWith(filename, ".tar.bz2") ||
      strings::endsWith(filename, ".txz") ||
      strings::endsWith(filename, ".tar.xz")) {
    command = "tar -C '" + directory + "' -xf";
  } else if (strings::endsWith(filename, ".zip")) {
    command = "unzip -d '" + directory + "'";
  } else {
    return false;
  }

  command += " '" + filename + "'";
  int status = os::system(command);
  if (status != 0) {
    return Error("Failed to extract: command " + command +
                 " exited with status: " + stringify(status));
  }

  LOG(INFO) << "Extracted resource '" << filename
            << "' into '" << directory << "'";

  return true;
}


// Attempt to get the uri using the hadoop client.
Try<string> fetchWithHadoopClient(
    const string& uri,
    const string& directory)
{
  HDFS hdfs;
  Try<bool> available = hdfs.available();

  if (available.isError() || !available.get()) {
    LOG(INFO) << "Hadoop Client not available, "
              << "skipping fetch with Hadoop Client";
    return Error("Hadoop Client unavailable");
  }

  LOG(INFO) << "Fetching URI '" << uri << "' using Hadoop Client";

  Try<string> base = os::basename(uri);
  if (base.isError()) {
    LOG(ERROR) << "Invalid basename for URI: " << base.error();
    return Error("Invalid basename for URI");
  }

  string path = path::join(directory, base.get());

  LOG(INFO) << "Downloading resource from '" << uri  << "' to '" << path << "'";

  Try<Nothing> result = hdfs.copyToLocal(uri, path);
  if (result.isError()) {
    LOG(ERROR) << "HDFS copyToLocal failed: " << result.error();
    return Error(result.error());
  }

  return path;
}


Try<string> fetchWithNet(
    const string& uri,
    const string& directory)
{
  LOG(INFO) << "Fetching URI '" << uri << "' with os::net";

  string path = uri.substr(uri.find("://") + 3);
  if (path.find("/") == string::npos ||
      path.size() <= path.find("/") + 1) {
    LOG(ERROR) << "Malformed URL (missing path)";
    return Error("Malformed URI");
  }

  path = path::join(directory, path.substr(path.find_last_of("/") + 1));
  LOG(INFO) << "Downloading '" << uri << "' to '" << path << "'";
  Try<int> code = net::download(uri, path);
  if (code.isError()) {
    LOG(ERROR) << "Error downloading resource: " << code.error().c_str();
    return Error("Fetch of URI failed (" + code.error() + ")");
  } else if (code.get() != 200) {
    LOG(ERROR) << "Error downloading resource, received HTTP/FTP return code "
    << code.get();
    return Error("HTTP/FTP error (" + stringify(code.get()) + ")");
  }

  return path;
}


Try<string> fetchWithLocalCopy(
    const string& uri,
    const string& directory)
{
    string local = uri;
    bool fileUri = false;
    if (strings::startsWith(local, string(FILE_URI_LOCALHOST))) {
        local = local.substr(sizeof(FILE_URI_LOCALHOST) - 1);
        fileUri = true;
    } else if (strings::startsWith(local, string(FILE_URI_PREFIX))) {
        local = local.substr(sizeof(FILE_URI_PREFIX) - 1);
        fileUri = true;
    }

    if (fileUri && !strings::startsWith(local, "/")) {
        return Error("File URI only supports absolute paths");
    }

    if (local.find_first_of("/") != 0) {
        // We got a non-Hadoop and non-absolute path.
        if (os::hasenv("MESOS_FRAMEWORKS_HOME")) {
            local = path::join(os::getenv("MESOS_FRAMEWORKS_HOME"), local);
            LOG(INFO) << "Prepended environment variable "
            << "MESOS_FRAMEWORKS_HOME to relative path, "
            << "making it: '" << local << "'";
        } else {
            LOG(ERROR) << "A relative path was passed for the resource but the "
            << "environment variable MESOS_FRAMEWORKS_HOME is not set. "
            << "Please either specify this config option "
            << "or avoid using a relative path";
            return Error("Could not resolve relative URI");
        }
    }

    Try<string> base = os::basename(local);
    if (base.isError()) {
        LOG(ERROR) << base.error();
        return Error("Fetch of URI failed");
    }

    // Copy the resource to the directory.
    string path = path::join(directory, base.get());
    std::ostringstream command;
    command << "cp '" << local << "' '" << path << "'";
    LOG(INFO) << "Copying resource from '" << local
    << "' to '" << directory << "'";

    int status = os::system(command.str());
    if (status != 0) {
        LOG(ERROR) << "Failed to copy '" << local
        << "' : Exit status " << status;
        return Error("Local copy failed");
    }

    return path;
}


// Fetch URI into directory.
Try<string> fetch(
    const string& uri,
    const string& directory)
{
    LOG(INFO) << "Fetching URI '" << uri << "'";
    // Some checks to make sure using the URI value in shell commands
    // is safe. TODO(benh): These should be pushed into the scheduler
    // driver and reported to the user.
    if (uri.find_first_of('\\') != string::npos ||
        uri.find_first_of('\'') != string::npos ||
        uri.find_first_of('\0') != string::npos) {
        LOG(ERROR) << "URI contains illegal characters, refusing to fetch";
        return Error("Illegal characters in URI");
    }

    // 1. Try to fetch using a local copy.
    // We consider file:// or no scheme uri are considered local uri.
    if (strings::startsWith(uri, "file://") ||
        uri.find("://") == string::npos) {
      return fetchWithLocalCopy(uri, directory);
    }

    // 2. Try to fetch URI using os::net / libcurl implementation.
    // We consider http, https, ftp, ftps compatible with libcurl
    if (strings::startsWith(uri, "http://") ||
               strings::startsWith(uri, "https://") ||
               strings::startsWith(uri, "ftp://") ||
               strings::startsWith(uri, "ftps://")) {
      return fetchWithNet(uri, directory);
    }

    // 3. Try to fetch the URI using hadoop client.
    // We use the hadoop client to fetch any URIs that are not
    // handled by other fetchers(local / os::net). These URIs may be
    // `hdfs://` URIs or any other URI that has been configured (and
    // hence handled) in the hadoop client. This allows mesos to
    // externalize the handling of previously unknown resource
    // endpoints without the need to support them natively.
    // Note: Hadoop Client is not a hard dependency for running mesos.
    // This allows users to get mesos up and running without a
    // hadoop_home or the hadoop client setup but in case we reach
    // this part and don't have one configured, the fetch would fail
    // and log an appropriate error.
    return fetchWithHadoopClient(uri, directory);
}


int main(int argc, char* argv[])
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  mesos::logging::Flags flags;

  Try<Nothing> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    exit(1);
  }

  logging::initialize(argv[0], flags, true); // Catch signals.

  CHECK(os::hasenv("MESOS_FETCHER_INFO"))
    << "Missing MESOS_FETCHER_INFO environment variable";

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(os::getenv("MESOS_FETCHER_INFO"));

  if (parse.isError()) {
    EXIT(1) << "Failed to parse MESOS_FETCHER_INFO: " << parse.error();
  }

  Try<FetcherInfo> fetcherInfo = protobuf::parse<FetcherInfo>(parse.get());
  if (fetcherInfo.isError()) {
    EXIT(1) << "Failed to parse FetcherInfo: " << fetcherInfo.error();
  }

  const CommandInfo& commandInfo = fetcherInfo.get().command_info();

  const string& directory = fetcherInfo.get().work_directory();
  if (directory.empty()) {
    EXIT(1) << "Missing work directory";
  }

  Option<std::string> user = None();
  if (fetcherInfo.get().has_user()) {
    user = fetcherInfo.get().user();
  }

  // Fetch each URI to a local file, chmod, then chown if a user is provided.
  foreach (const CommandInfo::URI& uri, commandInfo.uris()) {
    // Fetch the URI to a local file.
    Try<string> fetched = fetch(uri.value(), directory);
    if (fetched.isError()) {
      EXIT(1) << "Failed to fetch: " << uri.value();
    }

    // Chmod the fetched URI if it's executable, else assume it's an archive
    // that should be extracted.
    if (uri.executable()) {
      Try<Nothing> chmod = os::chmod(
          fetched.get(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
      if (chmod.isError()) {
        EXIT(1) << "Failed to chmod " << fetched.get() << ": " << chmod.error();
      }
    } else if (uri.extract()) {
      // TODO(idownes): Consider removing the archive once extracted.
      // Try to extract the file if it's recognized as an archive.
      Try<bool> extracted = extract(fetched.get(), directory);
      if (extracted.isError()) {
        EXIT(1) << "Failed to extract "
                << fetched.get() << ":" << extracted.error();
      }
    } else {
      LOG(INFO) << "Skipped extracting path '" << fetched.get() << "'";
    }

    // Recursively chown the directory if a user is provided.
    if (user.isSome()) {
      Try<Nothing> chowned = os::chown(user.get(), directory);
      if (chowned.isError()) {
        EXIT(1) << "Failed to chown " << directory << ": " << chowned.error();
      }
    }
  }

  return 0;
}
