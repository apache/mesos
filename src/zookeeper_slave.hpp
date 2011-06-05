#include <zookeeper.h>

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::slave;

// A process for the slave to communicate with ZooKeeper.
class ZooKeeperProcessForSlave : public Tuple<Process>
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
      if (!master.empty())
	return;

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

  static void get_completion(int ret, const char* value, int len,
			     const struct Stat* stat, const void* data)
  {
    string* znode = static_cast<string*>(const_cast<void*>(data));
    if (ret != ZOK)
      fatal("zookeeper get failed on '%s': %s", znode->c_str(), zerror(ret));
    master = string(value, len);
    delete znode;
  }

  static void watcher(zhandle_t *zzh, int type, int state,
		      const char *path, void* context)
  {
    CHECK(zh != NULL);

    if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
      // Lookup master.
      string* znode = new string("/home/nexus/master");

      int ret = zoo_aget(zh, znode->c_str(), 0, get_completion, znode);

      if (ret != ZOK)
	fatal("zoo_aget failed! (%s)", zerror(ret));
    } else {
      fatal("unhandled ZooKeeper event!");
    }
  }

public:
  ZooKeeperProcessForSlave(const string& _zookeeper)
  {
    zookeeper = _zookeeper;
    zh = zookeeper_init(zookeeper.c_str(), watcher, 10000, 0, NULL, 0);
    if (zh == NULL)
      fatal("zookeeper_init failed!");
  }

  ~ZooKeeperProcessForSlave() {}
};


zhandle_t* ZooKeeperProcessForSlave::zh = NULL;
string ZooKeeperProcessForSlave::zookeeper = "";
string ZooKeeperProcessForSlave::master = "";
