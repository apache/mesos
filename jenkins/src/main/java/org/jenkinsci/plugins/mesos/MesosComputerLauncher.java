package org.jenkinsci.plugins.mesos;

import hudson.model.TaskListener;
import hudson.slaves.ComputerLauncher;
import hudson.slaves.SlaveComputer;

import java.io.PrintStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;
import java.util.logging.Logger;

import org.jenkinsci.plugins.mesos.Mesos.JenkinsSlave;

public class MesosComputerLauncher extends ComputerLauncher {

  enum State { INIT, RUNNING, FAILURE }

  private static final Logger LOGGER = Logger.getLogger(MesosComputerLauncher.class.getName());

  public MesosComputerLauncher(String _name) {
    super();
    LOGGER.info("Constructing MesosComputerLauncher");
    this.state = State.INIT;
    this.name = _name;
  }

  /**
   * Launches a mesos task that starts the jenkins slave.
   *
   * NOTE: This has to be a blocking call:
   *
   * @see hudson.slaves.ComputerLauncher#launch(hudson.slaves.SlaveComputer,
   *      hudson.model.TaskListener)
   */
  @Override
  public void launch(SlaveComputer _computer, TaskListener listener) throws InterruptedException {
    LOGGER.info("Launching slave computer " + name);

    MesosComputer computer = (MesosComputer) _computer;
    PrintStream logger = listener.getLogger();

    // Get a handle to mesos.
    Mesos mesos = Mesos.getInstance();

    // If Jenkins scheduler is not running, terminate the node.
    // This might happen if the computer was offline when Jenkins was shutdown.
    // Since Jenkins persists its state, it tries to launch offline slaves when
    // it restarts.
    if (!mesos.isSchedulerRunning()) {
      LOGGER.warning("Not launching " + name +
                     " because the Mesos Jenkins scheduler is not running");
      computer.getNode().terminate();
      return;
    }

    // Create the request.
    int numExecutors = computer.getNode().getNumExecutors();
    Mesos.SlaveRequest request = new Mesos.SlaveRequest(new JenkinsSlave(name), numExecutors);

    // Launch the jenkins slave.
    final Lock lock = new ReentrantLock();
    final CountDownLatch latch = new CountDownLatch(1);

    logger.println("Starting mesos slave " + name);
    LOGGER.info("Sending a request to start jenkins slave " + name);
    mesos.startJenkinsSlave(request, new Mesos.SlaveResult() {
      @Override
      public void running(JenkinsSlave slave) {
        state = State.RUNNING;
        latch.countDown();
      }

      @Override
      public void finished(JenkinsSlave slave) {
        state = State.FAILURE;
        latch.countDown();
      }

      @Override
      public void failed(JenkinsSlave slave) {
        state = State.FAILURE;
        latch.countDown();
      }
    });

    // Block until we know the status of the slave.
    // TODO(vinod): What happens if the callback is called again!
    latch.await();

    if (state == State.RUNNING) {
      logger.println("Successfully launched slave" + name);
    }

    LOGGER.info("Finished launching slave computer " + name);
  }

  /**
   * Kills the mesos task that corresponds to the Jenkins slave, asynchronously.
   */
  public void terminate() {
    // Get a handle to mesos.
    Mesos mesos = Mesos.getInstance();

    mesos.stopJenkinsSlave(name);
  }

  private volatile State state;
  private final String name;
}
