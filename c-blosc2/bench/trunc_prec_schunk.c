/*
  Copyright (C) 2017  Francesc Alted
  http://blosc.org
  License: BSD (see LICENSE.txt)

  Benchmark showing Blosc TRUNC_PREC filter from C code.

  To compile this program:

  $ gcc -O3 trunc_prec_schunk.c -o trunc_prec_schunk -lblosc

*/

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include "blosc.h"


#if defined(_WIN32)
/* For QueryPerformanceCounter(), etc. */
  #include <windows.h>
#elif defined(__MACH__)
  #include <mach/clock.h>
  #include <mach/mach.h>
  #include <time.h>
#elif defined(__unix__)
  #if defined(__linux__)
    #include <time.h>
  #else
    #include <sys/time.h>
  #endif
#else
  #error Unable to detect platform.
#endif

#define KB  1024
#define MB  (1024*KB)
#define GB  (1024*MB)

#define NCHUNKS 200
#define CHUNKSIZE (500 * 1000)
#define NTHREADS 4


/* System-specific high-precision timing functions. */
#if defined(_WIN32)

/* The type of timestamp used on this system. */
#define blosc_timestamp_t LARGE_INTEGER

/* Set a timestamp value to the current time. */
void blosc_set_timestamp(blosc_timestamp_t* timestamp) {
  /* Ignore the return value, assume the call always succeeds. */
  QueryPerformanceCounter(timestamp);
}

/* Given two timestamp values, return the difference in microseconds. */
double blosc_elapsed_usecs(blosc_timestamp_t start_time,
                           blosc_timestamp_t end_time) {
  LARGE_INTEGER CounterFreq;
  QueryPerformanceFrequency(&CounterFreq);

  return (double)(end_time.QuadPart - start_time.QuadPart) /
          ((double)CounterFreq.QuadPart / 1e6);
}

#else

/* The type of timestamp used on this system. */
#define blosc_timestamp_t struct timespec

/* Set a timestamp value to the current time. */
void blosc_set_timestamp(blosc_timestamp_t* timestamp) {
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
  clock_serv_t cclock;
  mach_timespec_t mts;
  host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
  clock_get_time(cclock, &mts);
  mach_port_deallocate(mach_task_self(), cclock);
  timestamp->tv_sec = mts.tv_sec;
  timestamp->tv_nsec = mts.tv_nsec;
#else
  clock_gettime(CLOCK_MONOTONIC, timestamp);
#endif
}

/* Given two timestamp values, return the difference in microseconds. */
double blosc_elapsed_usecs(blosc_timestamp_t start_time,
                           blosc_timestamp_t end_time) {
  return (1e6 * (end_time.tv_sec - start_time.tv_sec))
      + (1e-3 * (end_time.tv_nsec - start_time.tv_nsec));
}

#endif

/* Given two timeval stamps, return the difference in seconds */
double getseconds(blosc_timestamp_t last, blosc_timestamp_t current) {
  return 1e-6 * blosc_elapsed_usecs(last, current);
}

/* Given two timeval stamps, return the time per chunk in usec */
double get_usec_chunk(blosc_timestamp_t last, blosc_timestamp_t current,
                      int niter, size_t nchunks) {
  double elapsed_usecs = blosc_elapsed_usecs(last, current);
  return elapsed_usecs / (double)(niter * nchunks);
}


void fill_buffer(double *buffer, size_t nchunk) {
  double incx = 10. / (NCHUNKS * CHUNKSIZE);

  for (int i = 0; i < CHUNKSIZE; i++) {
    double x = incx * (nchunk * CHUNKSIZE + i);
    buffer[i] = (x - .25) * (x - 4.45) * (x - 8.95);
    //buffer[i] = x;
  }
}


int main() {
  blosc2_cparams cparams = BLOSC_CPARAMS_DEFAULTS;
  blosc2_dparams dparams = BLOSC_DPARAMS_DEFAULTS;
  blosc2_schunk *schunk;
  size_t isize = CHUNKSIZE * sizeof(double);
  int dsize;
  int64_t nbytes, cbytes;
  size_t nchunk, nchunks = 0;
  blosc_timestamp_t last, current;
  float totaltime;
  float totalsize = isize * NCHUNKS;
  double *data_buffer = malloc(CHUNKSIZE * sizeof(double));
  double *rec_buffer = malloc(CHUNKSIZE * sizeof(double));

  printf("Blosc version info: %s (%s)\n",
         BLOSC_VERSION_STRING, BLOSC_VERSION_DATE);

  /* Initialize the Blosc compressor */
  blosc_init();

  /* Create a super-chunk container */
  cparams.filters[0] = BLOSC_TRUNC_PREC;
  cparams.filters_meta[0] = 23;  // treat doubles as floats
  cparams.typesize = sizeof(double);
  // DELTA makes compression ratio quite worse in this case
  //cparams.filters[1] = BLOSC_DELTA;
  // BLOSC_BITSHUFFLE is not compressing better and it quite slower here
  //cparams.filters[BLOSC_LAST_FILTER - 1] = BLOSC_BITSHUFFLE;
  // Good codec params for this dataset
  //cparams.compcode = BLOSC_LZ4;
  //cparams.clevel = 9;
  cparams.compcode = BLOSC_LIZARD;
  cparams.clevel = 9;
  //cparams.compcode = BLOSC_BLOSCLZ;
  //cparams.clevel = 9;
  //cparams.compcode = BLOSC_ZSTD;
  //cparams.clevel = 7;
  cparams.nthreads = NTHREADS;
  schunk = blosc2_new_schunk(cparams, dparams);

  /* Append the chunks */
  blosc_set_timestamp(&last);
  for (nchunk = 0; nchunk < NCHUNKS; nchunk++) {
    fill_buffer(data_buffer, nchunk);
    nchunks = blosc2_append_buffer(schunk, isize, data_buffer);
  }
  blosc_set_timestamp(&current);
  totaltime = (float)getseconds(last, current);
  printf("[Compr] Elapsed time:\t %6.3f s."
                 "  Processed data: %.3f GB (%.3f GB/s)\n",
         totaltime, totalsize / GB, totalsize / (GB * totaltime));

  /* Gather some info */
  nbytes = schunk->nbytes;
  cbytes = schunk->cbytes;
  printf("Compression super-chunk: %ld -> %ld (%.1fx)\n",
         (long)nbytes, (long)cbytes, (1. * nbytes) / cbytes);

  /* Retrieve and decompress the chunks */
  blosc_set_timestamp(&last);
  for (nchunk = 0; nchunk < NCHUNKS; nchunk++) {
    dsize = blosc2_decompress_chunk(schunk, nchunk, (void*)rec_buffer, isize);
    if (dsize < 0) {
      printf("Decompression error.  Error code: %d\n", dsize);
      return dsize;
    }
    assert (dsize == isize);
  }
  blosc_set_timestamp(&current);
  totaltime = (float)getseconds(last, current);
  totalsize = isize * nchunks;
  printf("[Decompr] Elapsed time:\t %6.3f s."
                 "  Processed data: %.3f GB (%.3f GB/s)\n",
         totaltime, totalsize / GB, totalsize / (GB * totaltime));

  /* Check that all the values are in the precision range */
  blosc_set_timestamp(&last);
  for (nchunk = 0; nchunk < NCHUNKS; nchunk++) {
    dsize = blosc2_decompress_chunk(schunk, nchunk, (void*)rec_buffer, isize);
    if (dsize < 0) {
      printf("Decompression error.  Error code: %d\n", dsize);
      return dsize;
    }
    assert (dsize == isize);
    fill_buffer(data_buffer, nchunk);
    for (int i = 0; i < CHUNKSIZE; i++) {
      if (fabs(data_buffer[i] - rec_buffer[i]) > 1e-5) {
        printf("Value not in tolerance margin: ");
        printf("%g - %g: %g, (nchunk: %d, nelem: %d)\n",
               data_buffer[i], rec_buffer[i],
               (data_buffer[i] - rec_buffer[i]), (int)nchunk, i);
        return -1;
      }
    }
  }
  printf("All data did a good roundtrip!\n");

  /* Free resources */
  free(data_buffer);
  free(rec_buffer);
  /* Destroy the super-chunk */
  blosc2_destroy_schunk(schunk);
  /* Destroy the Blosc environment */
  blosc_destroy();

  return 0;
}
