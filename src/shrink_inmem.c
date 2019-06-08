/*
 * shrink_inmem.c - in-memory compression implementation
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

#include <stdlib.h>
#include <string.h>
#include "shrink_inmem.h"
#include "frame.h"
#include "format.h"
#include "lib.h"

/**
 * Get maximum compressed size of input(source) data
 *
 * @param nInputSize input(source) size in bytes
 * @param nFlags compression flags (LZ4ULTRA_FLAG_xxx)
 * @param nBlockMaxCode maximum block size code (4..7 for 64 Kb..4 Mb)
 *
 * @return maximum compressed size
 */
size_t lz4ultra_get_max_compressed_size_inmem(size_t nInputSize, unsigned int nFlags, int nBlockMaxCode) {
   int nBlockMaxBits;
   int nBlockMaxSize;

   if (nFlags & LZ4ULTRA_FLAG_LEGACY_FRAMES) {
      nBlockMaxBits = 23;
   }
   else {
      nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   }
   nBlockMaxSize = 1 << nBlockMaxBits;

   if (nInputSize < nBlockMaxSize && (nFlags & LZ4ULTRA_FLAG_LEGACY_FRAMES) == 0) {
      /* If the entire input data is shorter than the specified block size, try to reduce the
       * block size until is the smallest one that can fit the data */

      do {
         nBlockMaxBits = 8 + (nBlockMaxCode << 1);
         nBlockMaxSize = 1 << nBlockMaxBits;

         int nPrevBlockMaxBits = 8 + ((nBlockMaxCode - 1) << 1);
         int nPrevBlockMaxSize = 1 << nPrevBlockMaxBits;
         if (nBlockMaxCode > 4 && nPrevBlockMaxSize > nInputSize) {
            nBlockMaxCode--;
         }
         else
            break;
      } while (1);
   }

   return LZ4ULTRA_MAX_HEADER_SIZE + ((nInputSize + (nBlockMaxSize - 1)) >> nBlockMaxBits) * LZ4ULTRA_FRAME_SIZE + nInputSize + LZ4ULTRA_FRAME_SIZE /* footer */;
}

/**
 * Compress memory
 *
 * @param pInputData pointer to input(source) data to compress
 * @param pOutBuffer buffer for compressed data
 * @param nInputSize input(source) size in bytes
 * @param nMaxOutBufferSize maximum capacity of compression buffer
 * @param nFlags compression flags (LZ4ULTRA_FLAG_xxx)
 * @param nBlockMaxCode maximum block size code (4..7 for 64 Kb..4 Mb)
 *
 * @return actual compressed size, or -1 for error
 */
size_t lz4ultra_compress_inmem(const unsigned char *pInputData, unsigned char *pOutBuffer, size_t nInputSize, size_t nMaxOutBufferSize, unsigned int nFlags, int nBlockMaxCode) {
   lz4ultra_compressor compressor;
   size_t nOriginalSize = 0L;
   size_t nCompressedSize = 0L;
   int nBlockMaxBits;
   int nBlockMaxSize;
   int nResult;
   int nError = 0;

   if (nFlags & LZ4ULTRA_FLAG_LEGACY_FRAMES) {
      nBlockMaxBits = 23;
      nFlags |= LZ4ULTRA_FLAG_INDEP_BLOCKS;
   }
   else {
      nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   }
   nBlockMaxSize = 1 << nBlockMaxBits;

   if (nInputSize < nBlockMaxSize && (nFlags & LZ4ULTRA_FLAG_LEGACY_FRAMES) == 0) {
      /* If the entire input data is shorter than the specified block size, try to reduce the
       * block size until is the smallest one that can fit the data */

      do {
         nBlockMaxBits = 8 + (nBlockMaxCode << 1);
         nBlockMaxSize = 1 << nBlockMaxBits;

         int nPrevBlockMaxBits = 8 + ((nBlockMaxCode - 1) << 1);
         int nPrevBlockMaxSize = 1 << nPrevBlockMaxBits;
         if (nBlockMaxCode > 4 && nPrevBlockMaxSize > nInputSize) {
            nBlockMaxCode--;
         }
         else
            break;
      } while (1);
   }

   nResult = lz4ultra_compressor_init(&compressor, nBlockMaxSize + HISTORY_SIZE, nFlags);
   if (nResult != 0) {
      return LZ4ULTRA_ERROR_MEMORY;
   }

   if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
      int nHeaderSize = lz4ultra_encode_header(pOutBuffer + nCompressedSize, (int)(nMaxOutBufferSize - nCompressedSize), nFlags, nBlockMaxCode);
      if (nHeaderSize < 0)
         nError = LZ4ULTRA_ERROR_COMPRESSION;
      else {
         nCompressedSize += nHeaderSize;
      }
   }

   int nPreviousBlockSize = 0;
   int nNumBlocks = 0;

   while (nOriginalSize < nInputSize && !nError) {
      int nInDataSize;

      nInDataSize = (int)(nInputSize - nOriginalSize);
      if (nInDataSize > nBlockMaxSize)
         nInDataSize = nBlockMaxSize;

      if (nInDataSize > 0) {
         if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) != 0 && (nNumBlocks || nInDataSize > 0x400000)) {
            nError = LZ4ULTRA_ERROR_RAW_TOOLARGE;
            break;
         }

         int nOutDataSize;
         int nOutDataEnd = (int)(nMaxOutBufferSize - LZ4ULTRA_FRAME_SIZE - LZ4ULTRA_FRAME_SIZE /* footer */ - nCompressedSize);

         if (nOutDataEnd > nBlockMaxSize)
            nOutDataEnd = nBlockMaxSize;

         nOutDataSize = lz4ultra_compressor_shrink_block(&compressor, pInputData + nOriginalSize - nPreviousBlockSize, nPreviousBlockSize, nInDataSize, pOutBuffer + LZ4ULTRA_FRAME_SIZE + nCompressedSize, nOutDataEnd);
         if (nOutDataSize >= 0) {
            int nFrameHeaderSize = 0;

            /* Compressed block */

            if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
               nFrameHeaderSize = lz4ultra_encode_compressed_block_frame(pOutBuffer + nCompressedSize, (int)(nMaxOutBufferSize - nCompressedSize), nFlags, nOutDataSize);
               if (nFrameHeaderSize < 0)
                  nError = LZ4ULTRA_ERROR_COMPRESSION;
            }

            if (!nError) {
               nOriginalSize += nInDataSize;
               nCompressedSize += nFrameHeaderSize + nOutDataSize;
            }
         }
         else {
            /* Write uncompressible, literal block */

            if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) != 0) {
               /* Uncompressible data isn't supported by raw blocks */
               nError = LZ4ULTRA_ERROR_RAW_UNCOMPRESSED;
               break;
            }

            int nFrameHeaderSize;

            nFrameHeaderSize = lz4ultra_encode_uncompressed_block_frame(pOutBuffer + nCompressedSize, (int)(nMaxOutBufferSize - nCompressedSize), nFlags, nInDataSize);
            if (nFrameHeaderSize < 0)
               nError = LZ4ULTRA_ERROR_COMPRESSION;
            else {
               if (nInDataSize > (nMaxOutBufferSize - (nCompressedSize + nFrameHeaderSize)))
                  nError = LZ4ULTRA_ERROR_DST;
               else {
                  memcpy(pOutBuffer + nFrameHeaderSize + nCompressedSize, pInputData + nOriginalSize, nInDataSize);
                  nOriginalSize += nInDataSize;
                  nCompressedSize += nFrameHeaderSize + (long long)nInDataSize;
               }
            }
         }

         if (!(nFlags & LZ4ULTRA_FLAG_INDEP_BLOCKS)) {
            nPreviousBlockSize = nInDataSize;
            if (nPreviousBlockSize > HISTORY_SIZE)
               nPreviousBlockSize = HISTORY_SIZE;
         }
         else {
            nPreviousBlockSize = 0;
         }

         nNumBlocks++;
      }
   }

   int nFooterSize;

   if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) != 0) {
      nFooterSize = 0;
   }
   else {
      nFooterSize = lz4ultra_encode_footer_frame(pOutBuffer + nCompressedSize, (int)(nMaxOutBufferSize - nCompressedSize), nFlags);
      if (nFooterSize < 0)
         nError = LZ4ULTRA_ERROR_COMPRESSION;
   }

   if (!nError) {
      nCompressedSize += nFooterSize;
   }


   lz4ultra_compressor_destroy(&compressor);

   if (nError) {
      return -1;
   }
   else {
      return nCompressedSize;
   }
}
