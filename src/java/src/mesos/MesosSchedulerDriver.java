package mesos;

import mesos.Protos.*;

import java.util.Collection;
import java.util.Collections;
import java.util.Map;


/**
 * Concrete implementation of SchedulerDriver that communicates with
 * a Mesos master.
 */
public class MesosSchedulerDriver implements SchedulerDriver {
  static {
    System.loadLibrary("mesos");
  }

  public MesosSchedulerDriver(Scheduler sched, String url, FrameworkID frameworkId) {
    this.sched = sched;
    this.url = url;
    this.frameworkId = frameworkId;

    initialize();
  }

  public MesosSchedulerDriver(Scheduler sched, String url) {
    this(sched, url, FrameworkID.newBuilder().setValue("").build());
  }

  public native int start();
  public native int stop();
  public native int join();

  public int run() {
    int ret = start();
    return ret != 0 ? ret : join();
  }

  public native int sendFrameworkMessage(FrameworkMessage message);
  public native int killTask(TaskID taskId);
  public native int replyToOffer(OfferID offerId, Collection<TaskDescription> tasks, Map<String, String> params);

  public int replyToOffer(OfferID offerId, Collection<TaskDescription> tasks) {
    return replyToOffer(offerId, tasks, Collections.<String, String>emptyMap());
  }

  public native int reviveOffers();

  protected native void initialize();
  protected native void finalize();

  private final Scheduler sched;
  private final String url;
  private final FrameworkID frameworkId;

  private long __sched;
  private long __driver;
};
