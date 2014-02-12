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

#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "hdfs/hdfs.hpp"

using namespace mesos;

using std::string;

// Try to extract filename into directory. If filename is recognized as an
// archive it will be extracted and true returned; if not recognized then false
// will be returned. An Error is returned if the extraction command fails.
Try<Nothing> extract(const string& filename, const string& directory)
{
  string command;
  // Extract any .tgz, tar.gz, tar.bz2 or zip files.
  if (strings::endsWith(filename, ".tgz") ||
      strings::endsWith(filename, ".tar.gz") ||
      strings::endsWith(filename, ".tbz2") ||
      strings::endsWith(filename, ".tar.bz2") ||
      strings::endsWith(filename, ".txz") ||
      strings::endsWith(filename, ".tar.xz")) {
    command = "tar -C '" + directory + "' xJf";
  } else if (strings::endsWith(filename, ".zip")) {
    command = "unzip -d '" + directory + "'";
  } else {
    return Error("Could not extract file with unrecognized extension");
  }

  command += " '" + filename + "'";
  int status = os::system(command);
  if (status != 0) {
    return Error("Failed to extract: command " + command +
                 " exited with status: " + stringify(status));
  }

  LOG(INFO) << "Extracted resource '" << filename
            << "' into '" << directory << "'";

  return Nothing();
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

  // Grab the resource from HDFS if its path begins with hdfs:// or
  // hftp:
  // TODO(matei): Enforce some size limits on files we get from HDFS
  if (strings::startsWith(uri, "hdfs://") ||
      strings::startsWith(uri, "hftp://")) {
    Try<string> base = os::basename(uri);
    if (base.isError()) {
      LOG(ERROR) << "Invalid basename for URI: " << base.error();
      return Error("Invalid basename for URI");
    }
    string path = path::join(directory, base.get());

    HDFS hdfs;

    LOG(INFO) << "Downloading resource from '" << uri
              << "' to '" << path << "'";
    Try<Nothing> result = hdfs.copyToLocal(uri, path);
    if (result.isError()) {
      LOG(ERROR) << "HDFS copyToLocal failed: " << result.error();
      return Error(result.error());
    }

    return path;
  } else if (strings::startsWith(uri, "http://") ||
             strings::startsWith(uri, "https://") ||
             strings::startsWith(uri, "ftp://") ||
             strings::startsWith(uri, "ftps://")) {
    string path = uri.substr(uri.find("://") + 3);
    if (path.find("/") == string::npos ||
        path.size() <= path.find("/") + 1) {
      LOG(ERROR) << "Malformed URL (missing path)";
      return Error("Malformed URI");
    }

    path =  path::join(directory, path.substr(path.find_last_of("/") + 1));
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
  } else { // Copy the local resource.
    string local = uri;
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
}


int main(int argc, char* argv[])
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  CommandInfo commandInfo;
  // Construct URIs from the encoded environment string.
  const std::string& uris = os::getenv("MESOS_EXECUTOR_URIS");
  foreach (const std::string& token, strings::tokenize(uris, " ")) {
    // Delimiter between URI and execute permission.
    size_t pos = token.rfind("+");
    CHECK(pos != std::string::npos)
      << "Invalid executor uri token in env " << token;

    CommandInfo::URI uri;
    uri.set_value(token.substr(0, pos));
    uri.set_executable(token.substr(pos + 1) == "1");

    commandInfo.add_uris()->MergeFrom(uri);
  }

  CHECK(os::hasenv("MESOS_WORK_DIRECTORY"))
    << "Missing MESOS_WORK_DIRECTORY environment variable";
  std::string directory = os::getenv("MESOS_WORK_DIRECTORY");

  // We cannot use Some in the ternary expression because the compiler needs to
  // be able to infer the type, thus the explicit Option<string>.
  // TODO(idownes): Add an os::hasenv that returns an Option<string>.
  Option<std::string> user = os::hasenv("MESOS_USER")
    ? Option<std::string>(os::getenv("MESOS_USER")) // Explicit so it compiles.
    : None();

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
      bool chmodded = os::chmod(
          fetched.get(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
      if (!chmodded) {
        EXIT(1) << "Failed to chmod: " << fetched.get();
      }
    } else {
      //TODO(idownes): Consider removing the archive once extracted.
      // Try to extract the file if it's recognized as an archive.
      Try<Nothing> extracted = extract(fetched.get(), directory);
      if (extracted.isError()) {
        EXIT(1) << "Failed to extract "
                << fetched.get() << ":" << extracted.error();
      }
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
