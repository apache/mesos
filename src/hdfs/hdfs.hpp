#ifndef __HDFS_HPP__
#define __HDFS_HPP__

#include <sstream>
#include <vector>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>


// TODO(benh): We should get the hostname:port (or ip:port) of the
// server via:
//
//  hadoop dfsadmin -report | grep Name: | awk '{ print $2 }'
//
// The advantage of doing this is then we can explicitly use the
// 'hdfs://hostname' prefix when we're trying to do copies to avoid
// silent failures when HDFS is down and the tools just copies
// locally.
//
// Note that if HDFS is not on port 9000 then we'll also need to do an
// HTTP GET on hostname:port and grab the information in the
// <title>...</title> (this is the best hack I can think of to get
// 'fs.default.name' given the tools available).
struct HDFS
{
  // Look for `hadoop' first where proposed, otherwise, look for
  // HADOOP_HOME, otherwise, assume it's on the PATH.
  explicit HDFS(const std::string& _hadoop)
    : hadoop(os::exists(_hadoop)
             ? _hadoop
             : (os::getenv("HADOOP_HOME").isSome()
                ? path::join(os::getenv("HADOOP_HOME").get(), "bin/hadoop")
                : "hadoop")) {}

  // Look for `hadoop' in HADOOP_HOME or assume it's on the PATH.
  HDFS()
    : hadoop(os::getenv("HADOOP_HOME").isSome()
             ? path::join(os::getenv("HADOOP_HOME").get(), "bin/hadoop")
             : "hadoop") {}

  // Check if hadoop client is available at the path that was set.
  // This can be done by executing `hadoop version` command and
  // checking for status code == 0.
  Try<bool> available()
  {
    Try<std::string> command = strings::format("%s version", hadoop);

    CHECK_SOME(command);

    Try<int> status = os::shell(NULL, command.get() + " 2>&1");

    if(status.isError()) {
      return Error(status.error());
    }
    return status.get() == 0;
  }

  Try<bool> exists(std::string path)
  {
    // Make sure 'path' starts with a '/'.
    path = path::join("", path);

    Try<std::string> command = strings::format(
        "%s fs -test -e '%s'", hadoop, path);

    CHECK_SOME(command);

    Try<int> status = os::shell(NULL, command.get() + " 2>&1");

    if (status.isError()) {
      return Error(status.error());
    }

    return status.get() == 0;
  }

  Try<Bytes> du(std::string path)
  {
    // Make sure 'path' starts with a '/'.
    path = path::join("", path);

    Try<std::string> command = strings::format(
        "%s fs -du -h '%s'", hadoop, path);

    CHECK_SOME(command);

    std::ostringstream output;

    Try<int> status = os::shell(&output, command.get() + " 2>&1");

    if (status.isError()) {
      return Error("HDFS du failed: " + status.error());
    }

    const std::vector<std::string>& s = strings::split(output.str(), " ");
    if (s.size() != 2) {
      return Error("HDFS du returned an unexpected number of results: '" +
                   output.str() + "'");
    }

    Result<size_t> size = numify<size_t>(s[0]);
    if (size.isError()) {
      return Error("HDFS du returned unexpected format: " + size.error());
    } else if (size.isNone()) {
      return Error("HDFS du returned unexpected format");
    }

    return Bytes(size.get());
  }

  Try<Nothing> rm(std::string path)
  {
    // Make sure 'to' starts with a '/'.
    path = path::join("", path);

    Try<std::string> command = strings::format(
        "%s fs -rm '%s'", hadoop, path);

    CHECK_SOME(command);

    std::ostringstream output;

    Try<int> status = os::shell(&output, command.get() + " 2>&1");

    if (status.isError()) {
      return Error(status.error());
    } else if (status.get() != 0) {
      return Error(command.get() + "\n" + output.str());
    }

    return Nothing();
  }

  Try<Nothing> copyFromLocal(
      const std::string& from,
      std::string to)
  {
    if (!os::exists(from)) {
      return Error("Failed to find " + from);
    }

    // Make sure 'to' starts with a '/'.
    to = path::join("", to);

    // Copy to HDFS.
    Try<std::string> command = strings::format(
        "%s fs -copyFromLocal '%s' '%s'", hadoop, from, to);

    CHECK_SOME(command);

    std::ostringstream output;

    Try<int> status = os::shell(&output, command.get() + " 2>&1");

    if (status.isError()) {
      return Error(status.error());
    } else if (status.get() != 0) {
      return Error(command.get() + "\n" + output.str());
    }

    return Nothing();
  }

  Try<Nothing> copyToLocal(
      const std::string& from,
      const std::string& to)
  {
    // Copy from HDFS.
    Try<std::string> command = strings::format(
        "%s fs -copyToLocal '%s' '%s'", hadoop, from, to);

    CHECK_SOME(command);

    std::ostringstream output;

    Try<int> status = os::shell(&output, command.get() + " 2>&1");

    if (status.isError()) {
      return Error(status.error());
    } else if (status.get() != 0) {
      return Error(command.get() + "\n" + output.str());
    }

    return Nothing();
  }

private:
  const std::string hadoop;
};

#endif // __HDFS_HPP__
