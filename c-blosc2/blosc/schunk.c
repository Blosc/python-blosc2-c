/*********************************************************************
  Blosc - Blocked Shuffling and Compression Library

  Author: Francesc Alted <francesc@blosc.org>
  Creation date: 2015-07-30

  See LICENSES/BLOSC.txt for details about copyright and rights to use.
**********************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "blosc.h"


#if defined(_WIN32) && !defined(__MINGW32__)
  #include <windows.h>
  #include <malloc.h>

/* stdint.h only available in VS2010 (VC++ 16.0) and newer */
  #if defined(_MSC_VER) && _MSC_VER < 1600
    #include "win32/stdint-windows.h"
  #else
    #include <stdint.h>
  #endif

#endif  /* _WIN32 */

/* If C11 is supported, use it's built-in aligned allocation. */
#if __STDC_VERSION__ >= 201112L
  #include <stdalign.h>
#endif


/* Create a new super-chunk */
blosc2_schunk* blosc2_new_schunk(blosc2_cparams cparams,
                                 blosc2_dparams dparams) {
  blosc2_schunk* schunk = calloc(1, sizeof(blosc2_schunk));

  schunk->version = 0;     /* pre-first version */
  for (int i = 0; i < BLOSC_MAX_FILTERS; i++) {
    schunk->filters[i] = cparams.filters[i];
    schunk->filters_meta[i] = cparams.filters_meta[i];
  }
  schunk->compcode = cparams.compcode;
  schunk->clevel = cparams.clevel;
  schunk->typesize = cparams.typesize;
  schunk->blocksize = cparams.blocksize;
  schunk->cbytes = sizeof(blosc2_schunk);

  /* The compression context */
  cparams.schunk = schunk;
  schunk->cctx = blosc2_create_cctx(cparams);

  /* The decompression context */
  dparams.schunk = schunk;
  schunk->dctx = blosc2_create_dctx(dparams);

  return schunk;
}


/* Append an existing chunk into a super-chunk. */
size_t append_chunk(blosc2_schunk* schunk, void* chunk) {
  int64_t nchunks = schunk->nchunks;
  /* The uncompressed and compressed sizes start at byte 4 and 12 */
  int32_t nbytes = *(int32_t*)((uint8_t*)chunk + 4);
  int32_t cbytes = *(int32_t*)((uint8_t*)chunk + 12);

  /* Make space for appending a new chunk and do it */
  schunk->data = realloc(schunk->data, (nchunks + 1) * sizeof(void*));
  schunk->data[nchunks] = chunk;
  /* Update counters */
  schunk->nchunks = nchunks + 1;
  schunk->nbytes += nbytes;
  schunk->cbytes += cbytes + sizeof(void*);
  /* printf("Compression chunk #%lld: %d -> %d (%.1fx)\n", */
  /*         nchunks, nbytes, cbytes, (1.*nbytes) / cbytes); */

  return (size_t)nchunks + 1;
}


/* Append a data buffer to a super-chunk. */
size_t blosc2_append_buffer(blosc2_schunk* schunk, size_t nbytes, void* src) {
  int cbytes;
  void* chunk = malloc(nbytes + BLOSC_MAX_OVERHEAD);

  /* Compress the src buffer using super-chunk context */
  cbytes = blosc2_compress_ctx(schunk->cctx, nbytes, src, chunk,
                               nbytes + BLOSC_MAX_OVERHEAD);
  if (cbytes < 0) {
    free(chunk);
    return (size_t)cbytes;
  }

  return append_chunk(schunk, chunk);
}


/* Decompress and return a chunk that is part of a super-chunk. */
int blosc2_decompress_chunk(blosc2_schunk* schunk, size_t nchunk,
                            void* dest, size_t nbytes) {
  int64_t nchunks = schunk->nchunks;
  void* src;
  int chunksize;
  int nbytes_;

  if (nchunk >= nchunks) {
    printf("specified nchunk ('%ld') exceeds the number of chunks "
           "('%ld') in super-chunk\n", (long)nchunk, (long)nchunks);
    return -10;
  }

  src = schunk->data[nchunk];
  nbytes_ = *(int32_t*)((uint8_t*)src + 4);
  if (nbytes < nbytes_) {
    fprintf(stderr, "Buffer size is too small for the decompressed buffer "
                    "('%ld' bytes, but '%d' are needed)\n",
            (long)nbytes, nbytes_);
    return -11;
  }

  chunksize = blosc2_decompress_ctx(schunk->dctx, src, dest, nbytes);

  return chunksize;
}


/* Free all memory from a super-chunk. */
int blosc2_destroy_schunk(blosc2_schunk* schunk) {

  if (schunk->filters_chunk != NULL)
    free(schunk->filters_chunk);
  if (schunk->codec_chunk != NULL)
    free(schunk->codec_chunk);
  if (schunk->metadata_chunk != NULL)
    free(schunk->metadata_chunk);
  if (schunk->userdata_chunk != NULL)
    free(schunk->userdata_chunk);
  if (schunk->data != NULL) {
    for (int i = 0; i < schunk->nchunks; i++) {
      free(schunk->data[i]);
    }
    free(schunk->data);
  }
  blosc2_free_ctx(schunk->cctx);
  blosc2_free_ctx(schunk->dctx);
  free(schunk);

  return 0;
}


/* Compute the final length of a packed super-chunk */
int64_t blosc2_get_packed_length(blosc2_schunk* schunk) {
  int i;
  int64_t length = sizeof(blosc2_schunk);

  if (schunk->filters_chunk != NULL)
    length += *(int32_t*)(schunk->filters_chunk + 12);
  if (schunk->codec_chunk != NULL)
    length += *(int32_t*)(schunk->codec_chunk + 12);
  if (schunk->metadata_chunk != NULL)
    length += *(int32_t*)(schunk->metadata_chunk + 12);
  if (schunk->userdata_chunk != NULL)
    length += *(int32_t*)(schunk->userdata_chunk + 12);
  if (schunk->data != NULL) {
    for (i = 0; i < schunk->nchunks; i++) {
      length += sizeof(int64_t);
      length += *(int32_t*)(schunk->data[i] + 12);
    }
  }
  return length;
}


/* Copy a chunk into a packed super-chunk */
void pack_copy_chunk(void* chunk, void* packed, int offset, int64_t* cbytes,
                     int64_t* nbytes) {
  int32_t cbytes_, nbytes_;

  if (chunk != NULL) {
    nbytes_ = *(int32_t*)((uint8_t*)chunk + 4);
    cbytes_ = *(int32_t*)((uint8_t*)chunk + 12);
    memcpy((uint8_t*)packed + (size_t)*cbytes, chunk, (size_t)cbytes_);
    *(int64_t*)((uint8_t*)packed + offset) = *cbytes;
    *nbytes += nbytes_;
    *cbytes += cbytes_;
  }
  else {
    /* No data in chunk */
    *(int64_t*)((uint8_t*)packed + offset) = 0;
  }
}


/* Create a packed super-chunk */
void* blosc2_pack_schunk(blosc2_schunk* schunk) {
  int64_t cbytes = sizeof(blosc2_schunk);
  int64_t nbytes = sizeof(blosc2_schunk);
  int64_t nchunks = schunk->nchunks;
  void* packed;
  void* data_chunk;
  int64_t* data_pointers;
  uint64_t data_offsets_len;
  int32_t chunk_cbytes, chunk_nbytes;
  int64_t packed_len;
  int i;

  packed_len = blosc2_get_packed_length(schunk);
  packed = malloc((size_t)packed_len);

  /* Fill the header */
  memcpy(packed, schunk, 40);    /* copy until cbytes */

  /* Fill the ancillary chunks info */
  pack_copy_chunk(schunk->filters_chunk,  packed, 40, &cbytes, &nbytes);
  pack_copy_chunk(schunk->codec_chunk,    packed, 48, &cbytes, &nbytes);
  pack_copy_chunk(schunk->metadata_chunk, packed, 56, &cbytes, &nbytes);
  pack_copy_chunk(schunk->userdata_chunk, packed, 64, &cbytes, &nbytes);

  /* Finally, setup the data pointers section */
  data_offsets_len = nchunks * sizeof(int64_t);
  data_pointers = (int64_t*)((uint8_t*)packed + packed_len - data_offsets_len);
  *(uint64_t*)((uint8_t*)packed + 72) = packed_len - data_offsets_len;

  /* And fill the actual data chunks */
  if (schunk->data != NULL) {
    for (i = 0; i < nchunks; i++) {
      data_chunk = schunk->data[i];
      chunk_nbytes = *(int32_t*)((uint8_t*)data_chunk + 4);
      chunk_cbytes = *(int32_t*)((uint8_t*)data_chunk + 12);
      memcpy((uint8_t*)packed + cbytes, data_chunk, (size_t)chunk_cbytes);
      data_pointers[i] = cbytes;
      cbytes += chunk_cbytes;
      nbytes += chunk_nbytes;
    }
  }

  /* Add the length for the data chunk offsets */
  cbytes += data_offsets_len;
  nbytes += data_offsets_len;
  assert (cbytes == packed_len);
  *(int64_t*)((uint8_t*)packed + 16) = nchunks;
  *(int64_t*)((uint8_t*)packed + 24) = nbytes;
  *(int64_t*)((uint8_t*)packed + 32) = cbytes;

  return packed;
}


/* Copy a chunk into a packed super-chunk */
void* unpack_copy_chunk(uint8_t* packed, int offset,
    blosc2_schunk* schunk, int64_t *nbytes, int64_t *cbytes) {
  int32_t nbytes_, cbytes_;
  uint8_t *chunk, *dst_chunk = NULL;

  if (*(int64_t*)(packed + offset) != 0) {
    chunk = packed + *(int64_t*)(packed + offset);
    nbytes_ = *(int32_t*)(chunk + 4);
    cbytes_ = *(int32_t*)(chunk + 12);
    /* Create a copy of the chunk */
    dst_chunk = malloc((size_t)cbytes_);
    memcpy(dst_chunk, chunk, (size_t)cbytes_);
    /* Update counters */
    schunk->nbytes += nbytes_;
    schunk->cbytes += cbytes_;
    *cbytes += cbytes_;
    *nbytes += nbytes_;
  }
  return dst_chunk;
}


/* Unpack a packed super-chunk */
blosc2_schunk* blosc2_unpack_schunk(void* packed) {
  blosc2_schunk* schunk = calloc(1, sizeof(blosc2_schunk));
  int64_t nbytes = sizeof(blosc2_schunk);
  int64_t cbytes = sizeof(blosc2_schunk);
  uint8_t* data_chunk;
  void* new_chunk;
  int64_t* data;
  int64_t nchunks;
  int32_t chunk_size;
  int i;

  /* Fill the header */
  memcpy(schunk, packed, 52); /* Copy until cbytes */

  /* Fill the ancillary chunks info */
  schunk->filters_chunk = unpack_copy_chunk(
          packed, 52, schunk, &nbytes, &cbytes);
  schunk->codec_chunk = unpack_copy_chunk(
          packed, 52 + 8, schunk, &nbytes, &cbytes);
  schunk->metadata_chunk = unpack_copy_chunk(
          packed, 52 + 8 * 2, schunk, &nbytes, &cbytes);
  schunk->userdata_chunk = unpack_copy_chunk(
          packed, 52 + 8 * 3, schunk, &nbytes, &cbytes);

  /* Finally, fill the data pointers section */
  data = (int64_t*)(
          (uint8_t*)packed + *(int64_t*)((uint8_t*)packed + 52 + 8 * 4));
  nchunks = *(int64_t*)((uint8_t*)packed + 28);
  schunk->data = malloc(nchunks * sizeof(void*));
  nbytes += nchunks * sizeof(int64_t);
  cbytes += nchunks * sizeof(int64_t);

  /* And create the actual data chunks */
  if (data != NULL) {
    for (i = 0; i < nchunks; i++) {
      data_chunk = (uint8_t*)packed + data[i];
      chunk_size = *(int32_t*)(data_chunk + 12);
      new_chunk = malloc((size_t)chunk_size);
      memcpy(new_chunk, data_chunk, (size_t)chunk_size);
      schunk->data[i] = new_chunk;
      cbytes += chunk_size;
      nbytes += *(int32_t*)(data_chunk + 4);
    }
  }
  schunk->nbytes = nbytes;
  schunk->cbytes = cbytes;

  assert(*(int64_t*)((uint8_t*)packed + 36) == nbytes);
  assert(*(int64_t*)((uint8_t*)packed + 44) == cbytes);

  return schunk;
}


/* Append an existing chunk into a *packed* super-chunk. */
void* packed_append_chunk(void* packed, void* chunk) {
  int64_t nchunks = *(int64_t*)((uint8_t*)packed + 28);
  int64_t packed_len = *(int64_t*)((uint8_t*)packed + 44);
  int64_t data_offsets = *(int64_t*)((uint8_t*)packed + 52 + 8 * 4);
  uint64_t chunk_offset = packed_len - nchunks * sizeof(int64_t);
  /* The uncompressed and compressed sizes start at byte 4 and 12 */
  int32_t nbytes = *(int32_t*)((uint8_t*)chunk + 4);
  int32_t cbytes = *(int32_t*)((uint8_t*)chunk + 12);
  /* The current and new data areas */
  uint8_t* data;
  uint8_t* new_data;

  /* Make space for the new chunk and copy it */
  packed = realloc(packed, packed_len + cbytes + sizeof(int64_t));
  data = (uint8_t*)packed + data_offsets;
  new_data = data + cbytes;
  /* Move the data offsets to the end */
  memmove(new_data, data, (size_t)(nchunks * sizeof(int64_t)));
  ((uint64_t*)new_data)[nchunks] = chunk_offset;
  /* Copy the chunk */
  memcpy((uint8_t*)packed + chunk_offset, chunk, (size_t)cbytes);
  /* Update counters */
  *(int64_t*)((uint8_t*)packed + 28) += 1;
  *(uint64_t*)((uint8_t*)packed + 36) += nbytes + sizeof(uint64_t);
  *(uint64_t*)((uint8_t*)packed + 44) += cbytes + sizeof(uint64_t);
  *(uint64_t*)((uint8_t*)packed + 52 + 8 * 3) += cbytes;
  /* printf("Compression chunk #%lld: %d -> %d (%.1fx)\n",
          nchunks, nbytes, cbytes, (1.*nbytes) / cbytes); */

  return packed;
}


/* Append a data buffer to a *packed* super-chunk. */
// TODO: Update for the new filter pipeline support
void* blosc2_packed_append_buffer(void* packed, size_t typesize, size_t nbytes,
                                  void* src) {
  int cname = *(int16_t*)((uint8_t*)packed + 4);
  int clevel = *(int16_t*)((uint8_t*)packed + 6);
  void* filters_chunk = (uint8_t*)packed + *(uint64_t*)((uint8_t*)packed + 52);
  uint8_t* filters = (uint8_t*)packed + 12;
  int cbytes;
  void* chunk = malloc(nbytes + BLOSC_MAX_OVERHEAD);
  void* dest = malloc(nbytes);
  char* compname;
  int doshuffle, dodelta = 0;
  void* new_packed;

  /* Apply filters prior to compress */
  if (filters[0] == BLOSC_DELTA) {
    dodelta = 1;
    doshuffle = filters[1];
  }
  else if (filters[0] == BLOSC_TRUNC_PREC) {
    doshuffle = filters[1];
  }
  else {
    doshuffle = filters[0];
  }

  /* Compress the src buffer using super-chunk defaults */
  blosc_compcode_to_compname(cname, &compname);
  blosc_set_compressor(compname);
  blosc_set_delta(dodelta);
  cbytes = blosc_compress(clevel, doshuffle, typesize, nbytes, src, chunk,
                          nbytes + BLOSC_MAX_OVERHEAD);
  if (cbytes < 0) {
    free(chunk);
    free(dest);
    return NULL;
  }

  free(dest);

  /* Append the chunk and free it */
  new_packed = packed_append_chunk(packed, chunk);
  free(chunk);

  return new_packed;
}


/* Decompress and return a chunk that is part of a *packed* super-chunk. */
int blosc2_packed_decompress_chunk(void* packed, size_t nchunk, void** dest) {
  int64_t nchunks = *(int64_t*)((uint8_t*)packed + 28);
  int64_t* data = (int64_t*)(
          (uint8_t*)packed + *(int64_t*)((uint8_t*)packed + 52 + 8 * 4));
  void* src;
  int chunksize;
  int32_t nbytes;

  if (nchunk >= nchunks) {
    return -10;
  }

  /* Grab the address of the chunk */
  src = (uint8_t*)packed + data[nchunk];
  /* Create a buffer for destination */
  nbytes = *(int32_t*)((uint8_t*)src + 4);
  *dest = malloc((size_t)nbytes);

  /* And decompress it */
  chunksize = blosc_decompress(src, *dest, (size_t)nbytes);
  if (chunksize < 0) {
    return chunksize;
  }
  if (chunksize != nbytes) {
    return -11;
  }

  return chunksize;
}
