%module(directors="1") mesos

#define SWIG_NO_EXPORT_ITERATOR_METHODS

%{
#include <algorithm>
#include <stdexcept>

#include <mesos_sched.hpp>
#include <mesos_exec.hpp>

#define SWIG_STD_NOASSIGN_STL
%}

%include <stdint.i>
%include <std_map.i>
%include <std_string.i>
%include <std_vector.i>

#ifdef SWIGJAVA
  /* Wrap C++ enums using Java 1.5 enums instead of Java classes */
  %include <enums.swg>
  %javaconst(1);
  %insert("runtime") %{
  #define SWIG_JAVA_ATTACH_CURRENT_THREAD_AS_DAEMON
  %}
#endif

#ifdef SWIGJAVA
  /* Typemaps for vector<char> to convert it to a byte array */
  /* Based on a post at http://www.nabble.com/Swing-to-Java:-using-native-types-for-vector%3CT%3E-td22504981.html */
  %naturalvar mesos::data_string; 

  %typemap(jni) mesos::data_string "jbyteArray" 
  %typemap(jtype) mesos::data_string "byte[]" 
  %typemap(jstype) mesos::data_string "byte[]" 

  %typemap(out) mesos::data_string 
  %{ 
     $result = jenv->NewByteArray($1.size()); 
     jenv->SetByteArrayRegion($result, 0, $1.size(), (jbyte *) &$1[0]); 
  %} 

  %typemap(javaout) mesos::data_string 
  { 
    return $jnicall; 
  } 

  %typemap(jni) const mesos::data_string & "jbyteArray" 
  %typemap(jtype) const mesos::data_string & "byte[]" 
  %typemap(jstype) const mesos::data_string & "byte[]" 

  %typemap(in) const mesos::data_string & 
  %{ if(!$input) { 
       SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
         "null mesos::data_string"); 
       return $null; 
      } 
      const jsize $1_size = jenv->GetArrayLength($input); 
      jbyte *$1_ptr = jenv->GetByteArrayElements($input, NULL); 
      mesos::data_string $1_str((char *) $1_ptr, $1_size); 
      jenv->ReleaseByteArrayElements($input, $1_ptr, JNI_ABORT); 
      $1 = &$1_str; 
  %} 

  %typemap(javain) const mesos::data_string & "$javainput" 

  %typemap(out) const mesos::data_string & 
  %{ 
     $result = jenv->NewByteArray($1->size()); 
     jenv->SetByteArrayRegion($result, 0, $1->size(), (jbyte *) &(*$1)[0]); 
  %} 

  %typemap(javaout) const mesos::data_string & 
  { 
    return $jnicall; 
  } 

  /* Typemaps for MesosSchedulerDriver to keep a reference to the Scheduler */
  %typemap(javain) mesos::Scheduler* "getCPtrAndAddReference($javainput)"

  %typemap(javacode) mesos::MesosSchedulerDriver %{
    private static java.util.HashSet<Scheduler> schedulers =
      new java.util.HashSet<Scheduler>();

    private static long getCPtrAndAddReference(Scheduler scheduler) {
      synchronized (schedulers) {
        schedulers.add(scheduler);
      }
      return Scheduler.getCPtr(scheduler);
    }
  %}

  %typemap(javafinalize) mesos::MesosSchedulerDriver %{
    protected void finalize() {
      synchronized (schedulers) {
        schedulers.remove(getScheduler());
      }
      delete();
    }
  %}

  /* Typemaps for MesosExecutorDriver to keep a reference to the Executor */
  %typemap(javain) mesos::Executor* "getCPtrAndAddReference($javainput)"

  %typemap(javacode) mesos::MesosExecutorDriver %{
    private static java.util.HashSet<Executor> executors =
      new java.util.HashSet<Executor>();

    private static long getCPtrAndAddReference(Executor executor) {
      synchronized (executors) {
        executors.add(executor);
      }
      return Executor.getCPtr(executor);
    }
  %}

  %typemap(javafinalize) mesos::MesosExecutorDriver %{
    protected void finalize() {
      synchronized (executors) {
        executors.remove(getExecutor());
      }
      delete();
    }
  %}
#endif /* SWIGJAVA */

#ifdef SWIGPYTHON
  /* Add a reference to scheduler in the Python wrapper object to prevent it
     from being garbage-collected while the MesosSchedulerDriver exists */
  %feature("pythonappend") mesos::MesosSchedulerDriver::MesosSchedulerDriver %{
        self.scheduler = args[0]
  %}

  /* Add a reference to executor in the Python wrapper object to prevent it
     from being garbage-collected while the MesosExecutorDriver exists */
  %feature("pythonappend") mesos::MesosExecutorDriver::MesosExecutorDriver %{
        self.executor = args[0]
  %}
#endif /* SWIGPYTHON */

#ifdef SWIGRUBY
  /* Hide MesosSchedulerDriver::getScheduler because it would require
     object tracking to be turned on */
  %ignore mesos::MesosSchedulerDriver::getScheduler();
#endif /* SWIGRUBY */

/* Rename task_state enum so that the generated class is called TaskState */
%rename(TaskState) task_state;

/* Make it possible to inherit from Scheduler/Executor in target language */
%feature("director") mesos::Scheduler;
%feature("director") mesos::Executor;

/* Declare template instantiations we will use */
%template(SlaveOfferVector) std::vector<mesos::SlaveOffer>;
%template(TaskDescriptionVector) std::vector<mesos::TaskDescription>;
%template(StringMap) std::map<std::string, std::string>;

%include <mesos_types.h>
%include <mesos_types.hpp>
%include <mesos.hpp>
%include <mesos_sched.hpp>
%include <mesos_exec.hpp>
