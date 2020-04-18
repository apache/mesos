# BUILD JAVA ARTIFACTS.
#######################
# Build the Mesos JAR.
#
# NOTE: We do not utilize CMake's `UseJava` module for a few reasons:
#   (1) `add_jar()` calls `javac`, but we use Maven.
#   (2) `add_jar()` does not support something like `IMPORTED`.
#   (3) `create_javah()` checks the existence of each JAR in the given class
#       path at configuration time, even though it depends on `mesos-jar` to
#       run Maven at build time. Coupled with (1) and (2) this makes it
#       impossible to use.
set(MESOS_JAR ${CMAKE_CURRENT_BINARY_DIR}/target/mesos-${MESOS_PACKAGE_VERSION}.jar)
set(PROTOBUF_JAR ${CMAKE_CURRENT_BINARY_DIR}/target/dependency/protobuf-java-${PROTOBUF_VERSION}.jar)
set(MESOS_SHADED_PROTOBUF_JAR  ${CMAKE_CURRENT_BINARY_DIR}/target/mesos-${PACKAGE_VERSION}-shaded-protobuf.jar)

# This file is generated into the build tree.
set(MESOS_JAVA_LIBRARY generated/org/apache/mesos/MesosNativeLibrary.java)
configure_file(${MESOS_JAVA_LIBRARY}.in ${MESOS_JAVA_LIBRARY})

set(MESOS_JAR_SRC
  src/org/apache/mesos/ExecutorDriver.java
  src/org/apache/mesos/Executor.java
  src/org/apache/mesos/Log.java
  src/org/apache/mesos/MesosExecutorDriver.java
  src/org/apache/mesos/MesosSchedulerDriver.java
  src/org/apache/mesos/SchedulerDriver.java
  src/org/apache/mesos/Scheduler.java
  src/org/apache/mesos/state/AbstractState.java
  src/org/apache/mesos/state/InMemoryState.java
  src/org/apache/mesos/state/LevelDBState.java
  src/org/apache/mesos/state/LogState.java
  src/org/apache/mesos/state/State.java
  src/org/apache/mesos/state/Variable.java
  src/org/apache/mesos/state/ZooKeeperState.java
  src/org/apache/mesos/v1/scheduler/V1Mesos.java
  src/org/apache/mesos/v1/scheduler/Mesos.java
  src/org/apache/mesos/v1/scheduler/Scheduler.java
  src/org/apache/mesos/v1/scheduler/V0Mesos.java
  ${CMAKE_CURRENT_BINARY_DIR}/${MESOS_JAVA_LIBRARY})

configure_file(mesos.pom.in mesos.pom)

add_custom_command(
  COMMENT "Building ${MESOS_JAR} using Maven..."
  OUTPUT ${MESOS_JAR}
  COMMAND mvn -B -f mesos.pom clean package
  DEPENDS ${MESOS_JAR_SRC} ${JAVA_PROTOBUF_SRC})

add_custom_target(mesos-jar DEPENDS ${MESOS_JAR} mesos-protobufs)

# TODO(andschwa): Build the Example JAR.
set(EXAMPLE_JAR_SRC
  ../examples/java/TestExceptionFramework.java
  ../examples/java/TestExecutor.java
  ../examples/java/TestFramework.java
  ../examples/java/TestLog.java
  ../examples/java/TestMultipleExecutorsFramework.java
  ../examples/java/V1TestFramework.java)

set(JAVA_SRC
  jni/convert.cpp
  jni/construct.cpp
  jni/org_apache_mesos_Log.cpp
  jni/org_apache_mesos_MesosExecutorDriver.cpp
  jni/org_apache_mesos_MesosNativeLibrary.cpp
  jni/org_apache_mesos_MesosSchedulerDriver.cpp
  jni/org_apache_mesos_state_AbstractState.cpp
  jni/org_apache_mesos_state_LevelDBState.cpp
  jni/org_apache_mesos_state_LogState.cpp
  jni/org_apache_mesos_state_Variable.cpp
  jni/org_apache_mesos_state_ZooKeeperState.cpp
  jni/org_apache_mesos_v1_scheduler_V1Mesos.cpp
  jni/org_apache_mesos_v1_scheduler_V0Mesos.cpp
  ../jvm/jvm.cpp
  ../jvm/org/apache/log4j.cpp
  ../jvm/org/apache/zookeeper.cpp)

# These are generated and so must be included.
set(JAVA_H
  jni/org_apache_mesos_Log.h
  jni/org_apache_mesos_MesosExecutorDriver.h
  jni/org_apache_mesos_MesosSchedulerDriver.h
  jni/org_apache_mesos_state_AbstractState.h
  jni/org_apache_mesos_state_LevelDBState.h
  jni/org_apache_mesos_state_LogState.h
  jni/org_apache_mesos_state_Variable.h
  jni/org_apache_mesos_state_ZooKeeperState.h
  jni/org_apache_mesos_v1_scheduler_V1Mesos.h
  jni/org_apache_mesos_v1_scheduler_V0Mesos.h)

set(JAVA_CLASSES
  org.apache.mesos.Log
  org.apache.mesos.MesosExecutorDriver
  org.apache.mesos.MesosSchedulerDriver
  org.apache.mesos.state.AbstractState
  org.apache.mesos.state.LevelDBState
  org.apache.mesos.state.LogState
  org.apache.mesos.state.Variable
  org.apache.mesos.state.ZooKeeperState
  org.apache.mesos.v1.scheduler.V1Mesos
  org.apache.mesos.v1.scheduler.V0Mesos)

# This generates the header files.
if (WIN32)
  set(COLON ";")
else ()
  set(COLON ":")
endif ()

add_custom_command(
  OUTPUT ${JAVA_H}
  COMMAND ${Java_JAVAH_EXECUTABLE} -d jni -classpath "${MESOS_JAR}${COLON}${PROTOBUF_JAR}" ${JAVA_CLASSES}
  DEPENDS mesos-jar make_bin_jni_dir)

# We include the headers here to establish the dependency
# on the above custom command.
add_library(mesos-java ${JAVA_SRC} ${JAVA_H})

target_link_libraries(
  mesos-java PUBLIC
  mesos
  mesos-protobufs
  process
  zookeeper
  ${JNI_LIBRARIES})

target_include_directories(
  mesos-java PUBLIC
  ${JNI_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}/jni
  ${MESOS_PUBLIC_INCLUDE_DIR})

install(
  TARGETS mesos-java
  RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME}
  LIBRARY DESTINATION ${MESOS_INSTALL_LIBRARIES}
  ARCHIVE DESTINATION ${MESOS_INSTALL_LIBRARIES})

# NOTE: Currently, the automake build installs Java packages differently.
# Namely, `make install` doesn't install Java packages at all, and
# `make maven-install` calls `mvn install` for package installation.
install(
  FILES ${MESOS_JAR} ${MESOS_SHADED_PROTOBUF_JAR}
  DESTINATION ${MESOS_INSTALL_LIBRARIES})
