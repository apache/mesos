import java.io.File;
import java.util.*;

import nexus.*;

public class DaemonScheduler extends Scheduler {

  final static String executorScript =
    "/Users/benh/nexus-github-master/frameworks/daemon/daemon_executor";

  static Map<String, Configuration> configs =
    new HashMap<String, Configuration>();

  static Map<String, Set<TaskDescription>> running =
    new HashMap<String, Set<TaskDescription>>();

  int taskId = 0;

  static class Configuration {
    public int cpus; // in cores
    public int mem; // in bytes
    public int instances; // to launch
    Configuration(int cpus, int mem, int instances) {
      this.cpus = cpus;
      this.mem = mem;
      this.instances = instances;
    }
  }

  static {
    System.loadLibrary("nexus");

    configs.put("talon", new Configuration(1, 1*1024*1024*1024, 1));
    configs.put("puffin", new Configuration(1, 1*1024*1024*1024, 1));
  }

  public String getFrameworkName(SchedulerDriver driver) {
    return "DaemonScheduler";
  }

  public ExecutorInfo getExecutorInfo(SchedulerDriver driver) {
    return new ExecutorInfo(executorScript, new byte[0]);
  }

  public void registered(SchedulerDriver driver, int fid) {
    System.out.println("Registered with ID " + fid);
    // Register with ZooKeeper
  }

  public void resourceOffer(SchedulerDriver driver, long oid,
			    SlaveOfferVector offers) {
    try {
      // Tasks to launch.
      TaskDescriptionVector tasks = new TaskDescriptionVector();

      for (int i = 0; i < offers.size(); i++) {
	SlaveOffer offer = offers.get(i);
	System.out.println("Trying offer from " + offer.getSlaveId());
	for (Map.Entry<String, Configuration> entry : configs.entrySet()) {
	  String name = entry.getKey();
	  Configuration config = entry.getValue();
	  if (!running.containsKey(name) ||
	      running.get(name).size() < config.instances) {
	    // Need another instance ...
	    if (config.cpus >= Integer.parseInt(offer.getParams().get("cpus")) &&
		config.mem >= Integer.parseInt(offer.getParams().get("mem"))) {
	      // This offer has enough resources ...
	      StringMap params = new StringMap();
	      params.set("cpus", offer.getParams().get("cpus"));
	      params.set("mem", offer.getParams().get("mem"));

	      TaskDescription task = 
		new TaskDescription(taskId, offer.getSlaveId(),
				    name + "-" + taskId++,
				    params, new byte[0]);

	      tasks.add(task);

	      if (!running.containsKey(name)) {
		running.put(name, new HashSet<TaskDescription>());
	      }

	      running.get(name).add(task);
	      break;
	    }
	  }
	}
      }
      driver.replyToOffer(oid, tasks, new StringMap());
    } catch (Exception e) {
      e.printStackTrace();
    }
  }

  public void statusUpdate(SchedulerDriver driver, TaskStatus status) {
    try {
      for (Map.Entry<String, Set<TaskDescription>> entry : running.entrySet()) {
	Set<TaskDescription> tasks = entry.getValue();
	for (TaskDescription task : tasks) {
	  if (task.getTaskId() == status.getTaskId()) {
	    tasks.remove(task);
	  }
	}
      }
    } catch (Exception e) {
      e.printStackTrace();
    }
  }

  public void error(SchedulerDriver driver, int code, String message) {
    // TODO(*): Implement this ... to catch for lost executors, etc.
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 1) {
      System.err.println("usage: <master>");
      System.exit(1);
    }
    DaemonScheduler sched = new DaemonScheduler();
    NexusSchedulerDriver driver = new NexusSchedulerDriver(sched, args[0]);
    driver.run();
  }

}