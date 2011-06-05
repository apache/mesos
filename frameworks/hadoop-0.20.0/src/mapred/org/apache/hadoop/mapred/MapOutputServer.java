package org.apache.hadoop.mapred;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URISyntaxException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;
import java.util.TreeMap;
import java.util.Vector;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.regex.Pattern;

import javax.servlet.ServletContext;
import javax.servlet.ServletException;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

import org.mortbay.jetty.Connector;
import org.mortbay.jetty.Handler;
import org.mortbay.jetty.Server;
import org.mortbay.jetty.handler.ContextHandlerCollection;
import org.mortbay.jetty.nio.SelectChannelConnector;
import org.mortbay.jetty.security.SslSocketConnector;
import org.mortbay.jetty.servlet.Context;
import org.mortbay.jetty.servlet.DefaultServlet;
import org.mortbay.jetty.servlet.FilterHolder;
import org.mortbay.jetty.servlet.FilterMapping;
import org.mortbay.jetty.servlet.ServletHandler;
import org.mortbay.jetty.servlet.ServletHolder;
import org.mortbay.jetty.webapp.WebAppContext;
import org.mortbay.thread.QueuedThreadPool;
import org.mortbay.util.MultiException;


import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.FSDataInputStream;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.LocalDirAllocator;
import org.apache.hadoop.fs.LocalFileSystem;
import org.apache.hadoop.fs.Path;

import org.apache.hadoop.util.StringUtils;


public class MapOutputServer implements MRConstants {

  public MapOutputServer() throws Exception {
    JobConf conf = new JobConf();

    Server server = new Server(60060);
    Context root = new Context(server, "/", Context.SESSIONS);

    root.setAttribute("conf", conf);
    root.setAttribute("indexCache", new IndexCache(conf));
    root.setAttribute("local.file.system", FileSystem.getLocal(conf));
    root.setAttribute("localDirAllocator",
		      new LocalDirAllocator("mapred.local.dir"));

    root.addServlet(new ServletHolder(new MapOutputServlet()), "/mapOutput");

    server.start();
  }

  /**
   * This class is used in TaskTracker's Jetty to serve the map outputs
   * to other nodes.
   */
  public static class MapOutputServlet extends HttpServlet {
    private static final int MAX_BYTES_TO_READ = 64 * 1024;
    @Override
    public void doGet(HttpServletRequest request, 
                      HttpServletResponse response
                      ) throws ServletException, IOException {
      String mapId = request.getParameter("map");
      String reduceId = request.getParameter("reduce");
      String jobId = request.getParameter("job");

      if (jobId == null) {
        throw new IOException("job parameter is required");
      }

      if (mapId == null || reduceId == null) {
        throw new IOException("map and reduce parameters are required");
      }
      ServletContext context = getServletContext();
      int reduce = Integer.parseInt(reduceId);
      byte[] buffer = new byte[MAX_BYTES_TO_READ];
      // true iff IOException was caused by attempt to access input
      boolean isInputException = true;
      OutputStream outStream = null;
      FSDataInputStream mapOutputIn = null;
 
      long totalRead = 0;

      try {
        outStream = response.getOutputStream();
        JobConf conf = (JobConf) context.getAttribute("conf");
        LocalDirAllocator lDirAlloc = 
          (LocalDirAllocator)context.getAttribute("localDirAllocator");
        FileSystem rfs = ((LocalFileSystem)
			  context.getAttribute("local.file.system")).getRaw();
	IndexCache indexCache = ((IndexCache)
				 context.getAttribute("indexCache"));

        // Index file
        Path indexFileName = lDirAlloc.getLocalPathToRead(
            TaskTracker.getIntermediateOutputDir(jobId, mapId)
            + "/file.out.index", conf);
        
        // Map-output file
        Path mapOutputFileName = lDirAlloc.getLocalPathToRead(
            TaskTracker.getIntermediateOutputDir(jobId, mapId)
            + "/file.out", conf);

        /**
         * Read the index file to get the information about where
         * the map-output for the given reducer is available. 
         */
       IndexRecord info =
	 indexCache.getIndexInformation(mapId, reduce,indexFileName);
          
        //set the custom "Raw-Map-Output-Length" http header to 
        //the raw (decompressed) length
        response.setHeader(RAW_MAP_OUTPUT_LENGTH,
            Long.toString(info.rawLength));

        //set the custom "Map-Output-Length" http header to 
        //the actual number of bytes being transferred
        response.setHeader(MAP_OUTPUT_LENGTH,
            Long.toString(info.partLength));

        //use the same buffersize as used for reading the data from disk
        response.setBufferSize(MAX_BYTES_TO_READ);
        
        /**
         * Read the data from the sigle map-output file and
         * send it to the reducer.
         */
        //open the map-output file
        mapOutputIn = rfs.open(mapOutputFileName);

        //seek to the correct offset for the reduce
        mapOutputIn.seek(info.startOffset);
        long rem = info.partLength;
        int len =
          mapOutputIn.read(buffer, 0, (int)Math.min(rem, MAX_BYTES_TO_READ));
        while (rem > 0 && len >= 0) {
          rem -= len;
          try {
            outStream.write(buffer, 0, len);
            outStream.flush();
          } catch (IOException ie) {
            isInputException = false;
            throw ie;
          }
          totalRead += len;
          len =
            mapOutputIn.read(buffer, 0, (int)Math.min(rem, MAX_BYTES_TO_READ));
        }

	System.out.println("Sent out " + totalRead + " bytes for reduce: " +
			   reduce + " from map: " + mapId + " given " +
			   info.partLength + "/" + info.rawLength);
      } catch (IOException ie) {
        String errorMsg = ("getMapOutput(" + mapId + "," + reduceId + 
                           ") failed :\n"+
                           StringUtils.stringifyException(ie));
        if (isInputException) {
	  System.out.println("******************************************");
	  System.out.print("MESOS PORT DOES NOT HANDLE THIS ERROR!");
	  System.out.println("******************************************");
          //tracker.mapOutputLost(TaskAttemptID.forName(mapId), errorMsg);
        }
        response.sendError(HttpServletResponse.SC_GONE, errorMsg);
        throw ie;
      } finally {
        if (null != mapOutputIn) {
          mapOutputIn.close();
        }
      }
      outStream.close();
    }
  }

  public static void main(String argv[]) throws Exception {
    if (argv.length != 0) {
      System.out.println("usage: MapOutputServer");
      System.exit(-1);
    }
    new MapOutputServer();
  }
}
