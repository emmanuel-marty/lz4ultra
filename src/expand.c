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

/* This code is mostly here to verify the compressor's output. You should use the real, optimized lz4 decompressor to decompress your data. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "format.h"
#include "expand.h"

#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else /* _MSC_VER */
#define FORCE_INLINE __attribute__((always_inline))
#endif /* _MSC_VER */

static inline FORCE_INLINE int lz4ultra_expand_literals_slow(const unsigned char **ppInBlock, const unsigned char *pInBlockEnd, int nLiterals, unsigned char **ppCurOutData, const unsigned char *pOutDataEnd) {
   const unsigned char *pInBlock = *ppInBlock;
   unsigned char *pCurOutData = *ppCurOutData;

   if (nLiterals == LITERALS_RUN_LEN) {
      unsigned char nByte;

      do {
         if (pInBlock >= pInBlockEnd) break;
         nByte = *pInBlock++;
         nLiterals += (int)((unsigned int)nByte);
      } while (nByte == 255);
   }

   if (nLiterals != 0) {
      if ((pInBlock + nLiterals) > pInBlockEnd ||
         (pCurOutData + nLiterals) > pOutDataEnd) {
         return -1;
      }

      memcpy(pCurOutData, pInBlock, nLiterals);
      pInBlock += nLiterals;
      pCurOutData += nLiterals;
   }

   *ppInBlock = pInBlock;
   *ppCurOutData = pCurOutData;
   return 0;
}

static inline FORCE_INLINE int lz4ultra_expand_match_slow(const unsigned char **ppInBlock, const unsigned char *pInBlockEnd, const unsigned char *pSrc, int nMatchLen, unsigned char **ppCurOutData, const unsigned char *pOutDataEnd, const unsigned char *pOutDataFastEnd) {
   const unsigned char *pInBlock = *ppInBlock;
   unsigned char *pCurOutData = *ppCurOutData;

   if (nMatchLen == MATCH_RUN_LEN) {
      unsigned char nByte;

      do {
         if (pInBlock >= pInBlockEnd) break;
         nByte = *pInBlock++;
         nMatchLen += (int)((unsigned int)nByte);
      } while (nByte == 255);
   }

   nMatchLen += MIN_MATCH_SIZE;

   if ((pCurOutData + nMatchLen) > pOutDataEnd) {
      return -1;
   }

   if ((pSrc + 1) == pCurOutData && nMatchLen >= 16) {
      /* One-byte RLE */
      memset(pCurOutData, *pSrc, nMatchLen);
      pCurOutData += nMatchLen;
   }
   else {
      /* Do a deterministic, left to right byte copy instead of memcpy() so as to handle overlaps */

      if ((pCurOutData - pSrc) >= 8) {
         int nMaxFast = nMatchLen;
         if ((pCurOutData + nMaxFast) > (pOutDataFastEnd - 15))
            nMaxFast = (int)(pOutDataFastEnd - 15 - pCurOutData);

         if (nMaxFast > 0) {
            const unsigned char *pCopySrc = pSrc;
            unsigned char *pCopyDst = pCurOutData;
            const unsigned char *pCopyEndDst = pCurOutData + nMaxFast;

            do {
               memcpy(pCopyDst, pCopySrc, 8);
               memcpy(pCopyDst + 8, pCopySrc+8, 8);
               pCopySrc += 16;
               pCopyDst += 16;
            } while (pCopyDst < pCopyEndDst);

            pCurOutData += nMaxFast;
            pSrc += nMaxFast;
            nMatchLen -= nMaxFast;
         }
      }

      while (nMatchLen >= 4) {
         *pCurOutData++ = *pSrc++;
         *pCurOutData++ = *pSrc++;
         *pCurOutData++ = *pSrc++;
         *pCurOutData++ = *pSrc++;
         nMatchLen -= 4;
      }
      while (nMatchLen > 0) {
         *pCurOutData++ = *pSrc++;
         nMatchLen--;
      }
   }

   *ppInBlock = pInBlock;
   *ppCurOutData = pCurOutData;
   return 0;
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
int lz4ultra_expand_block(const unsigned char *pInBlock, int nBlockSize, unsigned char *pOutData, int nOutDataOffset, int nBlockMaxSize) {
   const unsigned char *pInBlockEnd = pInBlock + nBlockSize;
   const unsigned char *pInBlockFastEnd = pInBlock + nBlockSize - 16;
   unsigned char *pCurOutData = pOutData + nOutDataOffset;
   const unsigned char *pOutDataEnd = pCurOutData + nBlockMaxSize;
   const unsigned char *pOutDataFastEnd = pOutDataEnd - 16;

   /* Fast loop */

   while (pInBlock < pInBlockFastEnd && pCurOutData < pOutDataFastEnd) {
      const unsigned char token = *pInBlock++;
      int nLiterals = (int)((unsigned int)((token & 0xf0) >> 4));

      if (nLiterals < 15) {
         memcpy(pCurOutData, pInBlock, 16);
         pInBlock += nLiterals;
         pCurOutData += nLiterals;
      }
      else {
         if (lz4ultra_expand_literals_slow(&pInBlock, pInBlockEnd, nLiterals, &pCurOutData, pOutDataEnd))
            return -1;
      }

      if ((pInBlock + 1) < pInBlockEnd) { /* The last token in the block does not include match information */
         int nMatchOffset;

         nMatchOffset = ((unsigned int)*pInBlock++);
         nMatchOffset |= (((unsigned int)*pInBlock++) << 8);

         const unsigned char *pSrc = pCurOutData - nMatchOffset;
         if (pSrc < pOutData)
            return -1;

         int nMatchLen = (int)((unsigned int)(token & 0x0f));
         if (nMatchLen < (16 - MIN_MATCH_SIZE + 1) && (pSrc + MIN_MATCH_SIZE + nMatchLen) < pCurOutData && pCurOutData < pOutDataFastEnd) {
            memcpy(pCurOutData, pSrc, 16);
            pCurOutData += (MIN_MATCH_SIZE + nMatchLen);
         }
         else {
            if (lz4ultra_expand_match_slow(&pInBlock, pInBlockEnd, pSrc, nMatchLen, &pCurOutData, pOutDataEnd, pOutDataFastEnd))
               return -1;
         }
      }
   }

   /* Slow loop for the remainder of the buffer */

   while (pInBlock < pInBlockEnd) {
      const unsigned char token = *pInBlock++;
      int nLiterals = (int)((unsigned int)((token & 0xf0) >> 4));

      if (lz4ultra_expand_literals_slow(&pInBlock, pInBlockEnd, nLiterals, &pCurOutData, pOutDataEnd))
         return -1;

      if ((pInBlock + 1) < pInBlockEnd) { /* The last token in the block does not include match information */
         int nMatchOffset;

         nMatchOffset = ((unsigned int)*pInBlock++);
         nMatchOffset |= (((unsigned int)*pInBlock++) << 8);

         const unsigned char *pSrc = pCurOutData - nMatchOffset;
         if (pSrc < pOutData)
            return -1;

         int nMatchLen = (int)((unsigned int)(token & 0x0f));
         if (lz4ultra_expand_match_slow(&pInBlock, pInBlockEnd, pSrc, nMatchLen, &pCurOutData, pOutDataEnd, pOutDataFastEnd))
            return -1;
      }
   }

   return (int)(pCurOutData - (pOutData + nOutDataOffset));
}
