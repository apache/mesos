#include <jni.h>

#include <mesos/version.hpp>

extern "C" {

/*
 * Class:     org_apache_mesos_MesosNativeLibrary
 * Method:    version
 * Signature: ()Lorg/apache/mesos/MesosNativeLibrary$Version;
 */
JNIEXPORT jobject JNICALL Java_org_apache_mesos_MesosNativeLibrary__1version
  (JNIEnv* env)
{
  jclass clazz = env->FindClass("org/apache/mesos/MesosNativeLibrary$Version");
  jmethodID _init_ = env->GetMethodID(clazz, "<init>", "(JJJ)V");
  jobject jversion = env->NewObject(
      clazz,
      _init_,
      (jlong) MESOS_MAJOR_VERSION,
      (jlong) MESOS_MINOR_VERSION,
      (jlong) MESOS_PATCH_VERSION);
  return jversion;
}

} // extern "C" {