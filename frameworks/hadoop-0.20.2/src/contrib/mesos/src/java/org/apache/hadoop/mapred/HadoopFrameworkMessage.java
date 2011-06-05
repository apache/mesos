package org.apache.hadoop.mapred;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;

public class HadoopFrameworkMessage {
  enum Type {
    S2E_SEND_STATUS_UPDATE, // Used by scheduler to ask executor to send a Mesos
                            // status update for a given task
    S2E_SHUTDOWN_EXECUTOR,  // Used by the scheduler to ask executor to shutdown
                            // (so that we can clean up TaskTrackers when idle)
    E2S_KILL_REQUEST,       // Used by executor to report a killTask from Mesos
  }
  
  Type type;
  String arg1;
  String arg2;
  

  public HadoopFrameworkMessage(Type type, String arg1, String arg2) {
    this.type = type;
    this.arg1 = arg1;
    this.arg2 = arg2;
  }
  
  public HadoopFrameworkMessage(Type type, String arg1) {
    this(type, arg1, "");
  }

  public HadoopFrameworkMessage(byte[] bytes) throws IOException {
    DataInputStream in = new DataInputStream(new ByteArrayInputStream(bytes));
    String typeStr = in.readUTF();
    try {
      type = Type.valueOf(typeStr);
    } catch(IllegalArgumentException e) {
      throw new IOException("Unknown message type: " + typeStr);
    }
    arg1 = in.readUTF();
    arg2 = in.readUTF();
  }
  
  public byte[] serialize() throws IOException {
    ByteArrayOutputStream bos = new ByteArrayOutputStream();
    DataOutputStream dos = new DataOutputStream(bos);
    dos.writeUTF(type.toString());
    dos.writeUTF(arg1);
    dos.writeUTF(arg2);
    return bos.toByteArray();
  }
}
