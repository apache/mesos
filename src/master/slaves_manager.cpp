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

#include <glog/logging.h>

#include <map>
#include <sstream>

#include <boost/lexical_cast.hpp>

#include <process/dispatch.hpp>

#include "config/config.hpp"

#include "common/fatal.hpp"
#include "common/tokenize.hpp"
#ifdef WITH_ZOOKEEPER
#include "common/zookeeper.hpp"
#endif

#include "master.hpp"
#include "slaves_manager.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using boost::bad_lexical_cast;
using boost::lexical_cast;

using process::HttpInternalServerErrorResponse;
using process::HttpNotFoundResponse;
using process::HttpOKResponse;
using process::HttpResponse;
using process::HttpRequest;
using process::PID;
using process::Process;
using process::Promise;
using process::UPID;

using std::ostringstream;
using std::map;
using std::string;
using std::vector;


#ifdef WITH_ZOOKEEPER

// Forward declaration of watcher.
class ZooKeeperSlavesManagerStorageWatcher;


class ZooKeeperSlavesManagerStorage : public SlavesManagerStorage
{
public:
  ZooKeeperSlavesManagerStorage(const string& _servers,
                               const string& _znode,
                               const PID<SlavesManager>& _slavesManager);

  virtual ~ZooKeeperSlavesManagerStorage();

  virtual Promise<bool> add(const string& hostname, uint16_t port);
  virtual Promise<bool> remove(const string& hostname, uint16_t port);
  virtual Promise<bool> activate(const string& hostname, uint16_t port);
  virtual Promise<bool> deactivate(const string& hostname, uint16_t port);

  Promise<bool> connected();
  Promise<bool> reconnecting();
  Promise<bool> reconnected();
  Promise<bool> expired();
  Promise<bool> updated(const string& path);

private:
  bool parse(const string& key, const string& s, multimap<string, uint16_t>* result);

  const string servers;
  const string znode;
  const PID<SlavesManager> slavesManager;
  ZooKeeper* zk;
  ZooKeeperSlavesManagerStorageWatcher* watcher;
};


class ZooKeeperSlavesManagerStorageWatcher : public Watcher
{
public:
  ZooKeeperSlavesManagerStorageWatcher(const PID<ZooKeeperSlavesManagerStorage>& _pid)
    : pid(_pid), reconnect(false) {}

  virtual ~ZooKeeperSlavesManagerStorageWatcher() {}

  virtual void process(ZooKeeper* zk, int type, int state, const string& path)
  {
    if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
      // Check if this is a reconnect.
      if (!reconnect) {
        // Initial connect.
        process::dispatch(pid, &ZooKeeperSlavesManagerStorage::connected);
      } else {
        // Reconnected.
        process::dispatch(pid, &ZooKeeperSlavesManagerStorage::reconnected);
      }
    } else if ((state == ZOO_CONNECTING_STATE) && (type == ZOO_SESSION_EVENT)) {
      // The client library automatically reconnects, taking into
      // account failed servers in the connection string,
      // appropriately handling the "herd effect", etc.
      reconnect = true;
      process::dispatch(pid, &ZooKeeperSlavesManagerStorage::reconnecting);
    } else if ((state == ZOO_EXPIRED_SESSION_STATE) && (type == ZOO_SESSION_EVENT)) {
      // Session expiration. Let the manager take care of it.
      process::dispatch(pid, &ZooKeeperSlavesManagerStorage::expired);

      // If this watcher is reused, the next connect won't be a reconnect.
      reconnect = false;
    } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHANGED_EVENT)) {
      // Let the manager deal with file changes.
      process::dispatch(pid, &ZooKeeperSlavesManagerStorage::updated, path);
    } else {
      LOG(WARNING) << "Unimplemented watch event: (state is "
                   << state << " and type is " << type << ")";
    }
  }

private:
  const PID<ZooKeeperSlavesManagerStorage> pid;
  bool reconnect;
};


ZooKeeperSlavesManagerStorage::ZooKeeperSlavesManagerStorage(const string& _servers,
                                                             const string& _znode,
                                                             const PID<SlavesManager>& _slavesManager)
  : servers(_servers), znode(_znode), slavesManager(_slavesManager)
{
  PID<ZooKeeperSlavesManagerStorage> pid(*this);
  watcher = new ZooKeeperSlavesManagerStorageWatcher(pid);
  zk = new ZooKeeper(servers, 10000, watcher);
}


ZooKeeperSlavesManagerStorage::~ZooKeeperSlavesManagerStorage()
{
  delete zk;
  delete watcher;
}


Promise<bool> ZooKeeperSlavesManagerStorage::add(const string& hostname, uint16_t port)
{
  // TODO(benh): Use ZooKeeperSlavesManagerStorage::parse to clean up code.
  int ret;
  string result;
  Stat stat;

  ret = zk->get(znode, true, &result, &stat);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage failed to get '" << znode
                 << "' in ZooKeeper! (" << zk->error(ret) << ")";
    return false;
  }

  ostringstream out;
  out << hostname << ":" << port;

  if (result.size() == 0) {
    out << "active=" << hostname << ":" << port << "\n";
    out << "inactive=\n";
    result = out.str();
  } else {
    const string active("active=");

    size_t index = result.find(active);

    if (index == string::npos) {
      LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                   << "', could not find 'active='";
      return false;
    }

    if (result[index + active.size()] != '\n') {
      out << ",";
    }

    result.insert(index + active.size(), out.str());
  }

  // Set the data in the znode.
  ret = zk->set(znode, result, stat.version);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage could not add slave "
		 << hostname << ":" << port
                 << " to '" << znode << "' in ZooKeeper! ("
                 << zk->error(ret) << ")";
    return false;
  }

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::remove(const string& hostname, uint16_t port)
{
  // TODO(benh): Use ZooKeeperSlavesManagerStorage::parse to clean up code.
  int ret;
  string result;
  Stat stat;

  ret = zk->get(znode, true, &result, &stat);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage failed to get '" << znode
                 << "' in ZooKeeper! (" << zk->error(ret) << ")";
    return false;
  }

  ostringstream out;
  out << hostname << ":" << port;

  size_t index = result.find(out.str());

  if (index == string::npos) {
    LOG(WARNING) << "Slaves manager storage could not remove slave "
		 << hostname << ":" << port
                 << " because not currently active or inactive";
    return false;
  } else if (index == 0) {
    LOG(WARNING) << "Bad data in '" << znode;
    return false;
  }

  if (result[index - 1] == '=') {
    if (result[index + out.str().size()] == '\n') {
      result.erase(index, out.str().size());
    } else {
      result.erase(index, out.str().size() + 1);
    }
  } else {
    result.erase(index - 1, out.str().size() + 1);
  }

  // Set the data in the znode.
  ret = zk->set(znode, result, stat.version);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage could not remove slave "
		 << hostname << ":" << port
                 << " from '" << znode << "' in ZooKeeper! ("
                 << zk->error(ret) << ")";
    return false;
  }

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::activate(const string& hostname, uint16_t port)
{
  // TODO(benh): Use ZooKeeperSlavesManagerStorage::parse to clean up code.
  int ret;
  string result;
  Stat stat;

  ret = zk->get(znode, true, &result, &stat);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage failed to get '" << znode
                 << "' in ZooKeeper! (" << zk->error(ret) << ")";
    return false;
  }

  ostringstream out;
  out << hostname << ":" << port;

  const string inactive("inactive=");

  size_t index = result.find(inactive);

  if (index == string::npos) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                 << "', could not find 'inactive='";
    return false;
  }

  index = result.find(out.str(), index);

  if (index == string::npos) {
    LOG(WARNING) << "Slaves manager storage could not activate slave "
		 << hostname << ":" << port
                 << " because not currently inactive";
    return false;
  } else if (index == 0) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode;
    return false;
  }

  if (result[index - 1] == '=') {
    if (result[index + out.str().size()] == '\n') {
      result.erase(index, out.str().size());
    } else {
      result.erase(index, out.str().size() + 1);
    }
  } else {
    result.erase(index - 1, out.str().size() + 1);
  }

  const string active("active=");

  index = result.find(active);

  if (index == string::npos) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                 << "', could not find 'active='";
    return false;
  }

  if (result[index + active.size()] != '\n') {
    out << ",";
  }

  result.insert(index + active.size(), out.str());

  // Set the data in the znode.
  ret = zk->set(znode, result, stat.version);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage could not activate slave "
		 << hostname << ":" << port
                 << " in '" << znode << "' in ZooKeeper! ("
                 << zk->error(ret) << ")";
    return false;
  }

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::deactivate(const string& hostname, uint16_t port)
{
  // TODO(benh): Use ZooKeeperSlavesManagerStorage::parse to clean up code.
  int ret;
  string result;
  Stat stat;

  ret = zk->get(znode, true, &result, &stat);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage failed to get '" << znode
                 << "' in ZooKeeper! (" << zk->error(ret) << ")";
    return false;
  }

  ostringstream out;
  out << hostname << ":" << port;

  const string active("active=");

  size_t index = result.find(active);

  if (index == string::npos) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                 << "', could not find 'active='";
    return false;
  }

  index = result.find(out.str(), index);

  if (index == string::npos) {
    LOG(WARNING) << "Slaves manager storage could not deactivate slave "
		 << hostname << ":" << port
                 << " because not currently active";
    return false;
  } else if (index == 0) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode;
    return false;
  }

  if (result[index - 1] == '=') {
    if (result[index + out.str().size()] == '\n') {
      result.erase(index, out.str().size());
    } else {
      result.erase(index, out.str().size() + 1);
    }
  } else {
    result.erase(index - 1, out.str().size() + 1);
  }

  const string inactive("inactive=");

  index = result.find(inactive);

  if (index == string::npos) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                 << "', could not find 'inactive='";
    return false;
  }

  if (result[index + inactive.size()] != '\n') {
    out << ",";
  }

  result.insert(index + inactive.size(), out.str());

  // Set the data in the znode.
  ret = zk->set(znode, result, stat.version);

  if (ret != ZOK) {
    LOG(WARNING) << "Slaves manager storage could not activate slave "
		 << hostname << ":" << port
                 << " in '" << znode << "' in ZooKeeper! ("
                 << zk->error(ret) << ")";
    return false;
  }

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::connected()
{
  int ret;

  static const string delimiter = "/";

  // Assume the znode that was created does not end with a "/".
  CHECK(znode.at(znode.length() - 1) != '/');

  // Create directory path znodes as necessary.
  size_t index = znode.find(delimiter, 0);

  while (index < string::npos) {
    // Get out the prefix to create.
    index = znode.find(delimiter, index + 1);
    string prefix = znode.substr(0, index);

    // Create the node (even if it already exists).
    ret = zk->create(prefix, "", ZOO_OPEN_ACL_UNSAFE, 0, NULL);

    if (ret != ZOK && ret != ZNODEEXISTS) {
      // Okay, consider this a failure (maybe we lost our connection
      // to ZooKeeper), increment the failure count, log the issue,
      // and perhaps try again when ZooKeeper issues get sorted out.
      LOG(WARNING) << "Slaves manager storage failed to create '" << znode
                   << "' in ZooKeeper! (" << zk->error(ret) << ")";
      return false;
    }
  }

  // Reconcile what's in the znodes versus what we have in memory
  // (this also puts watches on these znodes).
  return updated(znode);
}


Promise<bool> ZooKeeperSlavesManagerStorage::reconnecting()
{
  LOG(INFO) << "Slaves manager storage lost connection to ZooKeeper, "
	    << "attempting to reconnect ...";
  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::reconnected()
{
  LOG(INFO) << "Slaves manager storage has reconnected ...";

  // Reconcile what's in the znodes versus what we have in memory
  // (this also puts watches on these znodes).
  return updated(znode);
}


Promise<bool> ZooKeeperSlavesManagerStorage::expired()
{
  LOG(WARNING) << "Slaves manager storage session expired!";

  CHECK(zk != NULL);
  delete zk;

  zk = new ZooKeeper(servers, 10000, watcher);

  // TODO(benh): Put mechanisms in place such that reconnects may
  // fail (or just take too long).

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::updated(const string& path)
{
  int ret;
  string result;

  if (path == znode) {
    LOG(INFO) << "Slaves manager storage found updates in ZooKeeper "
              << "... propogating changes";

    ret = zk->get(znode, true, &result, NULL);

    if (ret != ZOK) {
      LOG(WARNING) << "Slaves manager storage failed to get '" << znode
                   << "' in ZooKeeper! (" << zk->error(ret) << ")";
      return false;
    }

    // Parse what's in ZooKeeper into active/inactive hostname port pairs.
    multimap<string, uint16_t> active;
    if (parse("active=", result, &active)) {
      process::dispatch(slavesManager, &SlavesManager::updateActive, active);
    }

    multimap<string, uint16_t> inactive;
    if (parse("inactive=", result, &inactive)) {
      process::dispatch(slavesManager, &SlavesManager::updateInactive, inactive);
    }
  } else {
    LOG(WARNING) << "Slaves manager stoage not expecting changes to path '"
                 << path << "' in ZooKeeper";
    return false;
  }

  return true;
}


bool ZooKeeperSlavesManagerStorage::parse(const string& key, const string& s,
                                          multimap<string, uint16_t>* result)
{
  size_t begin = s.find(key);
  if (begin == string::npos) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                 << "', could not find '" << key << "'";
    return false;
  }

  size_t end = s.find("\n", begin);
  if (end == string::npos) {
    LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                 << "', missing LF after '" << key << "'";
    return false;
  }

  CHECK(end > begin);

  size_t length = end - begin - key.size();

  const string& temp = s.substr(begin + key.size(), length);

  const vector<string>& tokens = tokenize::split(temp, ",");
  foreach (const string& token, tokens) {
    const vector<string>& pairs = tokenize::split(token, ":");
    if (pairs.size() != 2) {
      LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                   << "', could not parse " << token;
      return false;
    }

    try {
      result->insert(pairs[0], lexical_cast<uint16_t>(pairs[1]));
    } catch (const bad_lexical_cast&) {
      LOG(WARNING) << "Slaves manager storage found bad data in '" << znode
                   << "', could not parse " << token;
      return false;
    }
  }

  return true;
}

#endif // WITH_ZOOKEEPER


SlavesManager::SlavesManager(const Configuration& conf,
                             const PID<Master>& _master)
  : process::ProcessBase("slaves"),
    master(_master)
{
  // Create the slave manager storage based on configuration.
  const string& slaves = conf.get<string>("slaves", "*");

  // Check if 'slaves' starts with "zoo://".
  string zoo = "zoo://";
  size_t index = slaves.find(zoo);
  if (index == 0) {
#ifdef WITH_ZOOKEEPER
    // TODO(benh): Consider actually using the chroot feature of
    // ZooKeeper, rather than just using it's syntax.
    string temp = slaves.substr(zoo.size());
    index = temp.find("/");
    if (index == string::npos) {
      fatal("Expecting chroot path for ZooKeeper");
    }

    const string& servers = temp.substr(0, index);

    const string& znode = temp.substr(index);
    if (znode == "/") {
      fatal("Expecting chroot path for ZooKeeper ('/' is not supported)");
    }

    storage = new ZooKeeperSlavesManagerStorage(servers, znode, self());
    process::spawn(storage);
#else
    fatal("Cannot get active/inactive slave information using 'zoo://',"
          " ZooKeeper is not supported in this build");
#endif // WITH_ZOOKEEPER
  } else {
    // Parse 'slaves' as initial active hostname:port pairs.
    if (slaves != "*") {
      const vector<string>& tokens = tokenize::split(slaves, ",");
      foreach (const string& token, tokens) {
        const vector<string>& pairs = tokenize::split(token, ":");
        if (pairs.size() != 2) {
          fatal("Failed to parse \"%s\" in option 'slaves'", token.c_str());
        }

        try {
          active[pairs[0]].insert(lexical_cast<uint16_t>(pairs[1]));
        } catch (const bad_lexical_cast&) {
          fatal("Failed to parse \"%s\" in option 'slaves'", token.c_str());
        }
      }
    }

    storage = new SlavesManagerStorage();
    process::spawn(storage);
  }

  // Set up our HTTP endpoints.
  installHttpHandler("add", &SlavesManager::add);
  installHttpHandler("remove", &SlavesManager::remove);
  installHttpHandler("activate", &SlavesManager::activate);
  installHttpHandler("deactivate", &SlavesManager::deactivate);
  installHttpHandler("activated", &SlavesManager::activated);
  installHttpHandler("deactivated", &SlavesManager::deactivated);
}


SlavesManager::~SlavesManager()
{
  process::post(storage->self(), process::TERMINATE);
  process::wait(storage->self());
  delete storage;
}


void SlavesManager::registerOptions(Configurator* configurator)
{
  configurator->addOption<string>("slaves",
                                  "Initial slaves that should be "
                                  "considered part of this cluster "
                                  "(or if using ZooKeeper a URL)", "*");
}


bool SlavesManager::add(const string& hostname, uint16_t port)
{
  // Ignore request if slave is already active.
  if (active.count(hostname, port) > 0) {
    LOG(WARNING) << "Attempted to add an already added slave!";
    return true;
  }

  // Make sure this slave is not currently deactivated.
  if (inactive.count(hostname, port) > 0) {
    LOG(WARNING) << "Attempted to add a deactivated slave, "
                 << "try activating it instead!";
    return false;
  }

  // Ask the storage system to persist the addition.
  if (process::call(storage->self(), &SlavesManagerStorage::add,
                    hostname, port)) {
    active.insert(hostname, port);

    // Tell the master that this slave is now active.
    process::dispatch(master, &Master::activatedSlaveHostnamePort,
                      hostname, port);

    return true;
  }

  return false;
}


bool SlavesManager::remove(const string& hostname, uint16_t port)
{
  // Make sure the slave is currently activated or deactivated.
  if (active.count(hostname, port) == 0 &&
      inactive.count(hostname, port) == 0) {
    LOG(WARNING) << "Attempted to remove unknown slave!";
    return false;
  }

  // Get the storage system to persist the removal.
  if (process::call(storage->self(), &SlavesManagerStorage::remove,
                    hostname, port)) {
    active.erase(hostname, port);
    inactive.erase(hostname, port);

    // Tell the master that this slave is now deactivated.
    process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                      hostname, port);

    return true;
  }

  return false;
}


bool SlavesManager::activate(const string& hostname, uint16_t port)
{
  // Make sure the slave is currently deactivated.
  if (inactive.count(hostname, port) > 0) {
    // Get the storage system to persist the activation.
    if (process::call(storage->self(), &SlavesManagerStorage::activate,
                      hostname, port)) {
      active.insert(hostname, port);
      inactive.erase(hostname, port);

      // Tell the master that this slave is now activated.
      process::dispatch(master, &Master::activatedSlaveHostnamePort,
                        hostname, port);

      return true;
    }
  }

  return false;
}


bool SlavesManager::deactivate(const string& hostname, uint16_t port)
{
  // Make sure the slave is currently activated.
  if (active.count(hostname, port) > 0) {
    // Get the storage system to persist the deactivation.
    if (process::call(storage->self(), &SlavesManagerStorage::deactivate,
                      hostname, port)) {
      active.erase(hostname, port);
      inactive.insert(hostname, port);

      // Tell the master that this slave is now deactivated.
      process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                        hostname, port);

      return true;
    }
  }

  return false;
}


void SlavesManager::updateActive(const multimap<string, uint16_t>& updated)
{
  // Loop through the current active slave hostname:port pairs and
  // remove all that are not found in updated.
  foreachpair (const string& hostname, uint16_t port, utils::copy(active)) {
    if (updated.count(hostname, port) == 0) {
      process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                        hostname, port);
      active.erase(hostname, port);
    }
  }

  // Now loop through the updated slave hostname:port pairs and add
  // all that are not found in active.
  foreachpair (const string& hostname, uint16_t port, updated) {
    if (active.count(hostname, port) == 0) {
      process::dispatch(master, &Master::activatedSlaveHostnamePort,
                        hostname, port);
      active.insert(hostname, port);
    }
  }
}


void SlavesManager::updateInactive(const multimap<string, uint16_t>& updated)
{
  inactive = updated;
}


Promise<HttpResponse> SlavesManager::add(const HttpRequest& request)
{
  // Parse the query to get out the slave hostname and port.
  string hostname = "";
  uint16_t port = 0;

  map<string, vector<string> > pairs =
    tokenize::pairs(request.query, ',', '=');

  // Make sure there is at least a 'hostname=' and 'port='.
  if (pairs.count("hostname") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'hostname' in query string"
                 << " when trying to add a slave";
    return HttpNotFoundResponse();
  } else if (pairs.count("port") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'port' in query string"
                 << " when trying to add a slave";
    return HttpNotFoundResponse();
  }

  hostname = pairs["hostname"].front();

  // Check that 'port' is valid.
  try {
    port = lexical_cast<uint16_t>(pairs["port"].front());
  } catch (const bad_lexical_cast&) {
    LOG(WARNING) << "Slaves manager failed to parse 'port = "
		 << pairs["port"].front()
                 << "'  when trying to add a slave";
    return HttpNotFoundResponse();
  }

  LOG(INFO) << "Slaves manager received HTTP request to add slave at "
	    << hostname << ":" << port;

  if (add(hostname, port)) {
    return HttpOKResponse();
  } else {
    return HttpInternalServerErrorResponse();
  }
}


Promise<HttpResponse> SlavesManager::remove(const HttpRequest& request)
{
  // Parse the query to get out the slave hostname and port.
  string hostname = "";
  uint16_t port = 0;

  map<string, vector<string> > pairs =
    tokenize::pairs(request.query, ',', '=');

  // Make sure there is at least a 'hostname=' and 'port='.
  if (pairs.count("hostname") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'hostname' in query string"
                 << " when trying to remove a slave";
    return HttpNotFoundResponse();
  } else if (pairs.count("port") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'port' in query string"
                 << " when trying to remove a slave";
    return HttpNotFoundResponse();
  }

  hostname = pairs["hostname"].front();

  // Check that 'port' is valid.
  try {
    port = lexical_cast<uint16_t>(pairs["port"].front());
  } catch (const bad_lexical_cast&) {
    LOG(WARNING) << "Slaves manager failed to parse 'port = "
		 << pairs["port"].front()
                 << "'  when trying to remove a slave";
    return HttpNotFoundResponse();
  }

  LOG(INFO) << "Slaves manager received HTTP request to remove slave at "
	    << hostname << ":" << port;

  if (remove(hostname, port)) {
    return HttpOKResponse();
  } else {
    return HttpInternalServerErrorResponse();
  }
}


Promise<HttpResponse> SlavesManager::activate(const HttpRequest& request)
{
  // Parse the query to get out the slave hostname and port.
  string hostname = "";
  uint16_t port = 0;

  map<string, vector<string> > pairs =
    tokenize::pairs(request.query, ',', '=');

  // Make sure there is at least a 'hostname=' and 'port='.
  if (pairs.count("hostname") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'hostname' in query string"
                 << " when trying to activate a slave";
    return HttpNotFoundResponse();
  } else if (pairs.count("port") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'port' in query string"
                 << " when trying to activate a slave";
    return HttpNotFoundResponse();
  }

  hostname = pairs["hostname"].front();

  // Check that 'port' is valid.
  try {
    port = lexical_cast<uint16_t>(pairs["port"].front());
  } catch (const bad_lexical_cast&) {
    LOG(WARNING) << "Slaves manager failed to parse 'port = "
		 << pairs["port"].front()
                 << "'  when trying to activate a slave";
    return HttpNotFoundResponse();
  }

  LOG(INFO) << "Slaves manager received HTTP request to activate slave at "
	    << hostname << ":" << port;

  if (activate(hostname, port)) {
    return HttpOKResponse();
  } else {
    return HttpInternalServerErrorResponse();
  }
}


Promise<HttpResponse> SlavesManager::deactivate(const HttpRequest& request)
{
  // Parse the query to get out the slave hostname and port.
  string hostname = "";
  uint16_t port = 0;

  map<string, vector<string> > pairs =
    tokenize::pairs(request.query, ',', '=');

  // Make sure there is at least a 'hostname=' and 'port='.
  if (pairs.count("hostname") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'hostname' in query string"
                 << " when trying to deactivate a slave";
    return HttpNotFoundResponse();
  } else if (pairs.count("port") == 0) {
    LOG(WARNING) << "Slaves manager expecting 'port' in query string"
                 << " when trying to deactivate a slave";
    return HttpNotFoundResponse();
  }

  hostname = pairs["hostname"].front();

  // Check that 'port' is valid.
  try {
    port = lexical_cast<uint16_t>(pairs["port"].front());
  } catch (const bad_lexical_cast&) {
    LOG(WARNING) << "Slaves manager failed to parse 'port = "
		 << pairs["port"].front()
                 << "'  when trying to deactivate a slave";
    return HttpNotFoundResponse();
  }

  LOG(INFO) << "Slaves manager received HTTP request to deactivate slave at "
	    << hostname << ":" << port;

  if (deactivate(hostname, port)) {
    return HttpOKResponse();
  } else {
    return HttpInternalServerErrorResponse();
  }
}


Promise<HttpResponse> SlavesManager::activated(const HttpRequest& request)
{
  LOG(INFO) << "Slaves manager received HTTP request for activated slaves";

  ostringstream out;

  foreachpair (const string& hostname, uint16_t port, active) {
    out << hostname << ":" << port << "\n";
  }

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> SlavesManager::deactivated(const HttpRequest& request)
{
  LOG(INFO) << "Slaves manager received HTTP request for deactivated slaves";

  ostringstream out;

  foreachpair (const string& hostname, uint16_t port, inactive) {
    out << hostname << ":" << port << "\n";
  }

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}
