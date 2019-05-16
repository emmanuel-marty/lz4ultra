/*
 * expand.c - block decompressor implementation
 *
 * Copyright (C) 2019 Emmanuel Marty
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

/*
 * Uses the libdivsufsort library Copyright (c) 2003-2008 Yuta Mori
 *
 * Inspired by LZ4 by Yann Collet. https://github.com/lz4/lz4
 * With help, ideas, optimizations and speed measurements by spke <zxintrospec@gmail.com>
 * With ideas from Lizard by Przemyslaw Skibinski and Yann Collet. https://github.com/inikep/lizard
 * Also with ideas from smallz4 by Stephan Brumme. https://create.stephan-brumme.com/smallz4/
 *
 */

/* This code is mostly here to verify the compressor's output. You should use the real, optimized lz4 decompressor to decompress your data. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "format.h"
#include "expand.h"

#if defined(__GNUC__) || defined(__clang__)
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#define LZ4ULTRA_DECOMPRESSOR_BUILD_LEN(__len) { \
   unsigned int byte; \
   do { \
      if (unlikely(pInBlock >= pInBlockEnd)) return -1; \
      byte = (unsigned int)*pInBlock++; \
      __len += byte; \
   } while (unlikely(byte == 255)); \
}

/**
 * Decompress one data block
 *
 * @param pInBlock pointer to compressed data
 * @param nInBlockSize size of compressed data, in bytes
 * @param pOutData pointer to output decompression buffer (previously decompressed bytes + room for decompressing this block)
 * @param nOutDataOffset starting index of where to store decompressed bytes in output buffer (and size of previously decompressed bytes)
 * @param nBlockMaxSize total size of output decompression buffer, in bytes
 *
 * @return size of decompressed data in bytes, or -1 for error
 */
int lz4ultra_decompressor_expand_block_lz4(const unsigned char *pInBlock, int nBlockSize, unsigned char *pOutData, int nOutDataOffset, int nBlockMaxSize) {
   const unsigned char *pInBlockEnd = pInBlock + nBlockSize;
   unsigned char *pCurOutData = pOutData + nOutDataOffset;
   const unsigned char *pOutDataEnd = pCurOutData + nBlockMaxSize;
   const unsigned char *pOutDataFastEnd = pOutDataEnd - 18;

   while (likely(pInBlock < pInBlockEnd)) {
      const unsigned int token = (unsigned int)*pInBlock++;
      unsigned int nLiterals = ((token & 0xf0) >> 4);

      if (nLiterals != LITERALS_RUN_LEN && pCurOutData <= pOutDataFastEnd && (pInBlock + 16) <= pInBlockEnd) {
         memcpy(pCurOutData, pInBlock, 16);
      }
      else {
         if (likely(nLiterals == LITERALS_RUN_LEN))
            LZ4ULTRA_DECOMPRESSOR_BUILD_LEN(nLiterals);

         if (unlikely((pInBlock + nLiterals) > pInBlockEnd)) return -1;
         if (unlikely((pCurOutData + nLiterals) > pOutDataEnd)) return -1;

         memcpy(pCurOutData, pInBlock, nLiterals);
      }

      pInBlock += nLiterals;
      pCurOutData += nLiterals;

      if (likely((pInBlock + 2) <= pInBlockEnd)) {
         unsigned int nMatchOffset;

         nMatchOffset = (unsigned int)*pInBlock++;
         nMatchOffset |= ((unsigned int)*pInBlock++) << 8;

         unsigned int nMatchLen = (token & 0x0f);

         nMatchLen += MIN_MATCH_SIZE;
         if (nMatchLen != (MATCH_RUN_LEN + MIN_MATCH_SIZE) && nMatchOffset >= 8 && pCurOutData <= pOutDataFastEnd) {
            const unsigned char *pSrc = pCurOutData - nMatchOffset;

            if (unlikely(pSrc < pOutData)) return -1;

            memcpy(pCurOutData, pSrc, 8);
            memcpy(pCurOutData + 8, pSrc + 8, 8);
            memcpy(pCurOutData + 16, pSrc + 16, 2);

            pCurOutData += nMatchLen;
         }
         else {
            if (likely(nMatchLen == (MATCH_RUN_LEN + MIN_MATCH_SIZE)))
               LZ4ULTRA_DECOMPRESSOR_BUILD_LEN(nMatchLen);

            if (unlikely((pCurOutData + nMatchLen) > pOutDataEnd)) return -1;

            const unsigned char *pSrc = pCurOutData - nMatchOffset;
            if (unlikely(pSrc < pOutData)) return -1;

            if (nMatchOffset >= 16 && (pCurOutData + nMatchLen) <= pOutDataFastEnd) {
               const unsigned char *pCopySrc = pSrc;
               unsigned char *pCopyDst = pCurOutData;
               const unsigned char *pCopyEndDst = pCurOutData + nMatchLen;

               do {
                  memcpy(pCopyDst, pCopySrc, 16);
                  pCopySrc += 16;
                  pCopyDst += 16;
               } while (pCopyDst < pCopyEndDst);

               pCurOutData += nMatchLen;
            }
            else {
               while (nMatchLen--) {
                  *pCurOutData++ = *pSrc++;
               }
            }
         }
      }
   }

   return (int)(pCurOutData - (pOutData + nOutDataOffset));
}
