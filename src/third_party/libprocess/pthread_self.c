#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdio.h>


struct Process;

struct Process * proc_process = NULL;

pthread_t proc_thread = 0;


/* TODO(benh): Make thread-safe! */

pthread_t (*___pthread_self)() = NULL;


pthread_t __pthread_self(void)
{
  if (___pthread_self == NULL)
    ___pthread_self = dlsym(RTLD_NEXT, "pthread_self");
  
  return ___pthread_self();
}


pthread_t pthread_self(void)
{
/*   void *array[10]; */

/*   size_t size = backtrace(array, 10); */

/*   backtrace_symbols_fd(array, size, fileno(stdout)); */

  if (___pthread_self == NULL)
    ___pthread_self = dlsym(RTLD_NEXT, "pthread_self");

  if (proc_thread == ___pthread_self()) {
    printf("proc_process: 0x%x\n", proc_process);
    return (pthread_t) proc_process;
  }

  return ___pthread_self();
}
