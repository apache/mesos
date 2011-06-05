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

  /**
   * Add a utility method for finding a class even from new Java threads.
   * Unfortunately, JNI's FindClass uses the system ClassLoader when it
   * is called from a C++ thread, but in Scala (and probably other Java
   * environments too), this classloader is not enough to locate mesos.jar.
   * Instead, we try to capture Thread.currentThread()'s context ClassLoader
   * when the Mesos library is initialized, in case it has more paths that
   * we can search.
   *
   * This code is based on Apache 2 licensed Android code obtained from
   * http://android.git.kernel.org/?p=platform/frameworks/base.git;a=blob;f=core/jni/AndroidRuntime.cpp;h=f61e2476c71191aa6eabc93bcb26b3c15ccf6136;hb=HEAD
   */
  %insert("runtime") %{
     #include <string>
     #include <assert.h>

     jweak mesosClassLoader = NULL;


     JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* jvm, void* reserved)
     {
       // Grab the context ClassLoader of the current thread, if any

       JNIEnv* env;
       if (jvm->GetEnv((void **)&env, JNI_VERSION_1_2)) {
         return JNI_ERR; /* JNI version not supported */
       }

       jclass javaLangThread, javaLangClassLoader;
       jmethodID currentThread, getContextClassLoader, loadClass;
       jobject thread, classLoader;

       /* find this thread's context class loader; none of this is expected to fail */
       javaLangThread = env->FindClass("java/lang/Thread");
       assert(javaLangThread != NULL);
       javaLangClassLoader = env->FindClass("java/lang/ClassLoader");
       assert(javaLangClassLoader != NULL);
       currentThread = env->GetStaticMethodID(javaLangThread,
           "currentThread", "()Ljava/lang/Thread;");
       getContextClassLoader = env->GetMethodID(javaLangThread,
           "getContextClassLoader", "()Ljava/lang/ClassLoader;");
       assert(currentThread != NULL);
       assert(getContextClassLoader != NULL);
       thread = env->CallStaticObjectMethod(javaLangThread, currentThread);
       assert(thread != NULL);
       classLoader = env->CallObjectMethod(thread, getContextClassLoader);
       if (classLoader != NULL) {
         mesosClassLoader = env->NewWeakGlobalRef(classLoader);
       }
 
       return JNI_VERSION_1_2;
     }


     JNIEXPORT void JNICALL JNI_OnUnLoad(JavaVM* jvm, void* reserved)
     {
       JNIEnv *env;
       if (jvm->GetEnv((void **)&env, JNI_VERSION_1_2)) {
           return;
       }
       if (mesosClassLoader != NULL) {
         env->DeleteWeakGlobalRef(mesosClassLoader);
         mesosClassLoader = NULL;
       }
     }


     jclass FindClassWithMesosClassLoader(JNIEnv* env, const char* className)
     {
         if (env->ExceptionCheck()) {
             fprintf(stderr, "ERROR: exception pending on entry to "
                             "FindClassWithMesosClassLoader()\n");
             return NULL;
         }

         if (mesosClassLoader == NULL) {
           return env->FindClass(className);
         }
     
         /*
          * JNI FindClass uses class names with slashes, but ClassLoader.loadClass
          * uses the dotted "binary name" format. Convert formats.
          */
         std::string convName = className;
         for (int i = 0; i < convName.size(); i++) {
           if (convName[i] == '/')
             convName[i] = '.';
         }

         jclass javaLangClassLoader = env->FindClass("java/lang/ClassLoader");
         assert(javaLangClassLoader != NULL);
         jmethodID loadClass = env->GetMethodID(javaLangClassLoader,
             "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
         assert(loadClass != NULL);
         jclass cls = NULL;

         /* create an object for the class name string; alloc could fail */
         jstring strClassName = env->NewStringUTF(convName.c_str());
         if (env->ExceptionCheck()) {
             fprintf(stderr, "ERROR: unable to convert '%s' to string\n", convName.c_str());
             goto bail;
         }

         /* try to find the named class */
         cls = (jclass) env->CallObjectMethod(mesosClassLoader, loadClass,
                                              strClassName);
         if (env->ExceptionCheck()) {
             fprintf(stderr, "ERROR: unable to load class '%s' from %p\n",
                 className, mesosClassLoader);
             cls = NULL;
             goto bail;
         }

     bail:
         return cls;
     }
  %}

  /* Typemaps for vector<char> to map it to a byte array */
  /* Based on a post at http://www.nabble.com/Swing-to-Java:-using-native-types-for-vector%3CT%3E-td22504981.html */
  %naturalvar mesos::bytes; 

  %typemap(jni) mesos::bytes "jbyteArray" 
  %typemap(jtype) mesos::bytes "byte[]" 
  %typemap(jstype) mesos::bytes "byte[]" 

  %typemap(out) mesos::bytes 
  %{ 
     $result = jenv->NewByteArray($1.size()); 
     jenv->SetByteArrayRegion($result, 0, $1.size(), (jbyte *) &$1[0]); 
  %} 

  %typemap(javaout) mesos::bytes 
  { 
    return $jnicall; 
  } 

  %typemap(jni) const mesos::bytes & "jbyteArray" 
  %typemap(jtype) const mesos::bytes & "byte[]" 
  %typemap(jstype) const mesos::bytes & "byte[]" 

  %typemap(in) const mesos::bytes & 
  %{ if(!$input) { 
       SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
         "null mesos::bytes"); 
       return $null; 
      } 
      const jsize $1_size = jenv->GetArrayLength($input); 
      jbyte *$1_ptr = jenv->GetByteArrayElements($input, NULL); 
      mesos::bytes $1_str((char *) $1_ptr, $1_size); 
      jenv->ReleaseByteArrayElements($input, $1_ptr, JNI_ABORT); 
      $1 = &$1_str; 
  %} 

  %typemap(javain) const mesos::bytes & "$javainput" 

  %typemap(out) const mesos::bytes & 
  %{ 
     $result = jenv->NewByteArray($1->size()); 
     jenv->SetByteArrayRegion($result, 0, $1->size(), (jbyte *) &(*$1)[0]); 
  %} 

  %typemap(javaout) const mesos::bytes & 
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


  /* Typemaps for vector<SlaveOffer> to map it to a java.util.List */
  %naturalvar std::vector<mesos::SlaveOffer>;

  %typemap(jni) const std::vector<mesos::SlaveOffer> & "jobject"
  %typemap(jtype) const std::vector<mesos::SlaveOffer> & "java.util.List<SlaveOffer>"
  %typemap(jstype) const std::vector<mesos::SlaveOffer> & "java.util.List<SlaveOffer>"
  %typemap(javadirectorin) const std::vector<mesos::SlaveOffer> & "$jniinput"
  %typemap(javadirectorout) const std::vector<mesos::SlaveOffer> & "$javacall"

  %typemap(in) const std::vector<mesos::SlaveOffer> &
  %{
     std::vector<mesos::SlaveOffer> $1_vec;
     {
     if(!$input) {
      SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
        "null std::vector<mesos::SlaveOffer>");
      return $null;
     }
     jclass listCls = jenv->GetObjectClass($input);
     jmethodID iterator = jenv->GetMethodID(listCls, "iterator", "()Ljava/util/Iterator;");
     jobject iterObj = jenv->CallObjectMethod($input, iterator);
     jclass iterCls = jenv->GetObjectClass(iterObj);
     jmethodID hasNext = jenv->GetMethodID(iterCls, "hasNext", "()Z");
     jmethodID next = jenv->GetMethodID(iterCls, "next", "()Ljava/lang/Object;");
     jclass taskDescCls = FindClassWithMesosClassLoader(jenv, "mesos/SlaveOffer");
     jmethodID getCPtr = jenv->GetStaticMethodID(taskDescCls, "getCPtr", "(Lmesos/SlaveOffer;)J");
     while (jenv->CallBooleanMethod(iterObj, hasNext)) {
       jobject obj = jenv->CallObjectMethod(iterObj, next);
       jlong offerPtr = jenv->CallStaticLongMethod(taskDescCls, getCPtr, obj);
       $1_vec.push_back(*((mesos::SlaveOffer*) offerPtr));
       jenv->DeleteLocalRef(obj); // Recommended in case list is big and fills local ref table
     }
     $1 = &$1_vec;
  } %}

  %typemap(javain) const std::vector<mesos::SlaveOffer> & "$javainput"

  %typemap(directorin,descriptor="Ljava/util/List;") const std::vector<mesos::SlaveOffer> &
  %{ {
     jclass listCls = FindClassWithMesosClassLoader(jenv, "java/util/ArrayList");
     jmethodID listCtor = jenv->GetMethodID(listCls, "<init>", "()V");
     jmethodID add = jenv->GetMethodID(listCls, "add", "(Ljava/lang/Object;)Z");
     jobject list = jenv->NewObject(listCls, listCtor);
     jclass taskDescCls = FindClassWithMesosClassLoader(jenv, "mesos/SlaveOffer");
if (jenv->ExceptionOccurred()) {
jenv->ExceptionDescribe();
}
     jmethodID taskDescCtor = jenv->GetMethodID(taskDescCls, "<init>", "(JZ)V");
     for (int i = 0; i < $1.size(); i++) {
       // TODO: Copy the SlaveOffer object here so Java owns it?
       jlong ptr;
       *(const mesos::SlaveOffer **)&ptr = &($1.at(i));
       jobject obj = jenv->NewObject(taskDescCls, taskDescCtor, ptr, JNI_FALSE);
       jenv->CallVoidMethod(list, add, obj);
       jenv->DeleteLocalRef(obj); // Recommended in case list is big and fills local ref table
     }
     $input = list;
  } %}

  %typemap(out) const std::vector<mesos::SlaveOffer> &
  %{ {
     jclass listCls = FindClassWithMesosClassLoader(jenv, "java/util/ArrayList");
     jmethodID listCtor = jenv->GetMethodID(listCls, "<init>", "()V");
     jmethodID add = jenv->GetMethodID(listCls, "add", "(Ljava/lang/Object;)Z");
     jobject list = jenv->NewObject(listCls, listCtor);
     jclass taskDescCls = FindClassWithMesosClassLoader(jenv, "mesos/SlaveOffer");
     jmethodID taskDescCtor = jenv->GetMethodID(taskDescCls, "<init>", "(JZ)V");
     for (int i = 0; i < $1.size(); i++) {
       // TODO: Copy the SlaveOffer object here so Java owns it?
       jlong ptr;
       *(const mesos::SlaveOffer **)&ptr = &($1.at(i));
       jobject obj = jenv->NewObject(taskDescCls, taskDescCtor, ptr, JNI_FALSE);
       jenv->CallVoidMethod(list, add, obj);
       jenv->DeleteLocalRef(obj); // Recommended in case list is big and fills local ref table
     }
     $result = list;
  } %}

  %typemap(javaout) const std::vector<mesos::SlaveOffer> &
  {
    return $jnicall;
  }


  /* Typemaps for vector<TaskDescription> to map it to a java.util.List */
  %naturalvar std::vector<mesos::TaskDescription>;

  %typemap(jni) const std::vector<mesos::TaskDescription> & "jobject"
  %typemap(jtype) const std::vector<mesos::TaskDescription> & "java.util.List<TaskDescription>"
  %typemap(jstype) const std::vector<mesos::TaskDescription> & "java.util.List<TaskDescription>"
  %typemap(javadirectorin) const std::vector<mesos::TaskDescription> & "$jniinput"
  %typemap(javadirectorout) const std::vector<mesos::TaskDescription> & "$javacall"

  %typemap(in) const std::vector<mesos::TaskDescription> &
  %{
     std::vector<mesos::TaskDescription> $1_vec;
     {
     if(!$input) {
      SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
        "null std::vector<mesos::TaskDescription>");
      return $null;
     }
     jclass listCls = jenv->GetObjectClass($input);
     jmethodID iterator = jenv->GetMethodID(listCls, "iterator", "()Ljava/util/Iterator;");
     jobject iterObj = jenv->CallObjectMethod($input, iterator);
     jclass iterCls = jenv->GetObjectClass(iterObj);
     jmethodID hasNext = jenv->GetMethodID(iterCls, "hasNext", "()Z");
     jmethodID next = jenv->GetMethodID(iterCls, "next", "()Ljava/lang/Object;");
     jclass taskDescCls = FindClassWithMesosClassLoader(jenv, "mesos/TaskDescription");
     jmethodID getCPtr = jenv->GetStaticMethodID(taskDescCls, "getCPtr", "(Lmesos/TaskDescription;)J");
     while (jenv->CallBooleanMethod(iterObj, hasNext)) {
       jobject obj = jenv->CallObjectMethod(iterObj, next);
       jlong offerPtr = jenv->CallStaticLongMethod(taskDescCls, getCPtr, obj);
       $1_vec.push_back(*((mesos::TaskDescription*) offerPtr));
       jenv->DeleteLocalRef(obj); // Recommended in case list is big and fills local ref table
     }
     $1 = &$1_vec;
  } %}

  %typemap(javain) const std::vector<mesos::TaskDescription> & "$javainput"

  %typemap(directorin,descriptor="Ljava/util/List;") const std::vector<mesos::TaskDescription> &
  %{ {
     jclass listCls = FindClassWithMesosClassLoader(jenv, "java/util/ArrayList");
     jmethodID listCtor = jenv->GetMethodID(listCls, "<init>", "()V");
     jmethodID add = jenv->GetMethodID(listCls, "add", "(Ljava/lang/Object;)V");
     jobject list = jenv->NewObject(listCls, listCtor);
     jclass taskDescCls = FindClassWithMesosClassLoader(jenv, "mesos/TaskDescription");
     jmethodID taskDescCtor = jenv->GetMethodID(taskDescCls, "<init>", "(JZ)V");
     for (int i = 0; i < $1.size(); i++) {
       // TODO: Copy the TaskDescription object here so Java owns it?
       jlong ptr;
       *(const mesos::TaskDescription **)&ptr = &($1.at(i));
       jobject obj = jenv->NewObject(taskDescCls, taskDescCtor, ptr, JNI_FALSE);
       jenv->CallVoidMethod(list, add, obj);
       jenv->DeleteLocalRef(obj); // Recommended in case list is big and fills local ref table
     }
     $input = list;
  } %}

  %typemap(out) const std::vector<mesos::TaskDescription> &
  %{ {
     jclass listCls = FindClassWithMesosClassLoader(jenv, "java/util/ArrayList");
     jmethodID listCtor = jenv->GetMethodID(listCls, "<init>", "()V");
     jmethodID add = jenv->GetMethodID(listCls, "add", "(Ljava/lang/Object;)V");
     jobject list = jenv->NewObject(listCls, listCtor);
     jclass taskDescCls = FindClassWithMesosClassLoader(jenv, "mesos/TaskDescription");
     jmethodID taskDescCtor = jenv->GetMethodID(taskDescCls, "<init>", "(JZ)V");
     for (int i = 0; i < $1.size(); i++) {
       // TODO: Copy the TaskDescription object here so Java owns it?
       jlong ptr;
       *(const mesos::TaskDescription **)&ptr = &($1.at(i));
       jobject obj = jenv->NewObject(taskDescCls, taskDescCtor, ptr, JNI_FALSE);
       jenv->CallVoidMethod(list, add, obj);
       jenv->DeleteLocalRef(obj); // Recommended in case list is big and fills local ref table
     }
     $result = list;
  } %}

  %typemap(javaout) const std::vector<mesos::TaskDescription> &
  {
    return $jnicall;
  }


  /* Typemaps for map<string, string> to map it to a java.util.Map */
  %naturalvar std::map<std::string, std::string>;

  %typemap(jni) const std::map<std::string, std::string> & "jobject"
  %typemap(jtype) const std::map<std::string, std::string> & "java.util.Map<String, String>"
  %typemap(jstype) const std::map<std::string, std::string> & "java.util.Map<String, String>"
  %typemap(javadirectorin) const std::map<std::string, std::string> & "$jniinput"
  %typemap(javadirectorout) const std::map<std::string, std::string> & "$javacall"

  %typemap(in) const std::map<std::string, std::string> &
  %{
     std::map<std::string, std::string> $1_map;
     {
     if(!$input) {
      SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
        "null std::map<std::string, std::string>");
      return $null;
     }
     jclass mapCls = jenv->GetObjectClass($input);
     jmethodID entrySet = jenv->GetMethodID(mapCls, "entrySet", "()Ljava/util/Set;");
     jobject entriesObj = jenv->CallObjectMethod($input, entrySet);
     jclass entriesCls = jenv->GetObjectClass(entriesObj);
     jmethodID iterator = jenv->GetMethodID(entriesCls, "iterator", "()Ljava/util/Iterator;");
     jobject iterObj = jenv->CallObjectMethod(entriesObj, iterator);
     jclass iterCls = jenv->GetObjectClass(iterObj);
     jmethodID hasNext = jenv->GetMethodID(iterCls, "hasNext", "()Z");
     jmethodID next = jenv->GetMethodID(iterCls, "next", "()Ljava/lang/Object;");
     jclass entryCls = FindClassWithMesosClassLoader(jenv, "java/util/Map$Entry");
     jmethodID getKey = jenv->GetMethodID(entryCls, "getKey", "()Ljava/lang/Object;");
     jmethodID getValue = jenv->GetMethodID(entryCls, "getValue", "()Ljava/lang/Object;");
     while (jenv->CallBooleanMethod(iterObj, hasNext)) {
       // Get the key and value for this entry
       jobject entryObj = jenv->CallObjectMethod(iterObj, next);
       jstring keyObj = (jstring) jenv->CallObjectMethod(entryObj, getKey);
       jstring valueObj = (jstring) jenv->CallObjectMethod(entryObj, getValue);
       // Convert them to strings
       const char* keyChars = jenv->GetStringUTFChars(keyObj, NULL);
       if (keyChars == NULL) {
         return $null; // OutOfMemoryError has been thrown
       }
       std::string keyStr(keyChars);
       jenv->ReleaseStringUTFChars(keyObj, keyChars);
       const char* valueChars = jenv->GetStringUTFChars(valueObj, NULL);
       if (valueChars == NULL) {
         return $null; // OutOfMemoryError has been thrown
       }
       std::string valueStr(valueChars);
       jenv->ReleaseStringUTFChars(valueObj, valueChars);
       // Add the entry to the map
       $1_map[keyStr] = valueStr;
       // Delete local refs -- recommended in case map is big and fills local ref table.
       jenv->DeleteLocalRef(entryObj);
       jenv->DeleteLocalRef(keyObj);
       jenv->DeleteLocalRef(valueObj);
     }
     $1 = &$1_map;
  } %}

  %typemap(javain) const std::map<std::string, std::string> & "$javainput"

  %typemap(directorin,descriptor="Ljava/util/Map;") const std::map<std::string, std::string> &
  %{ {
     jclass mapCls = FindClassWithMesosClassLoader(jenv, "java/util/HashMap");
     jmethodID mapCtor = jenv->GetMethodID(mapCls, "<init>", "()V");
     jmethodID put = jenv->GetMethodID(mapCls, "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
     jobject mapObj = jenv->NewObject(mapCls, mapCtor);
     for (std::map<std::string, std::string>::const_iterator it = $1->begin();
          it != $1->end(); ++it)
     {
       jstring keyObj = jenv->NewStringUTF(it->first.c_str());
       if (keyObj == NULL) {
         return $null; // OutOfMemoryError has been thrown
       }
       jstring valueObj = jenv->NewStringUTF(it->second.c_str());
       if (valueObj == NULL) {
         return $null; // OutOfMemoryError has been thrown
       }
       jenv->CallObjectMethod(mapObj, put, keyObj, valueObj);
       // Delete local refs to avoid filling up local ref table if map is big
       jenv->DeleteLocalRef(keyObj);
       jenv->DeleteLocalRef(valueObj);
     }
     $input = mapObj;
  } %}

  %typemap(out) const std::map<std::string, std::string> &
  %{ {
     jclass mapCls = FindClassWithMesosClassLoader(jenv, "java/util/HashMap");
     jmethodID mapCtor = jenv->GetMethodID(mapCls, "<init>", "()V");
     jmethodID put = jenv->GetMethodID(mapCls, "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
     jobject mapObj = jenv->NewObject(mapCls, mapCtor);
     for (std::map<std::string, std::string>::const_iterator it = $1->begin();
          it != $1->end(); ++it)
     {
       jstring keyObj = jenv->NewStringUTF(it->first.c_str());
       if (keyObj == NULL) {
         return $null; // OutOfMemoryError has been thrown
       }
       jstring valueObj = jenv->NewStringUTF(it->second.c_str());
       if (valueObj == NULL) {
         return $null; // OutOfMemoryError has been thrown
       }
       jenv->CallObjectMethod(mapObj, put, keyObj, valueObj);
       // Delete local refs to avoid filling up local ref table if map is big
       jenv->DeleteLocalRef(keyObj);
       jenv->DeleteLocalRef(valueObj);
     }
     $result = mapObj;
  } %}

  %typemap(javaout) const std::map<std::string, std::string> &
  {
    return $jnicall;
  }
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

  /* Declare template instantiations we will use */
  %template(SlaveOfferVector) std::vector<mesos::SlaveOffer>;
  %template(TaskDescriptionVector) std::vector<mesos::TaskDescription>;
  %template(StringMap) std::map<std::string, std::string>;

  %feature("director:except") {
    if( $error != NULL ) {
      PyObject *ptype, *pvalue, *ptraceback;
      PyErr_Fetch( &ptype, &pvalue, &ptraceback );
      PyErr_Restore( ptype, pvalue, ptraceback );
      PyErr_Print();
      Py_Exit(1);
    }
  }

#endif /* SWIGPYTHON */

/* Make it possible to inherit from Scheduler/Executor in target language */
%feature("director") mesos::Scheduler;
%feature("director") mesos::Executor;

%include <mesos_sched.hpp>
%include <mesos_exec.hpp>
