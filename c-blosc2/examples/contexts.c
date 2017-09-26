/*
    Copyright (C) 2014  Francesc Alted
    http://blosc.org
    License: MIT (see LICENSE.txt)

    Example program demonstrating the use of a Blosc from C code.

    To compile this program:

    $ gcc -O contexts.c -o contexts -lblosc

    To run:

    $ ./contexts
    Blosc version info: 2.0.0a2 ($Date:: 2016-01-08 #$)
    Compression: 40000000 -> 999393 (40.0x)
    Correctly extracted 5 elements from compressed chunk!
    Decompression succesful!
    Succesful roundtrip!

*/

#include <stdio.h>
#include "blosc.h"

#define SIZE 10 * 1000 * 1000
#define NTHREADS 2


int main() {
  static float data[SIZE];
  static float data_out[SIZE];
  static float data_dest[SIZE];
  float data_subset[5];
  float data_subset_ref[5] = {5, 6, 7, 8, 9};
  int isize = SIZE * sizeof(float), osize = SIZE * sizeof(float);
  int dsize = SIZE * sizeof(float), csize;
  int i, ret;
  blosc2_context_cparams cparams = BLOSC_CPARAMS_DEFAULTS;
  blosc2_context_dparams dparams = BLOSC_DPARAMS_DEFAULTS;
  blosc_context *cctx, *dctx;

  /* Initialize dataset */
  for (i = 0; i < SIZE; i++) {
    data[i] = i;
  }

  printf("Blosc version info: %s (%s)\n",
         BLOSC_VERSION_STRING, BLOSC_VERSION_DATE);

  /* Create a context for compression */
  cparams.typesize = sizeof(float);
  cparams.compcode = BLOSC_BLOSCLZ;
  cparams.filtercode = BLOSC_SHUFFLE;
  cparams.clevel = 5;
  cparams.nthreads = NTHREADS;
  cctx = blosc2_create_cctx(&cparams);

  /* Compress with clevel=5 and shuffle active  */
  csize = blosc2_compress_ctx(cctx, isize, data, data_out, osize);
  if (csize == 0) {
    printf("Buffer is uncompressible.  Giving up.\n");
    return 1;
  }
  else if (csize < 0) {
    printf("Compression error.  Error code: %d\n", csize);
    return csize;
  }

  printf("Compression: %d -> %d (%.1fx)\n", isize, csize, (1. * isize) / csize);

  /* Create a context for decompression */
  dparams.nthreads = NTHREADS;
  dctx = blosc2_create_dctx(&dparams);

  ret = blosc2_getitem_ctx(dctx, data_out, 5, 5, data_subset);
  if (ret < 0) {
    printf("Error in blosc2_getitem_ctx().  Giving up.\n");
    return 1;
  }

  for (i = 0; i < 5; i++) {
    if (data_subset[i] != data_subset_ref[i]) {
      printf("blosc2_getitem_ctx() fetched data differs from original!\n");
      return -1;
    }
  }
  printf("Correctly extracted 5 elements from compressed chunk!\n");

  /* Decompress  */
  dsize = blosc2_decompress_ctx(dctx, data_out, data_dest, dsize);
  if (dsize < 0) {
    printf("Decompression error.  Error code: %d\n", dsize);
    return dsize;
  }

  printf("Decompression succesful!\n");

  for (i = 0; i < SIZE; i++) {
    if (data[i] != data_dest[i]) {
      printf("Decompressed data differs from original!\n");
      return -1;
    }
  }
  printf("Succesful roundtrip!\n");

  /* Release resources */
  blosc2_free_ctx(cctx);
  blosc2_free_ctx(dctx);

  return 0;
}
