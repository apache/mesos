#include <zookeeper.h>

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::master;

// A process for the master to communicate with ZooKeeper.
class ZooKeeperProcessForMaster : public Tuple<Process>
{
private:
  static zhandle_t* zh; // ZooKeeper connection handle.
  static string zookeeper; // ZooKeeper server host:port.

public:
  static string master; // Master PID as string.

protected:
  void operator () ()
  {
    while (true) {
      int fd;
      int interest;
      struct timeval tv;

      if (zookeeper_interest(zh, &fd, &interest, &tv) != ZOK)
	fatal("zookeeper_interest failed!");

      int op = 0;

      if ((interest & ZOOKEEPER_READ) && (interest & ZOOKEEPER_WRITE)) {
	op |= RDWR;
      } else if (interest & ZOOKEEPER_READ) {
	op |= RDONLY;
      } else if (interest & ZOOKEEPER_WRITE) {
	op |= WRONLY;
      }

      if (await(fd, op, tv)) {
	int events = 0;
	if (ready(fd, RDONLY))
	  events |= ZOOKEEPER_READ;
	if (ready(fd, WRONLY))
	  events |= ZOOKEEPER_WRITE;

	int ret = zookeeper_process(zh, events);

	if (ret != ZOK && ret != ZNOTHING) {
	  fatal("zookeeper_process failed! (%s)", zerror(ret));
	}
      } else {
	LOG(WARNING) << "ZooKeeperProcess interrupted during await ... ";
      }
    }
  }

  static void create_completion(int ret, const char* name, const void* data)
  {
    string* znode = static_cast<string*>(const_cast<void*>(data));
    if (ret != ZOK)
      fatal("zookeeper create failed on '%s': %s", znode->c_str(), zerror(ret));
    delete znode;
  }

  static void create_completion_ignore_node_exists(int ret,
						   const char* name,
						   const void* data)
  {
    string* znode = static_cast<string*>(const_cast<void*>(data));
    if (ret != ZOK && ret != ZNODEEXISTS)
      fatal("zookeeper create failed on '%s': %s", znode->c_str(), zerror(ret));
    delete znode;
  }

  static void watcher(zhandle_t *zzh, int type, int state,
		      const char *path, void* context)
  {
    CHECK(zh != NULL);

    if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
      // Register master.
      static string basename = "/home/nexus";
      static string delimiter = "/";
      size_t index = basename.find(delimiter, 0);

      int ret;

      // Create basename znodes as necessary.
      while (index < string::npos) {
	index = basename.find(delimiter, index+1);
	string* prefix = new string(basename.substr(0, index));
  	ret = zoo_acreate(zh, prefix->c_str(), "", 0, &ZOO_CREATOR_ALL_ACL,
			  0, create_completion_ignore_node_exists, prefix);
	if (ret != ZOK)
	  fatal("zoo_acreate failed! (%s)", zerror(ret));
      }

      string* znode = new string(basename + delimiter + "master");

      ret = zoo_acreate(zh, znode->c_str(), master.c_str(), master.length(),
			&ZOO_CREATOR_ALL_ACL, ZOO_EPHEMERAL,
			create_completion, znode);

      if (ret != ZOK)
 	fatal("zoo_acreate failed! (%s)", zerror(ret));
    } else {
      fatal("unhandled ZooKeeper event!");
    }
  }

public:
  ZooKeeperProcessForMaster(const string& _master, const string& _zookeeper)
  {
    master = _master;
    zookeeper = _zookeeper;
    zh = zookeeper_init(zookeeper.c_str(), watcher, 10000, 0, NULL, 0);
    if (zh == NULL)
      fatal("zookeeper_init failed!");
  }

  ~ZooKeeperProcessForMaster() {}
};


zhandle_t* ZooKeeperProcessForMaster::zh = NULL;
string ZooKeeperProcessForMaster::zookeeper = "";
string ZooKeeperProcessForMaster::master = "";


