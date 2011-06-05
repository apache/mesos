#ifndef __MASTER_DETECTOR_HPP__
#define __MASTER_DETECTOR_HPP__

#include <string>
#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>

#include <process.hpp>


namespace mesos { namespace internal {

/**
 * Implements functionality for:
 *   a) detecting masters
 *   b) contending to be a master
 */
class MasterDetector
{
public:
  virtual ~MasterDetector() = 0;

  /**
   * Creates a master detector that, given the specified url, knows
   * how to connect to the master, or contend to be a master. The
   * master detector sends messages to the specified pid when a new
   * master is elected, a master is lost, etc.
   *
   * @param url string possibly containing zoo://, zoofile://, mesos://
   * @param pid libprocess pid to both receive our messages and be
   * used if we should contend
   * @param contend true if should contend to be master
   * @return instance of MasterDetector
   */
  static MasterDetector * create(const std::string& url,
				 const process::UPID& pid,
				 bool contend = false,
				 bool quiet = true);

  /**
   * Cleans up and deallocates the detector.
   */
  static void destroy(MasterDetector* detector);
};


class BasicMasterDetector : public MasterDetector
{
public:
  /**
   * Create a new master detector where the specified pid contends to
   * be the master and gets elected by default.
   *
   * @param master libprocess pid to send messages/updates and be the
   * master
   */
  BasicMasterDetector(const process::UPID& master);

  /**
   * Create a new master detector where the 'master' pid is 
   * the master (no contending).
   *
   * @param master libprocess pid to send messages/updates and be the
   * master
   * @param pid/pids libprocess pids to send messages/updates to regarding
   * the master
   * @param elect if true then contend and elect the specified master
   */
  BasicMasterDetector(const process::UPID& master,
		      const process::UPID& pid,
		      bool elect = false);

  BasicMasterDetector(const process::UPID& master,
		      const std::vector<process::UPID>& pids,
		      bool elect = false);

  virtual ~BasicMasterDetector();

private:
  const process::UPID master;
};

}} // namespace mesos { namespace internal {

#endif // __MASTER_DETECTOR_HPP__

