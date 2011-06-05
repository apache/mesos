%module(directors="1") nexus

#define SWIG_NO_EXPORT_ITERATOR_METHODS

%{
#include <algorithm>
#include <stdexcept>

#include <nexus_sched.hpp>
#include <nexus_exec.hpp>

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
  %naturalvar nexus::data_string; 

  %typemap(jni) nexus::data_string "jbyteArray" 
  %typemap(jtype) nexus::data_string "byte[]" 
  %typemap(jstype) nexus::data_string "byte[]" 

  %typemap(out) nexus::data_string 
  %{ 
     $result = jenv->NewByteArray($1.size()); 
     jenv->SetByteArrayRegion($result, 0, $1.size(), (jbyte *) &$1[0]); 
  %} 

  %typemap(javaout) nexus::data_string 
  { 
    return $jnicall; 
  } 

  %typemap(jni) const nexus::data_string & "jbyteArray" 
  %typemap(jtype) const nexus::data_string & "byte[]" 
  %typemap(jstype) const nexus::data_string & "byte[]" 

  %typemap(in) const nexus::data_string & 
  %{ if(!$input) { 
       SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
         "null nexus::data_string"); 
       return $null; 
      } 
      const jsize $1_size = jenv->GetArrayLength($input); 
      jbyte *$1_ptr = jenv->GetByteArrayElements($input, NULL); 
      nexus::data_string $1_str((char *) $1_ptr, $1_size); 
      jenv->ReleaseByteArrayElements($input, $1_ptr, JNI_ABORT); 
      $1 = &$1_str; 
  %} 

  %typemap(javain) const nexus::data_string & "$javainput" 

  %typemap(out) const nexus::data_string & 
  %{ 
     $result = jenv->NewByteArray($1->size()); 
     jenv->SetByteArrayRegion($result, 0, $1->size(), (jbyte *) &(*$1)[0]); 
  %} 

  %typemap(javaout) const nexus::data_string & 
  { 
    return $jnicall; 
  } 

  /* Typemap for NexusSchedulerDriver to keep a reference to the Scheduler */
  %typemap(javain) nexus::Scheduler* "getCPtrAndAddReference($javainput)"

  %typemap(javacode) nexus::NexusSchedulerDriver %{
    private static java.util.HashSet<Scheduler> schedulers =
      new java.util.HashSet<Scheduler>();

    private static long getCPtrAndAddReference(Scheduler scheduler) {
      synchronized (schedulers) {
        schedulers.add(scheduler);
      }
      return Scheduler.getCPtr(scheduler);
    }
  %}

  %typemap(javafinalize) nexus::NexusSchedulerDriver %{
    protected void finalize() {
      synchronized (schedulers) {
        schedulers.remove(getScheduler());
      }
      delete();
    }
  %}
#endif /* SWIGJAVA */

#ifdef SWIGPYTHON
  /* Add a reference to scheduler in the Python wrapper object to prevent it
     from being garbage-collected while the NexusSchedulerDriver exists */
  %feature("pythonappend") nexus::NexusSchedulerDriver::NexusSchedulerDriver %{
        self.scheduler = args[0]
  %}
#endif

#ifdef SWIGRUBY
  /* Hide NexusSchedulerDriver::getScheduler because it would require
     object tracking to be turned on */
  %ignore nexus::NexusSchedulerDriver::getScheduler();
#endif /* SWIGRUBY */

/* Rename task_state enum so that the generated class is called TaskState */
%rename(TaskState) task_state;

/* Make it possible to inherit from Scheduler/Executor in target language */
%feature("director") nexus::Scheduler;
%feature("director") nexus::Executor;

/* Declare template instantiations we will use */
%template(SlaveOfferVector) std::vector<nexus::SlaveOffer>;
%template(TaskDescriptionVector) std::vector<nexus::TaskDescription>;
%template(StringMap) std::map<std::string, std::string>;

%include <nexus_types.h>
%include <nexus_types.hpp>
%include <nexus.hpp>
%include <nexus_sched.hpp>
%include <nexus_exec.hpp>
