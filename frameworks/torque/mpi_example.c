#include "mpi.h"
#include "unistd.h"
#include "stdio.h"

int main(int argc, char **argv) {

  size_t len=256;
  char *hostname = new char[len];
  int size,rank;

  MPI_Init(&argc, &argv);

  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  gethostname(hostname, len);

  printf("Hi, I am %d of %d and my hostname is %s\n", rank, size, hostname);

  MPI_Finalize();

}
