#include <iomanip>

#include <glog/logging.h>

#include <google/protobuf/descriptor.h>

#include "config/config.hpp"

#include "common/date_utils.hpp"
#ifdef WITH_ZOOKEEPER
#include "common/zookeeper.hpp"
#endif

#include "allocator.hpp"
#include "allocator_factory.hpp"
#include "master.hpp"
#include "webui.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using boost::bad_lexical_cast;
using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

using process::HttpInternalServerErrorResponse;
using process::HttpNotFoundResponse;
using process::HttpOKResponse;
using process::HttpResponse;
using process::HttpRequest;
using process::PID;
using process::Process;
using process::Promise;
using process::UPID;

using std::endl;
using std::map;
using std::max;
using std::ostringstream;
using std::pair;
using std::set;
using std::setfill;
using std::setw;
using std::string;
using std::vector;


namespace {

// A process that periodically pings the master to check filter
// expiries, etc.
class AllocatorTimer : public Process<AllocatorTimer>
{
public:
  AllocatorTimer(const PID<Master>& _master) : master(_master) {}

protected:
  virtual void operator () ()
  {
    link(master);
    while (true) {
      receive(1);
      if (name() == process::TIMEOUT) {
        process::dispatch(master, &Master::timerTick);
      } else if (name() == process::EXITED) {
	return;
      }
    }
  }

private:
  const PID<Master> master;
};


// A process that periodically prints frameworks' shares to a file
class SharesPrinter : public Process<SharesPrinter>
{
public:
  SharesPrinter(const PID<Master>& _master) : master(_master) {}
  ~SharesPrinter() {}

protected:
  virtual void operator () ()
  {
    int tick = 0;

    std::ofstream file ("/mnt/shares");
    if (!file.is_open()) {
      LOG(FATAL) << "Could not open /mnt/shares";
    }

    while (true) {
      pause(1);

      state::MasterState* state = call(master, &Master::getState);

      uint32_t total_cpus = 0;
      uint32_t total_mem = 0;

      foreach (state::Slave* s, state->slaves) {
        total_cpus += s->cpus;
        total_mem += s->mem;
      }
      
      if (state->frameworks.empty()) {
        file << "--------------------------------" << endl;
      } else {
        foreach (state::Framework* f, state->frameworks) {
          double cpu_share = f->cpus / (double) total_cpus;
          double mem_share = f->mem / (double) total_mem;
          double max_share = max(cpu_share, mem_share);
          file << tick << "#" << f->id << "#" << f->name << "#" 
               << f->cpus << "#" << f->mem << "#"
               << cpu_share << "#" << mem_share << "#" << max_share << endl;
        }
      }
      delete state;
      tick++;
    }
    file.close();
  }

private:
  const PID<Master> master;
};

} // namespace {


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
    } else if ((state == ZOO_EXPIRED_SESSION_STATE) && (type == ZOO_SESSION_EVENT)) {
      // Session expiration. Let the manager take care of it.
      process::dispatch(pid, &ZooKeeperSlavesManagerStorage::expired);

      // If this watcher is reused, the next connect won't be a reconnect.
      reconnect = false;
    } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHANGED_EVENT)) {
      // Let the manager deal with file changes.
      process::dispatch(pid, &ZooKeeperSlavesManagerStorage::updated, path);
    } else if ((state == ZOO_CONNECTING_STATE) && (type == ZOO_SESSION_EVENT)) {
      // The client library automatically reconnects, taking into
      // account failed servers in the connection string,
      // appropriately handling the "herd effect", etc.
      reconnect = true;
      process::dispatch(pid, &ZooKeeperSlavesManagerStorage::reconnecting);
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
  int ret;
  string result;
  Stat stat;

  ret = zk->get(znode + "/active", true, &result, &stat);

  if (ret != ZOK) {
    LOG(WARNING) << "Failed to get '" << znode + "/active"
                 << "' in ZooKeeper! (" << zk->error(ret) << ")";
    return false;
  }

  ostringstream out;

  if (result.size() == 0) {
    out << hostname << ":" << port;
  } else {
    out << "," << hostname << ":" << port;
  }

  result += out.str();

  // Set the data in the znode.
  ret = zk->set(znode + "/active", result, stat.version);

  if (ret != ZOK) {
    LOG(WARNING) << "Could not add slave " << hostname << ":" << port
                 << " to '" << znode + "/active' in ZooKeeper! ("
                 << zk->error(ret) << ")";
    return false;
  }

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::remove(const string& hostname, uint16_t port)
{
  string files[] = { "/active", "/inactive" };

  foreach (const string& file, files) {
    int ret;
    string result;
    Stat stat;

    ret = zk->get(znode + file, true, &result, &stat);

    if (ret != ZOK) {
      LOG(WARNING) << "Failed to get '" << znode + file
                   << "' in ZooKeeper! (" << zk->error(ret) << ")";
      return false;
    }

    ostringstream out;
    out << hostname << ":" << port;

    size_t index = result.find(out.str());

    if (index != string::npos) {
      if (index == 0) {
        result.erase(index, out.str().size() + 1);
      } else {
        result.erase(index - 1, out.str().size() + 1);
      }

      // Set the data in the znode.
      ret = zk->set(znode + file, result, stat.version);

      if (ret != ZOK) {
        LOG(WARNING) << "Could not remove slave " << hostname << ":" << port
                     << " to '" << znode + file << "' in ZooKeeper! ("
                     << zk->error(ret) << ")";
        return false;
      }
    }
  }

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::activate(const string& hostname, uint16_t port)
{
  fatal("unimplemented");
  return false;
}


Promise<bool> ZooKeeperSlavesManagerStorage::deactivate(const string& hostname, uint16_t port)
{
  fatal("unimplemented");
  return false;
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
      LOG(WARNING) << "Failed to create '" << znode
                   << "' in ZooKeeper! (" << zk->error(ret) << ")";
      return false;
    }
  }

  // Now make sure the 'active' znode is created.
  ret = zk->create(znode + "/active", "", ZOO_OPEN_ACL_UNSAFE, 0, NULL);

  if (ret != ZOK && ret != ZNODEEXISTS) {
    LOG(WARNING) << "Failed to create '" << znode + "/active"
                 << "' in ZooKeeper! (" << zk->error(ret) << ")";
    return false;
  }

  // Now make sure the 'inactive' znode is created.
  ret = zk->create(znode + "/inactive", "", ZOO_OPEN_ACL_UNSAFE, 0, NULL);

  if (ret != ZOK && ret != ZNODEEXISTS) {
    LOG(WARNING) << "Failed to create '" << znode + "/inactive"
                 << "' in ZooKeeper! (" << zk->error(ret) << ")";
    return false;
  }

  // Reconcile what's in the znodes versus what we have in memory
  // (this also puts watches on these znodes).
  updated(znode + "/active");
  updated(znode + "/inactive");

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::reconnecting()
{
  LOG(INFO) << "ZooKeeperSlavesManagerStorage is attempting to reconnect";
  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::reconnected()
{
  LOG(INFO) << "ZooKeeperSlavesManagerStorage has reconnected";

  // Reconcile what's in the znodes versus what we have in memory
  // (this also puts watches on these znodes).
  updated(znode + "/active");
  updated(znode + "/inactive");

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::expired()
{
  LOG(WARNING) << "ZooKeeperSlavesManagerStorage session expired!";

  CHECK(zk != NULL);
  delete zk;

  zk = new ZooKeeper(servers, 10000, watcher);

  // TODO(benh): Put mechanisms in place such that reconnects may
  // fail (or just take too long).

  return true;
}


Promise<bool> ZooKeeperSlavesManagerStorage::updated(const string& path)
{
  LOG(INFO) << "Slave information at '" << path
            << "' in ZooKeeper has been updated ... propogating changes";

  int ret;
  string result;

  if (path == znode + "/active") {
    ret = zk->get(znode + "/active", true, &result, NULL);

    if (ret != ZOK) {
      LOG(WARNING) << "Failed to get '" << znode + "/active"
                   << "' in ZooKeeper! (" << zk->error(ret) << ")";
      return false;
    }

    // Parse what's in ZooKeeper into hostname port pairs.
    map<string, set<uint16_t> > active;

    const vector<string>& tokens = tokenize::tokenize(result, ",");
    foreach (const string& token, tokens) {
      const vector<string>& pairs = tokenize::tokenize(token, ":");
      if (pairs.size() != 2) {
        LOG(WARNING) << "Bad data in '" << znode + "/active"
                     << "', could not parse " << token;
        return false;
      }

      try {
        active[pairs[0]].insert(lexical_cast<uint16_t>(pairs[1]));
      } catch (const bad_lexical_cast&) {
        LOG(WARNING) << "Bad data in '" << znode + "/active"
                     << "', could not parse " << token;
        return false;
      }
    }

    process::dispatch(slavesManager, &SlavesManager::updateActive, active);
  } else if (path == znode + "/inactive") {
    ret = zk->get(znode + "/inactive", true, &result, NULL);

    if (ret != ZOK) {
      LOG(WARNING) << "Failed to get '" << znode + "/inactive"
                   << "' in ZooKeeper! (" << zk->error(ret) << ")";
      return false;
    }

    // Parse what's in ZooKeeper into hostname port pairs.
    map<string, set<uint16_t> > inactive;

    const vector<string>& tokens = tokenize::tokenize(result, ",");
    foreach (const string& token, tokens) {
      const vector<string>& pairs = tokenize::tokenize(token, ":");
      if (pairs.size() != 2) {
        LOG(WARNING) << "Bad data in '" << znode + "/inactive"
                     << "', could not parse " << token;
        return false;
      }

      try {
        inactive[pairs[0]].insert(lexical_cast<uint16_t>(pairs[1]));
      } catch (const bad_lexical_cast&) {
        LOG(WARNING) << "Bad data in '" << znode + "/inactive"
                     << "', could not parse " << token;
        return false;
      }
    }

    process::dispatch(slavesManager, &SlavesManager::updateInactive, inactive);
  } else {
    LOG(WARNING) << "Not expecting changes to path '"
                 << path << "' in ZooKeeper";
    return false;
  }

  return true;
}

#endif // WITH_ZOOKEEPER


SlavesManager::SlavesManager(const Configuration& conf,
                             const PID<Master>& _master)
  : process::Process<SlavesManager>("slaves"),
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
    // Parse 'slaves' as initial hostname:port pairs.
    if (slaves != "*") {
      const vector<string>& tokens = tokenize::tokenize(slaves, ",");
      foreach (const string& token, tokens) {
        const vector<string>& pairs = tokenize::tokenize(token, ":");
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
  install("add", &SlavesManager::add);
  install("remove", &SlavesManager::remove);
}


SlavesManager::~SlavesManager()
{
  // TODO(benh): Terminate and deallocate 'storage'.
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
  // Make sure this slave is not currently deactivated or activated.
  if (inactive.count(hostname) > 0 && inactive[hostname].count(port) > 0) {
    LOG(WARNING) << "Attempted to add deactivated slave at "
                 << hostname << ":" << port;
    return false;
  } else if (active.count(hostname) == 0 || active[hostname].count(port) == 0) {
    // Get the storage system to persist the addition.
    bool result = process::call(storage->self(), &SlavesManagerStorage::add,
                                hostname, port);
    if (result) {
      LOG(INFO) << "Adding slave at " << hostname << ":" << port;
      // Tell the master that this slave is now active (so it can
      // allow the slave to register).
      process::dispatch(master, &Master::activatedSlaveHostnamePort,
                        hostname, port);
      active[hostname].insert(port);
      return true;
    }
  }

  return false;
}


bool SlavesManager::remove(const string& hostname, uint16_t port)
{
  // Make sure the slave is currently activated or deactivated.
  if ((active.count(hostname) > 0 && active[hostname].count(port) > 0) ||
      (inactive.count(hostname) > 0 && inactive[hostname].count(port) > 0)) {
    // Get the storage system to persist the removal.
    bool result = process::call(storage->self(), &SlavesManagerStorage::remove,
                                hostname, port);
    if (result) {
      LOG(INFO) << "Removing slave at " << hostname << ":" << port;
      if (active.count(hostname) > 0 && active[hostname].count(port) > 0) {
        process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                          hostname, port);
        active[hostname].erase(port);
        if (active[hostname].size() == 0) active.erase(hostname);
      }

      if (inactive.count(hostname) > 0 && inactive[hostname].count(port) > 0) {
        inactive[hostname].erase(port);
        if (inactive[hostname].size() == 0) inactive.erase(hostname);
      }

      return true;
    }
  }

  return false;
}


bool SlavesManager::activate(const string& hostname, uint16_t port)
{
  // Make sure the slave is currently deactivated.
  if (inactive.count(hostname) > 0 && inactive[hostname].count(port) > 0) {
    // Get the storage system to persist the activation.
    bool result = process::call(storage->self(), &SlavesManagerStorage::activate,
                                hostname, port);
    if (result) {
      LOG(INFO) << "Activating slave at " << hostname << ":" << port;
      process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                        hostname, port);
      active[hostname].insert(port);
      inactive[hostname].erase(port);
      if (inactive[hostname].size() == 0) inactive.erase(hostname);
      return true;
    }
  }

  return false;
}


bool SlavesManager::deactivate(const string& hostname, uint16_t port)
{
  // Make sure the slave is currently activated.
  if (active.count(hostname) > 0 && active[hostname].count(port) > 0) {
    // Get the storage system to persist the deactivation.
    bool result = process::call(storage->self(), &SlavesManagerStorage::deactivate,
                                hostname, port);
    if (result) {
      LOG(INFO) << "Deactivating slave at " << hostname << ":" << port;
      process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                        hostname, port);
      active[hostname].erase(port);
      if (active[hostname].size() == 0) active.erase(hostname);
      inactive[hostname].insert(port);
      return true;
    }
  }

  return false;
}


void SlavesManager::updateActive(const map<string, set<uint16_t> >& _updated)
{
  // TODO(benh): Remove this unnecessary copy. The code below uses the
  // [] operator to make it easier to read, but [] can't be used on
  // something that is const, hence the copy. Ugh.
  map<string, set<uint16_t> > updated = _updated;

  // Loop through the current active slave hostname:port pairs and
  // remove all that are not found in updated.
  foreachpair (const string& hostname, _, active) {
    if (updated.count(hostname) == 0) {
      foreach (uint16_t port, active[hostname]) {
        LOG(INFO) << "Removing slave at " << hostname << ":" << port;
        process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                          hostname, port);
      }
      active.erase(hostname);
    } else {
      foreach (uint16_t port, active[hostname]) {
        if (updated[hostname].count(port) == 0) {
          LOG(INFO) << "Removing slave at " << hostname << ":" << port;
          process::dispatch(master, &Master::deactivatedSlaveHostnamePort,
                            hostname, port);
          active[hostname].erase(port);
          if (active[hostname].size() == 0) active.erase(hostname);
        }
      }
    }
  }

  // Now loop through the updated slave hostname:port pairs and add
  // all that are not found in active.
  foreachpair (const string& hostname, _, updated) {
    if (active.count(hostname) == 0) {
      foreach (uint16_t port, updated[hostname]) {
        LOG(INFO) << "Adding slave at " << hostname << ":" << port;
        process::dispatch(master, &Master::activatedSlaveHostnamePort,
                          hostname, port);
        active[hostname].insert(port);
      }
    } else {
      foreach (uint16_t port, active[hostname]) {
        if (active[hostname].count(port) == 0) {
          LOG(INFO) << "Adding slave at " << hostname << ":" << port;
          process::dispatch(master, &Master::activatedSlaveHostnamePort,
                            hostname, port);
          active[hostname].insert(port);
        }
      }
    }
  }
}


void SlavesManager::updateInactive(const map<string, set<uint16_t> >& updated)
{
  inactive = updated;
}


Promise<HttpResponse> SlavesManager::add(const HttpRequest& request)
{
  // Parse the query to get out the slave hostname and port.
  string hostname = "";
  uint16_t port = 0;

  map<string, vector<string> > pairs = tokenize::pairs(request.query, ",", "=");

  if (pairs.size() != 2) {
    LOG(WARNING) << "Malformed query string when trying to add a slave";
    return HttpNotFoundResponse();
  }

  // Make sure there is at least a 'hostname=' and 'port='.
  if (pairs.count("hostname") == 0) {
    LOG(WARNING) << "Missing 'hostname' in query string"
                 << " when trying to add a slave";
    return HttpNotFoundResponse();
  } else if (pairs.count("port") == 0) {
    LOG(WARNING) << "Missing 'port' in query string"
                 << " when trying to add a slave";
    return HttpNotFoundResponse();
  }

  hostname = pairs["hostname"].front();

  // Check that 'port' is valid.
  try {
    port = lexical_cast<uint16_t>(pairs["port"].front());
  } catch (const bad_lexical_cast&) {
    LOG(WARNING) << "Failed to parse 'port = " << pairs["port"].front()
                 << "'  when trying to add a slave";
    return HttpNotFoundResponse();
  }

  LOG(INFO) << "Asked to add slave at " << hostname << ":" << port;

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

  // TODO(benh): Don't use tokenize::pairs to get better errors?
  map<string, vector<string> > pairs = tokenize::pairs(request.query, ",", "=");

  if (pairs.size() != 2) {
    LOG(WARNING) << "Malformed query string when trying to remove a slave";
    return HttpNotFoundResponse();
  }

  // Make sure there is at least a 'hostname=' and 'port='.
  if (pairs.count("hostname") == 0) {
    LOG(WARNING) << "Missing 'hostname' in query string"
                 << " when trying to remove a slave";
    return HttpNotFoundResponse();
  } else if (pairs.count("port") == 0) {
    LOG(WARNING) << "Missing 'port' in query string"
                 << " when trying to remove a slave";
    return HttpNotFoundResponse();
  }

  hostname = pairs["hostname"].front();

  // Check that 'port' is valid.
  try {
    port = lexical_cast<uint16_t>(pairs["port"].front());
  } catch (const bad_lexical_cast&) {
    LOG(WARNING) << "Failed to parse 'port = " << pairs["port"].front()
                 << "'  when trying to remove a slave";
    return HttpNotFoundResponse();
  }

  LOG(INFO) << "Asked to remove slave at " << hostname << ":" << port;

  if (remove(hostname, port)) {
    return HttpOKResponse();
  } else {
    return HttpInternalServerErrorResponse();
  }
}


Master::Master()
  : MesosProcess<Master>("master"),
    active(false), nextFrameworkId(0), nextSlaveId(0), nextOfferId(0)
{
  allocatorType = "simple";
  initialize();
}


Master::Master(const Configuration& _conf)
  : MesosProcess<Master>("master"),
    active(false), conf(_conf), nextFrameworkId(0), nextSlaveId(0),
    nextOfferId(0)
{
  allocatorType = conf.get("allocator", "simple");
  initialize();
}
                   

Master::~Master()
{
  LOG(INFO) << "Shutting down master";

  foreachpaircopy (_, Framework* framework, frameworks) {
    removeFramework(framework);
  }

  foreachpaircopy (_, Slave* slave, slaves) {
    removeSlave(slave);
  }

  delete allocator;

  CHECK(slotOffers.size() == 0);

  // TODO(benh): Terminate and delete slave manager!
}


void Master::registerOptions(Configurator* configurator)
{
  SlavesManager::registerOptions(configurator);
  configurator->addOption<string>("allocator", 'a', "Allocation module name",
                                  "simple");
  configurator->addOption<bool>("root_submissions",
                                "Can root submit frameworks?",
                                true);
}


Promise<state::MasterState*> Master::getState()
{
  state::MasterState* state =
    new state::MasterState(build::DATE, build::USER, self());

  foreachpair (_, Slave* s, slaves) {
    Resources resources(s->info.resources());
    Resource::Scalar cpus;
    Resource::Scalar mem;
    cpus.set_value(-1);
    mem.set_value(-1);
    cpus = resources.getScalar("cpus", cpus);
    mem = resources.getScalar("mem", mem);

    state::Slave* slave =
      new state::Slave(s->slaveId.value(), s->info.hostname(),
                       s->info.public_hostname(), cpus.value(),
                       mem.value(), s->connectTime);

    state->slaves.push_back(slave);
  }

  foreachpair (_, Framework* f, frameworks) {
    Resources resources(f->resources);
    Resource::Scalar cpus;
    Resource::Scalar mem;
    cpus.set_value(-1);
    mem.set_value(-1);
    cpus = resources.getScalar("cpus", cpus);
    mem = resources.getScalar("mem", mem);

    state::Framework* framework =
      new state::Framework(f->frameworkId.value(), f->info.user(),
                           f->info.name(), f->info.executor().uri(),
                           cpus.value(), mem.value(), f->connectTime);

    state->frameworks.push_back(framework);

    foreachpair (_, Task* t, f->tasks) {
      Resources resources(t->resources());
      Resource::Scalar cpus;
      Resource::Scalar mem;
      cpus.set_value(-1);
      mem.set_value(-1);
      cpus = resources.getScalar("cpus", cpus);
      mem = resources.getScalar("mem", mem);

      state::Task* task =
        new state::Task(t->task_id().value(), t->name(),
                        t->framework_id().value(), t->slave_id().value(),
                        TaskState_descriptor()->FindValueByNumber(t->state())->name(),
                        cpus.value(), mem.value());

      framework->tasks.push_back(task);
    }

    foreach (SlotOffer* o, f->slotOffers) {
      state::SlotOffer* offer =
        new state::SlotOffer(o->offerId.value(), o->frameworkId.value());

      foreach (const SlaveResources& r, o->resources) {
        Resources resources(r.resources);
        Resource::Scalar cpus;
        Resource::Scalar mem;
        cpus.set_value(-1);
        mem.set_value(-1);
        cpus = resources.getScalar("cpus", cpus);
        mem = resources.getScalar("mem", mem);

        state::SlaveResources* sr =
          new state::SlaveResources(r.slave->slaveId.value(),
                                    cpus.value(), mem.value());

        offer->resources.push_back(sr);
      }

      framework->offers.push_back(offer);
    }
  }
  
  return state;
}


// Return connected frameworks that are not in the process of being removed
vector<Framework*> Master::getActiveFrameworks()
{
  vector <Framework*> result;
  foreachpair(_, Framework* framework, frameworks) {
    if (framework->active) {
      result.push_back(framework);
    }
  }
  return result;
}


// Return connected slaves that are not in the process of being removed
vector<Slave*> Master::getActiveSlaves()
{
  vector <Slave*> result;
  foreachpair(_, Slave* slave, slaves) {
    if (slave->active) {
      result.push_back(slave);
    }
  }
  return result;
}


Framework* Master::lookupFramework(const FrameworkID& frameworkId)
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks[frameworkId];
  } else {
    return NULL;
  }
}


Slave* Master::lookupSlave(const SlaveID& slaveId)
{
  if (slaves.count(slaveId) > 0) {
    return slaves[slaveId];
  } else {
    return NULL;
  }
}


SlotOffer* Master::lookupSlotOffer(const OfferID& offerId)
{
  if (slotOffers.count(offerId) > 0) {
    return slotOffers[offerId];
  } else {
    return NULL;
  }
}


void Master::operator () ()
{
  LOG(INFO) << "Master started at mesos://" << self();

  // Don't do anything until we get a master token.
  while (receive() != GOT_MASTER_TOKEN) {
    LOG(INFO) << "Oops! We're dropping a message since "
              << "we haven't received an identifier yet!";  
  }

  MSG<GOT_MASTER_TOKEN> msg;
  msg.ParseFromString(body());

  // The master ID is comprised of the current date and some ephemeral
  // token (e.g., determined by ZooKeeper).

  masterId = DateUtils::currentDate() + "-" + msg.token();
  LOG(INFO) << "Master ID: " << masterId;

  // Setup slave manager.
  slavesManager = new SlavesManager(conf, self());
  process::spawn(slavesManager);

  // Create the allocator (we do this after the constructor because it
  // leaks 'this').
  allocator = createAllocator();
  if (!allocator) {
    LOG(FATAL) << "Unrecognized allocator type: " << allocatorType;
  }

  link(spawn(new AllocatorTimer(self())));
  //link(spawn(new SharesPrinter(self())));

  while (true) {
    serve();
    if (name() == process::TERMINATE) {
      LOG(INFO) << "Asked to terminate by " << from();
      foreachpair (_, Slave* slave, slaves) {
        send(slave->pid, process::TERMINATE);
      }
      break;
    } else {
      LOG(WARNING) << "Dropping unknown message '" << name() << "'"
                   << " from: " << from();
    }
  }
}


void Master::initialize()
{
  // Install handler functions for certain messages.
  install(NEW_MASTER_DETECTED, &Master::newMasterDetected,
          &NewMasterDetectedMessage::pid);

  install(NO_MASTER_DETECTED, &Master::noMasterDetected);

  install(MASTER_DETECTION_FAILURE, &Master::masterDetectionFailure);

  install(F2M_REGISTER_FRAMEWORK, &Master::registerFramework,
          &RegisterFrameworkMessage::framework);

  install(F2M_REREGISTER_FRAMEWORK, &Master::reregisterFramework,
          &ReregisterFrameworkMessage::framework_id,
          &ReregisterFrameworkMessage::framework,
          &ReregisterFrameworkMessage::generation);

  install(F2M_UNREGISTER_FRAMEWORK, &Master::unregisterFramework,
          &UnregisterFrameworkMessage::framework_id);

  install(F2M_RESOURCE_OFFER_REPLY, &Master::resourceOfferReply,
          &ResourceOfferReplyMessage::framework_id,
          &ResourceOfferReplyMessage::offer_id,
          &ResourceOfferReplyMessage::tasks,
          &ResourceOfferReplyMessage::params);

  install(F2M_REVIVE_OFFERS, &Master::reviveOffers,
          &ReviveOffersMessage::framework_id);

  install(F2M_KILL_TASK, &Master::killTask,
          &KillTaskMessage::framework_id,
          &KillTaskMessage::task_id);

  install(F2M_FRAMEWORK_MESSAGE, &Master::schedulerMessage,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::message);

  install(F2M_STATUS_UPDATE_ACK, &Master::statusUpdateAck,
          &StatusUpdateAckMessage::framework_id,
          &StatusUpdateAckMessage::task_id,
          &StatusUpdateAckMessage::slave_id);

  install(S2M_REGISTER_SLAVE, &Master::registerSlave,
          &RegisterSlaveMessage::slave);

  install(S2M_REREGISTER_SLAVE, &Master::reregisterSlave,
          &ReregisterSlaveMessage::slave_id,
          &ReregisterSlaveMessage::slave,
          &ReregisterSlaveMessage::tasks);

  install(S2M_UNREGISTER_SLAVE, &Master::unregisterSlave,
          &UnregisterSlaveMessage::slave_id);

  install(S2M_STATUS_UPDATE, &Master::statusUpdate,
          &StatusUpdateMessage::framework_id,
          &StatusUpdateMessage::status);

  install(S2M_FRAMEWORK_MESSAGE, &Master::executorMessage,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::message);

  install(S2M_EXITED_EXECUTOR, &Master::exitedExecutor,
          &ExitedExecutorMessage::slave_id,
          &ExitedExecutorMessage::framework_id,
          &ExitedExecutorMessage::executor_id,
          &ExitedExecutorMessage::result);

  install(process::EXITED, &Master::exited);

  // Install HTTP request handlers.
  Process<Master>::install("vars", &Master::vars);
}


void Master::newMasterDetected(const string& pid)
{
  // Check and see if we are (1) still waiting to be the active
  // master, (2) newly active master, (3) no longer active master,
  // or (4) still active master.

  UPID master(pid);

  if (master != self() && !active) {
    LOG(INFO) << "Waiting to be master!";
  } else if (master == self() && !active) {
    LOG(INFO) << "Acting as master!";
    active = true;
  } else if (master != self() && active) {
    LOG(FATAL) << "No longer active master ... committing suicide!";
  } else if (master == self() && active) {
    LOG(INFO) << "Still acting as master!";
  }
}


void Master::noMasterDetected()
{
  if (active) {
    LOG(FATAL) << "No longer active master ... committing suicide!";
  } else {
    LOG(FATAL) << "No master detected (?) ... committing suicide!";
  }
}


void Master::masterDetectionFailure()
{
  LOG(FATAL) << "Cannot reliably detect master ... committing suicide!";
}


void Master::registerFramework(const FrameworkInfo& frameworkInfo)
{
  Framework* framework =
    new Framework(frameworkInfo, newFrameworkId(), from(), elapsed());

  LOG(INFO) << "Registering framework " << framework << " at " << from();

  if (framework->info.executor().uri() == "") {
    LOG(INFO) << framework << " registering without an executor URI";
    MSG<M2F_ERROR> out;
    out.set_code(1);
    out.set_message("No executor URI given");
    send(from(), out);
    delete framework;
  } else {
    bool rootSubmissions = conf.get<bool>("root_submissions", true);
    if (framework->info.user() == "root" && rootSubmissions == false) {
      LOG(INFO) << framework << " registering as root, but "
                << "root submissions are disabled on this cluster";
      MSG<M2F_ERROR> out;
      out.set_code(1);
      out.set_message("User 'root' is not allowed to run frameworks");
      send(from(), out);
      delete framework;
    }
  }

  addFramework(framework);
}


void Master::reregisterFramework(const FrameworkID& frameworkId,
                                 const FrameworkInfo& frameworkInfo,
                                 int32_t generation)
{
  if (frameworkId == "") {
    LOG(ERROR) << "Framework re-registering without an id!";
    MSG<M2F_ERROR> out;
    out.set_code(1);
    out.set_message("Missing framework id");
    send(from(), out);
  } else if (frameworkInfo.executor().uri() == "") {
    LOG(INFO) << "Framework " << frameworkId << " re-registering "
              << "without an executor URI";
    MSG<M2F_ERROR> out;
    out.set_code(1);
    out.set_message("No executor URI given");
    send(from(), out);
  } else {
    LOG(INFO) << "Re-registering framework " << frameworkId
              << " at " << from();

    if (frameworks.count(frameworkId) > 0) {
      // Using the "generation" of the scheduler allows us to keep a
      // scheduler that got partitioned but didn't die (in ZooKeeper
      // speak this means didn't lose their session) and then
      // eventually tried to connect to this master even though
      // another instance of their scheduler has reconnected. This
      // might not be an issue in the future when the
      // master/allocator launches the scheduler can get restarted
      // (if necessary) by the master and the master will always
      // know which scheduler is the correct one.
      if (generation == 0) {
        LOG(INFO) << "Framework " << frameworkId << " failed over";
        failoverFramework(frameworks[frameworkId], from());
        // TODO: Should we check whether the new scheduler has given
        // us a different framework name, user name or executor info?
      } else {
        LOG(INFO) << "Framework " << frameworkId
                  << " re-registering with an already used id "
                  << " and not failing over!";
        MSG<M2F_ERROR> out;
        out.set_code(1);
        out.set_message("Framework id in use");
        send(from(), out);
        return;
      }
    } else {
      // We don't have a framework with this ID, so we must be a newly
      // elected Mesos master to which either an existing scheduler or a
      // failed-over one is connecting. Create a Framework object and add
      // any tasks it has that have been reported by reconnecting slaves.
      Framework* framework =
        new Framework(frameworkInfo, frameworkId, from(), elapsed());

      // TODO(benh): Check for root submissions like above!

      addFramework(framework);
      // Add any running tasks reported by slaves for this framework.
      foreachpair (const SlaveID& slaveId, Slave* slave, slaves) {
        foreachpair (_, Task* task, slave->tasks) {
          if (framework->frameworkId == task->framework_id()) {
            framework->addTask(task);
          }
        }
      }
    }

    CHECK(frameworks.count(frameworkId) > 0);

    // Broadcast the new framework pid to all the slaves. We have to
    // broadcast because an executor might be running on a slave but
    // it currently isn't running any tasks. This could be a
    // potential scalability issue ...
    foreachpair (_, Slave* slave, slaves) {
      MSG<M2S_UPDATE_FRAMEWORK> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.set_pid(from());
      send(slave->pid, out);
    }
  }
}


void Master::unregisterFramework(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to unregister framework " << frameworkId;

  Framework* framework = lookupFramework(frameworkId);
  if (framework != NULL) {
    if (framework->pid == from()) {
      removeFramework(framework);
    } else {
      LOG(WARNING) << from() << " tried to unregister framework; "
                   << "expecting " << framework->pid;
    }
  }
}


void Master::resourceOfferReply(const FrameworkID& frameworkId,
                                const OfferID& offerId,
                                const vector<TaskDescription>& tasks,
                                const Params& params)
{
  Framework* framework = lookupFramework(frameworkId);
  if (framework != NULL) {
    SlotOffer* offer = lookupSlotOffer(offerId);
    if (offer != NULL) {
      processOfferReply(offer, tasks, params);
    } else {
      // The slot offer is gone, meaning that we rescinded it, it
      // has already been replied to, or that the slave was lost;
      // immediately report any tasks in it as lost (it would
      // probably be better to have better error messages here).
      foreach (const TaskDescription& task, tasks) {
        MSG<M2F_STATUS_UPDATE> out;
        out.mutable_framework_id()->MergeFrom(frameworkId);
        TaskStatus* status = out.mutable_status();
        status->mutable_task_id()->MergeFrom(task.task_id());
        status->mutable_slave_id()->MergeFrom(task.slave_id());
        status->set_state(TASK_LOST);
        send(framework->pid, out);
      }
    }
  }
}


void Master::reviveOffers(const FrameworkID& frameworkId)
{
  Framework* framework = lookupFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Reviving offers for " << framework;
    framework->slaveFilter.clear();
    allocator->offersRevived(framework);
  }
}


void Master::killTask(const FrameworkID& frameworkId,
                      const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = lookupFramework(frameworkId);
  if (framework != NULL) {
    Task* task = framework->lookupTask(taskId);
    if (task != NULL) {
      Slave* slave = lookupSlave(task->slave_id());
      CHECK(slave != NULL);

      LOG(INFO) << "Telling slave " << slave->slaveId
                << " to kill task " << taskId
                << " of framework " << frameworkId;

      MSG<M2S_KILL_TASK> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_task_id()->MergeFrom(taskId);
      send(slave->pid, out);
    } else {
      LOG(WARNING) << "Cannot kill task " << taskId
                   << " of framework " << frameworkId
                   << " because it cannot be found";
      MSG<M2F_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      TaskStatus *status = out.mutable_status();
      status->mutable_task_id()->MergeFrom(taskId);
      status->mutable_slave_id()->set_value("UNKNOWN");
      status->set_state(TASK_LOST);
      send(framework->pid, out);
    }
  }
}


void Master::schedulerMessage(const FrameworkID& frameworkId,
                              const FrameworkMessage& message)
{
  Framework* framework = lookupFramework(frameworkId);
  if (framework != NULL) {
    Slave* slave = lookupSlave(message.slave_id());
    if (slave != NULL) {
      LOG(INFO) << "Sending framework message for framework "
                << frameworkId << " to slave " << slave->slaveId;
      MSG<M2S_FRAMEWORK_MESSAGE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_message()->MergeFrom(message);
      send(slave->pid, out);
    } else {
      LOG(WARNING) << "Cannot send framework message for framework "
                   << frameworkId << " to slave " << message.slave_id()
                   << " because slave does not exist";
    }
  } else {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << frameworkId << " to slave " << message.slave_id()
                 << " because framework does not exist";
  }
}


void Master::statusUpdateAck(const FrameworkID& frameworkId,
                             const TaskID& taskId,
                             const SlaveID& slaveId)
{
  Framework* framework = lookupFramework(frameworkId);
  if (framework != NULL) {
    Slave* slave = lookupSlave(slaveId);
    if (slave != NULL) {
      LOG(INFO) << "Sending slave " << slaveId
                << " status update acknowledgement for task " << taskId
                << " of framework " << frameworkId;
      MSG<M2S_STATUS_UPDATE_ACK> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_slave_id()->MergeFrom(slaveId);
      out.mutable_task_id()->MergeFrom(taskId);
      send(slave->pid, out);
    } else {
      LOG(WARNING) << "Cannot tell slave " << slaveId
                   << " of status update acknowledgement for task " << taskId
                   << " of framework " << frameworkId
                   << " because slave does not exist";
    }
  } else {
    LOG(WARNING) << "Cannot tell slave " << slaveId
                 << " of status update acknowledgement for task " << taskId
                 << " of framework " << frameworkId
                 << " because framework does not exist";
  }
}


void Master::registerSlave(const SlaveInfo& slaveInfo)
{
  addSlave(slaveInfo, newSlaveId(), from());
}


void Master::reregisterSlave(const SlaveID& slaveId,
                             const SlaveInfo& slaveInfo,
                             const vector<Task>& tasks)
{
  LOG(INFO) << "Re-registering " << slaveId << " at " << from();

  if (slaveId == "") {
    LOG(ERROR) << "Slave re-registered without a SlaveID!";
    send(from(), process::TERMINATE);
  } else {
    if (lookupSlave(slaveId) != NULL) {
      // TODO(benh): Once we support handling session expiration, we
      // will want to handle having a slave re-register with us when
      // we already have them recorded.
      LOG(ERROR) << "Slave re-registered with in use SlaveID!";
      send(from(), process::TERMINATE);
    } else {
      Slave* slave = addSlave(slaveInfo, slaveId, from());
      if (slave != NULL) {
        for (int i = 0; i < tasks.size(); i++) {
          Task* task = new Task(tasks[i]);

          // Add the task to the slave.
          slave->addTask(task);

          // Try and add the task to the framework too, but since the
          // framework might not yet be connected we won't be able to
          // add them. However, when the framework connects later we
          // will add them then. We also tell this slave the current
          // framework pid for this task. Again, we do the same thing
          // if a framework currently isn't registered.
          Framework* framework = lookupFramework(task->framework_id());
          if (framework != NULL) {
            framework->addTask(task);
            MSG<M2S_UPDATE_FRAMEWORK> out;
            out.mutable_framework_id()->MergeFrom(framework->frameworkId);
            out.set_pid(framework->pid);
            send(slave->pid, out);
          } else {
            // TODO(benh): We should really put a timeout on how long we
            // keep tasks running on a slave that never have frameworks
            // reregister and claim them.
            LOG(WARNING) << "Possibly orphaned task " << task->task_id()
                         << " of framework " << task->framework_id()
                         << " running on slave " << slaveId;
          }
        }
      }
    }
  }
}


void Master::unregisterSlave(const SlaveID& slaveId)
{
  LOG(INFO) << "Asked to unregister slave " << slaveId;

  // TODO(benh): Check that only the slave is asking to unregister?

  Slave* slave = lookupSlave(slaveId);
  if (slave != NULL) {
    removeSlave(slave);
  }
}


void Master::statusUpdate(const FrameworkID& frameworkId,
                          const TaskStatus& status)
{
  LOG(INFO) << "Status update: task " << status.task_id()
            << " of framework " << frameworkId
            << " is now in state "
            << TaskState_descriptor()->FindValueByNumber(status.state())->name();

  Slave* slave = lookupSlave(status.slave_id());
  if (slave != NULL) {
    Framework* framework = lookupFramework(frameworkId);
    if (framework != NULL) {
      // Pass on the (transformed) status update to the framework.
      MSG<M2F_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_status()->MergeFrom(status);
      send(framework->pid, out);

      // Lookup the task and see if we need to update anything locally.
      Task* task = slave->lookupTask(frameworkId, status.task_id());
      if (task != NULL) {
        task->set_state(status.state());
        // Remove the task if necessary.
        if (status.state() == TASK_FINISHED ||
            status.state() == TASK_FAILED ||
            status.state() == TASK_KILLED ||
            status.state() == TASK_LOST) {
          removeTask(task, TRR_TASK_ENDED);
        }
      } else {
        LOG(WARNING) << "Status update error: couldn't lookup "
                     << "task " << status.task_id();
      }
    } else {
      LOG(WARNING) << "Status update error: couldn't lookup "
                   << "framework " <<frameworkId;
    }
  } else {
    LOG(WARNING) << "Status update error: couldn't lookup slave "
                 << status.slave_id();
  }
}

void Master::executorMessage(const FrameworkID& frameworkId,
                             const FrameworkMessage& message)
{
  Slave* slave = lookupSlave(message.slave_id());
  if (slave != NULL) {
    Framework* framework = lookupFramework(frameworkId);
    if (framework != NULL) {
      LOG(INFO) << "Sending framework message from slave " << slave->slaveId
                << " to framework " << frameworkId;
      MSG<M2S_FRAMEWORK_MESSAGE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_message()->MergeFrom(message);
      send(framework->pid, out);
    } else {
      LOG(WARNING) << "Cannot send framework message from slave "
                   << slave->slaveId << " to framework " << frameworkId
                   << " because framework does not exist";
    }
  } else {
    LOG(WARNING) << "Cannot send framework message from slave "
                 << message.slave_id() << " to framework " << frameworkId
                 << " because slave does not exist";
  }
}


void Master::exitedExecutor(const SlaveID& slaveId,
                            const FrameworkID& frameworkId,
                            const ExecutorID& executorId,
                            int32_t result)
{
  Slave* slave = lookupSlave(slaveId);
  if (slave != NULL) {
    Framework* framework = lookupFramework(frameworkId);
    if (framework != NULL) {
      LOG(INFO) << "Executor " << executorId
                << " of framework " << framework->frameworkId
                << " on slave " << slave->slaveId
                << " (" << slave->info.hostname() << ") "
                << "exited with result " << result;

      // Tell the framework which tasks have been lost.
      foreachpaircopy (_, Task* task, framework->tasks) {
        if (task->slave_id() == slave->slaveId &&
            task->executor_id() == executorId) {
          MSG<M2F_STATUS_UPDATE> out;
          out.mutable_framework_id()->MergeFrom(task->framework_id());
          TaskStatus* status = out.mutable_status();
          status->mutable_task_id()->MergeFrom(task->task_id());
          status->mutable_slave_id()->MergeFrom(task->slave_id());
          status->set_state(TASK_LOST);
          send(framework->pid, out);

          LOG(INFO) << "Removing task " << task->task_id()
                    << " of framework " << frameworkId
                    << " because of lost executor";

          removeTask(task, TRR_EXECUTOR_LOST);
        }
      }

      // TODO(benh): Send the framework it's executor's exit
      // status? Or maybe at least have something like
      // M2F_EXECUTOR_LOST?
    }
  }
}


void Master::activatedSlaveHostnamePort(const string& hostname, uint16_t port)
{
  LOG(INFO) << "Master now considering a slave at "
            << hostname << ":" << port << " as active";
  slaveHostnamePorts[hostname].insert(port);
}


void Master::deactivatedSlaveHostnamePort(const string& hostname, uint16_t port)
{
  if (slaveHostnamePorts.count(hostname) > 0 &&
      slaveHostnamePorts[hostname].count(port) > 0) {
    // Look for a connected slave and remove it.
    foreachpair (_, Slave* slave, slaves) {
      if (slave->info.hostname() == hostname && slave->pid.port == port) {
        LOG(WARNING) << "Removing slave " << slave->slaveId
                     << " because it has been deactivated";
        removeSlave(slave);
        break;
      }
    }

    slaveHostnamePorts[hostname].erase(port);
    if (slaveHostnamePorts[hostname].size() == 0) slaveHostnamePorts.erase(hostname);
  }
}


void Master::timerTick()
{
  // Check which framework filters can be expired.
  foreachpair (_, Framework* framework, frameworks) {
    framework->removeExpiredFilters(elapsed());
  }

  // Do allocations!
  allocator->timerTick();
}


void Master::frameworkExpired(const FrameworkID& frameworkId)
{
  Framework* framework = lookupFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Framework failover timer expired, removing "
              << framework;
    removeFramework(framework);
  }
}


void Master::exited()
{
  // TODO(benh): Could we get PROCESS_EXIT from a network partition?
  LOG(INFO) << "Process exited: " << from();
  if (pidToFrameworkId.count(from()) > 0) {
    const FrameworkID& frameworkId = pidToFrameworkId[from()];
    Framework* framework = lookupFramework(frameworkId);
    if (framework != NULL) {
      LOG(INFO) << "Framework " << frameworkId << " disconnected";

      // Stop sending offers here for now.
      framework->active = false;

      // Remove the framework's slot offers.
      foreachcopy (SlotOffer* offer, framework->slotOffers) {
        removeSlotOffer(offer, ORR_FRAMEWORK_FAILOVER, offer->resources);
      }

      framework->failoverTimer =
        new FrameworkFailoverTimer(self(), frameworkId);
      link(spawn(framework->failoverTimer));
//       removeFramework(framework);
    }
  } else if (pidToSlaveId.count(from()) > 0) {
    const SlaveID& slaveId = pidToSlaveId[from()];
    Slave* slave = lookupSlave(slaveId);
    if (slave != NULL) {
      LOG(INFO) << slave << " disconnected";
      removeSlave(slave);
    }
  } else {
    foreachpair (_, Framework* framework, frameworks) {
      if (framework->failoverTimer != NULL &&
          framework->failoverTimer->self() == from()) {
        LOG(ERROR) << "Bad framework failover timer, removing "
                   << framework;
        removeFramework(framework);
        break;
      }
    }
  }
}


Promise<HttpResponse> Master::vars(const HttpRequest& request)
{
  LOG(INFO) << "Request for 'vars'";

  ostringstream out;

  out <<
    "build_date " << build::DATE << "\n" <<
    "build_user " <<  build::USER << "\n" <<
    "build_flags " <<  build::FLAGS << "\n" <<
    "frameworks_count " << frameworks.size() << "\n";

  // Also add the configuration values.
  foreachpair (const string& key, const string& value, conf.getMap()) {
    out << key << " " << value << "\n";
  }

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}


OfferID Master::makeOffer(Framework* framework,
                          const vector<SlaveResources>& resources)
{
  const OfferID& offerId = newOfferId();

  SlotOffer* offer = new SlotOffer(offerId, framework->frameworkId, resources);

  slotOffers[offer->offerId] = offer;
  framework->addOffer(offer);

  // Update the resource information within each of the slave objects. Gross!
  foreach (const SlaveResources& r, resources) {
    r.slave->slotOffers.insert(offer);
    r.slave->resourcesOffered += r.resources;
  }

  LOG(INFO) << "Sending offer " << offer->offerId
            << " to framework " << framework->frameworkId;

  MSG<M2F_RESOURCE_OFFER> out;
  out.mutable_offer_id()->MergeFrom(offerId);

  foreach (const SlaveResources& r, resources) {
    SlaveOffer* offer = out.add_offers();
    offer->mutable_slave_id()->MergeFrom(r.slave->slaveId);
    offer->set_hostname(r.slave->info.hostname());
    offer->mutable_resources()->MergeFrom(r.resources);

    out.add_pids(r.slave->pid);
  }

  send(framework->pid, out);

  return offerId;
}


// Process a resource offer reply (for a non-cancelled offer) by launching
// the desired tasks (if the offer contains a valid set of tasks) and
// reporting any unused resources to the allocator.
void Master::processOfferReply(SlotOffer* offer,
                               const vector<TaskDescription>& tasks,
                               const Params& params)
{
  LOG(INFO) << "Received reply for " << offer;

  Framework* framework = lookupFramework(offer->frameworkId);
  CHECK(framework != NULL);

  // Count resources in the offer.
  unordered_map<Slave*, Resources> resourcesOffered;
  foreach (const SlaveResources& r, offer->resources) {
    resourcesOffered[r.slave] = r.resources;
  }

  // Count used resources and check that its tasks are valid.
  unordered_map<Slave*, Resources> resourcesUsed;
  foreach (const TaskDescription& task, tasks) {
    // Check whether the task is on a valid slave.
    Slave* slave = lookupSlave(task.slave_id());
    if (slave == NULL || resourcesOffered.count(slave) == 0) {
      terminateFramework(framework, 0, "Invalid slave in offer reply");
      return;
    }

    // Check whether or not the resources for the task are valid.
    // TODO(benh): In the future maybe we can also augment the
    // protobuf to deal with fragmentation purposes by providing some
    // sort of minimum amount of resources required per task.

    if (task.resources().size() == 0) {
      terminateFramework(framework, 0, "Invalid resources for task");
      return;
    }

    foreach (const Resource& resource, task.resources()) {
      if (!Resources::isAllocatable(resource)) {
        // TODO(benh): Also send back the invalid resources as a string?
        terminateFramework(framework, 0, "Invalid resources for task");
        return;
      }
    }

    resourcesUsed[slave] += task.resources();
  }

  // Check that the total accepted on each slave isn't more than offered.
  foreachpair (Slave* slave, const Resources& used, resourcesUsed) {
    if (!(used <= resourcesOffered[slave])) {
      terminateFramework(framework, 0, "Too many resources accepted");
      return;
    }
  }

  // Check that there are no duplicate task IDs.
  unordered_set<TaskID> idsInResponse;
  foreach (const TaskDescription& task, tasks) {
    if (framework->tasks.count(task.task_id()) > 0 ||
        idsInResponse.count(task.task_id()) > 0) {
      terminateFramework(framework, 0, "Duplicate task ID: " +
                         lexical_cast<string>(task.task_id()));
      return;
    }
    idsInResponse.insert(task.task_id());
  }

  // Launch the tasks in the response.
  foreach (const TaskDescription& task, tasks) {
    launchTask(framework, task);
  }

  // Get out the timeout for left over resources (if exists), and use
  // that to calculate the expiry timeout.
  int timeout = DEFAULT_REFUSAL_TIMEOUT;

  for (int i = 0; i < params.param_size(); i++) {
    if (params.param(i).key() == "timeout") {
      timeout = lexical_cast<int>(params.param(i).value());
      break;
    }
  }

  double expiry = (timeout == -1) ? 0 : elapsed() + timeout;  

  // Now check for unused resources on slaves and add filters for them.
  vector<SlaveResources> resourcesUnused;

  foreachpair (Slave* slave, const Resources& offered, resourcesOffered) {
    Resources used = resourcesUsed[slave];
    Resources unused = offered - used;

    CHECK(used == used.allocatable());

    Resources allocatable = unused.allocatable();

    if (allocatable.size() > 0) {
      resourcesUnused.push_back(SlaveResources(slave, allocatable));
    }

    // Only add a filter on a slave if none of the resources are used.
    if (timeout != 0 && used.size() == 0) {
      LOG(INFO) << "Adding filter on " << slave << " to " << framework
                << " for " << timeout << " seconds";
      framework->slaveFilter[slave] = expiry;
    }
  }
  
  // Return the resources left to the allocator.
  removeSlotOffer(offer, ORR_FRAMEWORK_REPLIED, resourcesUnused);
}


void Master::launchTask(Framework* framework, const TaskDescription& task)
{
  // The invariant right now is that launchTask is called only for
  // TaskDescriptions where the slave is still valid (see the code
  // above in processOfferReply).
  Slave* slave = lookupSlave(task.slave_id());
  CHECK(slave != NULL);

  // Determine the executor ID for this task.
  const ExecutorID& executorId = task.has_executor()
    ? task.executor().executor_id()
    : framework->info.executor().executor_id();

  Task* t = new Task();
  t->mutable_framework_id()->MergeFrom(framework->frameworkId);
  t->mutable_executor_id()->MergeFrom(executorId);
  t->set_state(TASK_STARTING);
  t->set_name(task.name());
  t->mutable_task_id()->MergeFrom(task.task_id());
  t->mutable_slave_id()->MergeFrom(task.slave_id());
  t->mutable_resources()->MergeFrom(task.resources());

  framework->addTask(t);
  slave->addTask(t);

  allocator->taskAdded(t);

  LOG(INFO) << "Launching " << t << " on " << slave;

  MSG<M2S_RUN_TASK> out;
  out.mutable_framework()->MergeFrom(framework->info);
  out.mutable_framework_id()->MergeFrom(framework->frameworkId);
  out.set_pid(framework->pid);
  out.mutable_task()->MergeFrom(task);
  send(slave->pid, out);
}


// Terminate a framework, sending it a particular error message
// TODO: Make the error codes and messages programmer-friendly
void Master::terminateFramework(Framework* framework,
                                int32_t code,
                                const string& message)
{
  LOG(INFO) << "Terminating " << framework << " due to error: " << message;

  MSG<M2F_ERROR> out;
  out.set_code(code);
  out.set_message(message);
  send(framework->pid, out);

  removeFramework(framework);
}


// Remove a slot offer (because it was replied or we lost a framework or slave)
void Master::removeSlotOffer(SlotOffer* offer,
                             OfferReturnReason reason,
                             const vector<SlaveResources>& resourcesUnused)
{
  // Remove from slaves.
  foreach (SlaveResources& r, offer->resources) {
    CHECK(r.slave != NULL);
    r.slave->resourcesOffered -= r.resources;
    r.slave->slotOffers.erase(offer);
  }
    
  // Remove from framework
  Framework *framework = lookupFramework(offer->frameworkId);
  CHECK(framework != NULL);
  framework->removeOffer(offer);

  // Also send framework a rescind message unless the reason we are
  // removing the offer is that the framework replied to it
  if (reason != ORR_FRAMEWORK_REPLIED) {
    MSG<M2F_RESCIND_OFFER> out;
    out.mutable_offer_id()->MergeFrom(offer->offerId);
    send(framework->pid, out);
  }
  
  // Tell the allocator about the unused resources.
  allocator->offerReturned(offer, reason, resourcesUnused);
  
  // Delete it
  slotOffers.erase(offer->offerId);
  delete offer;
}


void Master::addFramework(Framework* framework)
{
  CHECK(frameworks.count(framework->frameworkId) == 0);

  frameworks[framework->frameworkId] = framework;
  pidToFrameworkId[framework->pid] = framework->frameworkId;
  link(framework->pid);

  MSG<M2F_REGISTER_REPLY> out;
  out.mutable_framework_id()->MergeFrom(framework->frameworkId);
  send(framework->pid, out);

  allocator->frameworkAdded(framework);
}


// Replace the scheduler for a framework with a new process ID, in the
// event of a scheduler failover.
void Master::failoverFramework(Framework* framework, const UPID& newPid)
{
  const UPID& oldPid = framework->pid;

  // Remove the framework's slot offers (if they weren't removed before)..
  // TODO(benh): Consider just reoffering these to the new framework.
  foreachcopy (SlotOffer* offer, framework->slotOffers) {
    removeSlotOffer(offer, ORR_FRAMEWORK_FAILOVER, offer->resources);
  }

  MSG<M2F_ERROR> out;
  out.set_code(1);
  out.set_message("Framework failover");
  send(oldPid, out);

  // TODO(benh): unlink(oldPid);
  pidToFrameworkId.erase(oldPid);
  pidToFrameworkId[newPid] = framework->frameworkId;

  framework->pid = newPid;
  link(newPid);

  // Kill the failover timer.
  if (framework->failoverTimer != NULL) {
    process::post(framework->failoverTimer->self(), process::TERMINATE);
    process::wait(framework->failoverTimer->self());
    delete framework->failoverTimer;
    framework->failoverTimer = NULL;
  }

  // Make sure we can get offers again.
  framework->active = true;

  MSG<M2F_REGISTER_REPLY> reply;
  reply.mutable_framework_id()->MergeFrom(framework->frameworkId);
  send(newPid, reply);
}


// Kill all of a framework's tasks, delete the framework object, and
// reschedule slot offers for slots that were assigned to this framework
void Master::removeFramework(Framework* framework)
{ 
  framework->active = false;
  // TODO: Notify allocator that a framework removal is beginning?
  
  // Tell slaves to kill the framework
  foreachpair (_, Slave *slave, slaves) {
    MSG<M2S_KILL_FRAMEWORK> out;
    out.mutable_framework_id()->MergeFrom(framework->frameworkId);
    send(slave->pid, out);
  }

  // Remove pointers to the framework's tasks in slaves
  foreachpaircopy (_, Task *task, framework->tasks) {
    Slave *slave = lookupSlave(task->slave_id());
    CHECK(slave != NULL);
    removeTask(task, TRR_FRAMEWORK_LOST);
  }
  
  // Remove the framework's slot offers (if they weren't removed before).
  foreachcopy (SlotOffer* offer, framework->slotOffers) {
    removeSlotOffer(offer, ORR_FRAMEWORK_LOST, offer->resources);
  }

  // TODO(benh): Similar code between removeFramework and
  // failoverFramework needs to be shared!

  // TODO(benh): unlink(framework->pid);
  pidToFrameworkId.erase(framework->pid);

  // Delete it.
  frameworks.erase(framework->frameworkId);
  allocator->frameworkRemoved(framework);
  delete framework;
}


Slave* Master::addSlave(const SlaveInfo& slaveInfo,
                        const SlaveID& slaveId,
                        const UPID& pid)
{
  // TODO(benh): Start a reverse lookup to ensure IP maps to hostname.

  Slave* slave = NULL;

  // Check whether all slaves, or at least this slave is allocated.
  bool allocated = (conf.get<string>("slaves", "*") == "*") ||
    (slaveHostnamePorts.count(slaveInfo.hostname()) > 0 &&
     slaveHostnamePorts[slaveInfo.hostname()].count(pid.port) > 0);

  if (allocated) {
    Slave* slave = new Slave(slaveInfo, slaveId, pid, elapsed());

    LOG(INFO) << "Registering slave " << slave->slaveId
              << " at " << slave->pid;

    slaves[slave->slaveId] = slave;
    pidToSlaveId[slave->pid] = slave->slaveId;
    link(slave->pid);

    allocator->slaveAdded(slave);

    MSG<M2S_REGISTER_REPLY> out;
    out.mutable_slave_id()->MergeFrom(slave->slaveId);
    send(slave->pid, out);

    // TODO(benh):
    //     // Ask the slaves manager to monitor this slave for us.
    //     process::dispatch(slavesManager->self(), &SlavesManager::monitor,
    //                       slave->pid, slave->info, slave->slaveId);

    // Set up an observer for the slave.
    slave->observer = new SlaveObserver(slave->pid, slave->info,
                                        slave->slaveId, slavesManager->self());
    process::spawn(slave->observer);
  } else {
    LOG(WARNING) << "Cannot add slave at " << slaveInfo.hostname()
                 << " because not in allocated set of slaves!";
  }

  return slave;
}


// Lose all of a slave's tasks and delete the slave object
void Master::removeSlave(Slave* slave)
{ 
  slave->active = false;
  // TODO: Notify allocator that a slave removal is beginning?
  
  // Remove pointers to slave's tasks in frameworks, and send status updates
  foreachpaircopy (_, Task* task, slave->tasks) {
    Framework *framework = lookupFramework(task->framework_id());
    // A framework might not actually exist because the master failed
    // over and the framework hasn't reconnected. This can be a tricky
    // situation for frameworks that want to have high-availability,
    // because if they eventually do connect they won't ever get a
    // status update about this task.  Perhaps in the future what we
    // want to do is create a local Framework object to represent that
    // framework until it fails over. See the TODO above in
    // S2M_REREGISTER_SLAVE.
    if (framework != NULL) {
      MSG<M2F_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(task->framework_id());
      TaskStatus* status = out.mutable_status();
      status->mutable_task_id()->MergeFrom(task->task_id());
      status->mutable_slave_id()->MergeFrom(task->slave_id());
      status->set_state(TASK_LOST);
      send(framework->pid, out);
    }
    removeTask(task, TRR_SLAVE_LOST);
  }

  // Remove slot offers from the slave; this will also rescind them
  foreachcopy (SlotOffer* offer, slave->slotOffers) {
    // Only report resources on slaves other than this one to the allocator
    vector<SlaveResources> otherSlaveResources;
    foreach (const SlaveResources& r, offer->resources) {
      if (r.slave != slave) {
        otherSlaveResources.push_back(r);
      }
    }
    removeSlotOffer(offer, ORR_SLAVE_LOST, otherSlaveResources);
  }
  
  // Remove slave from any filters
  foreachpair (_, Framework* framework, frameworks) {
    framework->slaveFilter.erase(slave);
  }
  
  // Send lost-slave message to all frameworks (this helps them re-run
  // previously finished tasks whose output was on the lost slave)
  foreachpair (_, Framework* framework, frameworks) {
    MSG<M2F_LOST_SLAVE> out;
    out.mutable_slave_id()->MergeFrom(slave->slaveId);
    send(framework->pid, out);
  }

  // TODO(benh):
  //     // Tell the slaves manager to stop monitoring this slave for us.
  //     process::dispatch(slavesManager->self(), &SlavesManager::forget,
  //                       slave->pid, slave->info, slave->slaveId);

  // Kill the slave observer.
  process::post(slave->observer->self(), process::TERMINATE);
  process::wait(slave->observer->self());
  delete slave->observer;

  // TODO(benh): unlink(slave->pid);
  pidToSlaveId.erase(slave->pid);

  // Delete it
  slaves.erase(slave->slaveId);
  allocator->slaveRemoved(slave);
  delete slave;
}


// Remove a slot offer (because it was replied or we lost a framework or slave)
void Master::removeTask(Task* task, TaskRemovalReason reason)
{
  Framework* framework = lookupFramework(task->framework_id());
  Slave* slave = lookupSlave(task->slave_id());
  CHECK(framework != NULL);
  CHECK(slave != NULL);
  framework->removeTask(task->task_id());
  slave->removeTask(task);
  allocator->taskRemoved(task, reason);
  delete task;
}


Allocator* Master::createAllocator()
{
  LOG(INFO) << "Creating \"" << allocatorType << "\" allocator";
  return AllocatorFactory::instantiate(allocatorType, this);
}


// Create a new framework ID. We format the ID as MASTERID-FWID, where
// MASTERID is the ID of the master (launch date plus fault tolerant ID)
// and FWID is an increasing integer.
FrameworkID Master::newFrameworkId()
{
  ostringstream oss;
  oss << masterId << "-" << setw(4) << setfill('0') << nextFrameworkId++;
  FrameworkID frameworkId;
  frameworkId.set_value(oss.str());
  return frameworkId;
}


OfferID Master::newOfferId()
{
  OfferID offerId;
  offerId.set_value(masterId + "-" + lexical_cast<string>(nextOfferId++));
  return offerId;
}


SlaveID Master::newSlaveId()
{
  SlaveID slaveId;
  slaveId.set_value(masterId + "-" + lexical_cast<string>(nextSlaveId++));
  return slaveId;
}


const Configuration& Master::getConfiguration()
{
  return conf;
}
