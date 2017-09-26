/*********************************************************************
  Blosc - Blocked Shuffling and Compression Library

  Author: Francesc Alted <francesc@blosc.org>

  See LICENSES/BLOSC.txt for details about copyright and rights to use.
**********************************************************************/

#ifndef BLOSC_DELTA_H
#define BLOSC_DELTA_H

void delta_encoder(const uint8_t* dref, size_t offset, size_t nbytes,
                   size_t typesize, const uint8_t* src, uint8_t* dest);

void delta_decoder(const uint8_t* dref, size_t offset, size_t nbytes,
                   size_t typesize, uint8_t* dest);

#endif //BLOSC_DELTA_H
