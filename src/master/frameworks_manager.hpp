#ifndef __MASTER_FRAMEWORKS_MANAGER_HPP__
#define __MASTER_FRAMEWORKS_MANAGER_HPP__

// TODO(vinod): We are not using hashmap because it is giving
// strange compile errors for ref. Need to fix this in the future.
#include <map>

#include <process/process.hpp>

#include "common/option.hpp"
#include "common/result.hpp"

#include "messages/messages.hpp"

namespace mesos { namespace internal { namespace master {

using namespace process;

class FrameworksStorage : public Process<FrameworksStorage>
{
public:
  virtual Promise<Result<std::map<FrameworkID, FrameworkInfo> > > list() = 0;

  virtual Promise<Result<bool> > add(const FrameworkID& id,
      const FrameworkInfo& info) = 0;

  virtual Promise<Result<bool> > remove(const FrameworkID& id) = 0;
};

class FrameworksManager : public Process<FrameworksManager>
{
public:
  // Initializes the framework with underlying storage.
  FrameworksManager(FrameworksStorage* _storage);

  // Return the map of all the frameworks.
  Result<std::map<FrameworkID, FrameworkInfo> > list();

  // Add a new framework.
  Result<bool> add(const FrameworkID& id, const FrameworkInfo& info);

  // Remove a framework after a certain delay.
  Promise<Result<bool> > remove(const FrameworkID& id, double delay_secs);

  // Resurrect the framework.
  Result<bool> resurrect(const FrameworkID& id);
  //Result<bool> resurrect(const FrameworkID& id, const FrameworkInfo& info);

  // Check if the framework exists.
  Result<bool> exists(const FrameworkID& id);

private:
  void expire(const FrameworkID& id, Promise<Result<bool> > promise);

  bool cache();

  // Underlying storage implementation.
  FrameworksStorage* storage;

  // Whether or not we have cached the info from the underlying storage.
  bool cached;

  // Cached info about the frameworks.
  std::map<FrameworkID, std::pair<FrameworkInfo, Option<double> > > infos;
};

}}} // namespace mesos { namespace internal { namespace master {
#endif // __MASTER_FRAMEWORKS_MANAGER_HPP__
