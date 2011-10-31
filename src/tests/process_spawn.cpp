#include <iostream>

using std::cout;
using std::cin;
using std::endl;

void processinfo()
{
  cout << "pid: " << getpid()
       << " pgid: " << getpgrp()
       << " ppid: " << getppid() << endl;
}

void dummywait()
{
  while(getchar() == EOF) {
      sleep(INT_MAX);
  }

  cout << "Error: Shouldn't come here" << endl;
}

// This program forks 2 processes in a chain. The child process exits after
// forking its own child (the grandchild of the main). The grandchild is
// parented by Init but is in the same group as the main process.
int main()
{
  // Become session leader.
  setsid();

  pid_t pid = fork();

  if (pid) {
    //cout << "Inside Parent process: ";
    processinfo();
    dummywait();
  } else {
    //cout << "Inside Child process: ";
    processinfo();

    pid_t pid2 = fork();

    if (pid2) {
      // Kill the child process so that the tree link is broken.
      _exit(0);
    } else {
      //cout << "Inside Grand Child process:";
      processinfo();
      dummywait();
    }
  }

  cout << "Error: Shouldn't come here!!" << endl;
  return -1;
}
