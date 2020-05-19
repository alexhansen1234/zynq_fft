#include <stdio.h>
#include <stdlib.h>
#include <fftw3.h>

#define N 1024
#define ITERS 100000

int main(int argc, char ** argv)
{
  volatile fftw_complex in[N], out[N];
  fftw_plan p;
  p = fftw_plan_dft_1d(N, in, out, FFTW_FORWARD, FFTW_ESTIMATE);

  for(int i=0; i < N; ++i)
  {
    in[i][0] = ((float)rand())/RAND_MAX;
    in[i][1] = ((float)rand())/RAND_MAX;
  }

  for(int i=0; i < ITERS; ++i)
    fftw_execute(p);

  fftw_destroy_plan(p);

  return 0;
}
