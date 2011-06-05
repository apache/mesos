#ifndef __MASTER_DETECTOR_HPP__
#define __MASTER_DETECTOR_HPP__

#include <string>
#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>


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
  static MasterDetector * create(const std::string &url,
				 const PID &pid,
				 bool contend = false,
				 bool quiet = true);

  /**
   * Cleans up and deallocates the detector.
   */
  static void destroy(MasterDetector *detector);

  /**
   * @return unique id of the current master
   */
  virtual std::string getCurrentMasterId() = 0;

  /**
   * @return libprocess PID of the current master
   */
  virtual PID getCurrentMasterPID() = 0;
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
  BasicMasterDetector(const PID &master);

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
  BasicMasterDetector(const PID &master,
		      const PID &pid,
		      bool elect = false);

  BasicMasterDetector(const PID &master,
		      const std::vector<PID> &pids,
		      bool elect = false);

  virtual ~BasicMasterDetector();

  /**
   * @return unique id of the current master
   */
  virtual std::string getCurrentMasterId();

  /**
   * @return libprocess PID of the current master
   */
  virtual PID getCurrentMasterPID();

private:
  PID master;
};

}} /* namespace mesos { namespace internal { */

#endif /* __MASTER_DETECTOR_HPP__ */

