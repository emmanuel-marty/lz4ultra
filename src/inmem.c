/*
 * inmem.c - in-memory decompression for benchmarks
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inmem.h"
#include "lib.h"
#include "frame.h"

/**
 * Get maximum decompressed size of compressed data
 *
 * @param pFileData compressed data
 * @param nFileSize compressed size in bytes
 *
 * @return maximum decompressed size
 */
size_t lz4ultra_inmem_get_max_decompressed_size(const unsigned char *pFileData, size_t nFileSize) {
   const unsigned char *pCurFileData = pFileData;
   const unsigned char *pEndFileData = pCurFileData + nFileSize;
   int nBlockMaxCode = 0;
   unsigned int nFlags = 0;
   int nBlockMaxBits, nBlockMaxSize;
   size_t nMaxDecompressedSize = 0;

   /* Check header */
   if ((pCurFileData + LZ4ULTRA_HEADER_SIZE) > pEndFileData ||
       lz4ultra_decode_header(pCurFileData, LZ4ULTRA_HEADER_SIZE, &nBlockMaxCode, &nFlags) != LZ4ULTRA_DECODE_OK)
      return -1;

   nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   nBlockMaxSize = 1 << nBlockMaxBits;

   pCurFileData += LZ4ULTRA_HEADER_SIZE;

   while (pCurFileData < pEndFileData) {
      unsigned int nBlockDataSize = 0;
      int nIsUncompressed = 0;

      /* Decode frame header */
      if ((pCurFileData + LZ4ULTRA_FRAME_SIZE) > pEndFileData ||
          lz4ultra_decode_frame(pCurFileData, LZ4ULTRA_FRAME_SIZE, nFlags, &nBlockDataSize, &nIsUncompressed) != LZ4ULTRA_DECODE_OK)
         return -1;
      pCurFileData += LZ4ULTRA_FRAME_SIZE;

      if (!nBlockDataSize)
         break;

      /* Add one potentially full block to the decompressed size */
      nMaxDecompressedSize += nBlockMaxSize;

      if ((pCurFileData + nBlockDataSize) > pEndFileData)
         return -1;

      pCurFileData += nBlockDataSize;
   }

   return nMaxDecompressedSize;
}

/**
 * Decompress data in memory
 *
 * @param pFileData compressed data
 * @param pOutBuffer buffer for decompressed data
 * @param nFileSize compressed size in bytes
 * @param nMaxOutBufferSize maximum capacity of decompression buffer
 *
 * @return actual decompressed size, or -1 for error
 */
size_t lz4ultra_inmem_decompress_stream(const unsigned char *pFileData, unsigned char *pOutBuffer, size_t nFileSize, size_t nMaxOutBufferSize) {
   const unsigned char *pCurFileData = pFileData;
   const unsigned char *pEndFileData = pCurFileData + nFileSize;
   unsigned char *pCurOutBuffer = pOutBuffer;
   const unsigned char *pEndOutBuffer = pCurOutBuffer + nMaxOutBufferSize;
   int nBlockMaxCode = 0;
   unsigned int nFlags = 0;
   int nBlockMaxBits, nBlockMaxSize, nPreviousBlockSize;

   /* Check header */
   if ((pCurFileData + LZ4ULTRA_HEADER_SIZE) > pEndFileData ||
      lz4ultra_decode_header(pCurFileData, LZ4ULTRA_HEADER_SIZE, &nBlockMaxCode, &nFlags) != LZ4ULTRA_DECODE_OK)
      return -1;

   nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   nBlockMaxSize = 1 << nBlockMaxBits;

   pCurFileData += LZ4ULTRA_HEADER_SIZE;
   nPreviousBlockSize = 0;

   while (pCurFileData < pEndFileData) {
      unsigned int nBlockDataSize = 0;
      int nIsUncompressed = 0;

      /* Decode frame header */
      if ((pCurFileData + LZ4ULTRA_FRAME_SIZE) > pEndFileData ||
          lz4ultra_decode_frame(pCurFileData, LZ4ULTRA_FRAME_SIZE, nFlags, &nBlockDataSize, &nIsUncompressed) != LZ4ULTRA_DECODE_OK)
         return -1;
      pCurFileData += LZ4ULTRA_FRAME_SIZE;

      if (!nBlockDataSize)
         break;

      if (!nIsUncompressed) {
         int nDecompressedSize;

         /* Decompress block */
         if ((pCurFileData + nBlockDataSize) > pEndFileData)
            return -1;

         if ((nFlags & LZ4ULTRA_FLAG_INDEP_BLOCKS) || (nPreviousBlockSize == 0))
            nDecompressedSize = lz4ultra_decompressor_expand_block(pCurFileData, nBlockDataSize, pCurOutBuffer, 0, (int)(pEndOutBuffer - pCurOutBuffer));
         else
            nDecompressedSize = lz4ultra_decompressor_expand_block(pCurFileData, nBlockDataSize, pCurOutBuffer - nPreviousBlockSize, nPreviousBlockSize, (int)(pEndOutBuffer - pCurOutBuffer + nPreviousBlockSize));
         if (nDecompressedSize < 0)
            return -1;

         pCurOutBuffer += nDecompressedSize;
         nPreviousBlockSize = nDecompressedSize;
      }
      else {
         /* Copy uncompressed block */
         if ((pCurFileData + nBlockDataSize) > pEndFileData)
            return -1;
         if ((pCurOutBuffer + nBlockDataSize) > pEndOutBuffer)
            return -1;
         memcpy(pCurOutBuffer, pCurFileData, nBlockDataSize);
         pCurOutBuffer += nBlockDataSize;
      }

      pCurFileData += nBlockDataSize;
   }

   return (int)(pCurOutBuffer - pOutBuffer);
}
