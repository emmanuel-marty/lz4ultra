/*
 * shrink_streaming.c - streaming compression implementation
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
#include "shrink_streaming.h"
#include "format.h"
#include "frame.h"
#include "lib.h"

/*-------------- File API -------------- */

/**
 * Compress file
 *
 * @param pszInFilename name of input(source) file to compress
 * @param pszOutFilename name of output(compressed) file to generate
 * @param pszDictionaryFilename name of dictionary file, or NULL for none
 * @param nFlags compression flags (LZ4ULTRA_FLAG_xxx)
 * @param nBlockMaxCode maximum block size code (4..7 for 64 Kb..4 Mb)
 * @param start start function, called when the max block size is finalized and compression is about to start, or NULL for none
 * @param progress progress function, called after compressing each block, or NULL for none
 * @param pOriginalSize pointer to returned input(source) size, updated when this function is successful
 * @param pCompressedSize pointer to returned output(compressed) size, updated when this function is successful
 * @param pCommandCount pointer to returned token(compression commands) count, updated when this function is successful
 *
 * @return LZ4ULTRA_OK for success, or an error value from lz4ultra_status_t
 */
lz4ultra_status_t lz4ultra_compress_file(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename,
                                         const unsigned int nFlags, int nBlockMaxCode,
                                         void(*start)(int nBlockMaxCode, const unsigned int nFlags),
                                         void(*progress)(long long nOriginalSize, long long nCompressedSize), long long *pOriginalSize, long long *pCompressedSize, int *pCommandCount) {
   lz4ultra_stream_t inStream, outStream;
   void *pDictionaryData = NULL;
   int nDictionaryDataSize = 0;
   lz4ultra_status_t nStatus;

   if (lz4ultra_filestream_open(&inStream, pszInFilename, "rb") < 0) {
      return LZ4ULTRA_ERROR_SRC;
   }

   if (lz4ultra_filestream_open(&outStream, pszOutFilename, "wb") < 0) {
      inStream.close(&inStream);
      return LZ4ULTRA_ERROR_DST;
   }

   nStatus = lz4ultra_dictionary_load(pszDictionaryFilename, &pDictionaryData, &nDictionaryDataSize);
   if (nStatus) {
      outStream.close(&outStream);
      inStream.close(&inStream);

      return nStatus;
   }

   nStatus = lz4ultra_compress_stream(&inStream, &outStream, pDictionaryData, nDictionaryDataSize, nFlags, nBlockMaxCode, start, progress, pOriginalSize, pCompressedSize, pCommandCount);
   
   lz4ultra_dictionary_free(&pDictionaryData);
   outStream.close(&outStream);
   inStream.close(&inStream);
   return nStatus;
}

/*-------------- Streaming API -------------- */

/**
 * Compress stream
 *
 * @param pInStream input(source) stream to compress
 * @param pOutStream output(compressed) stream to write to
 * @param pDictionaryData dictionary contents, or NULL for none
 * @param nDictionaryDataSize size of dictionary contents, or 0
 * @param nFlags compression flags (LZ4ULTRA_FLAG_xxx)
 * @param nBlockMaxCode maximum block size code (4..7 for 64 Kb..4 Mb)
 * @param start start function, called when the max block size is finalized and compression is about to start, or NULL for none
 * @param progress progress function, called after compressing each block, or NULL for none
 * @param pOriginalSize pointer to returned input(source) size, updated when this function is successful
 * @param pCompressedSize pointer to returned output(compressed) size, updated when this function is successful
 * @param pCommandCount pointer to returned token(compression commands) count, updated when this function is successful
 *
 * @return LZ4ULTRA_OK for success, or an error value from lz4ultra_status_t
 */
lz4ultra_status_t lz4ultra_compress_stream(lz4ultra_stream_t *pInStream, lz4ultra_stream_t *pOutStream, const void *pDictionaryData, int nDictionaryDataSize, unsigned int nFlags,
                                           int nBlockMaxCode,
                                           void(*start)(int nBlockMaxCode, const unsigned int nFlags),
                                           void(*progress)(long long nOriginalSize, long long nCompressedSize), long long *pOriginalSize, long long *pCompressedSize, int *pCommandCount) {
   unsigned char *pInData, *pOutData;
   lz4ultra_compressor compressor;
   long long nOriginalSize = 0LL, nCompressedSize = 0LL;
   int nBlockMaxBits;
   int nBlockMaxSize;
   int nPreloadedInDataSize;
   int nResult;
   unsigned char cFrameData[16];
   int nError = 0;

   memset(cFrameData, 0, 16);

   if (nFlags & LZ4ULTRA_FLAG_LEGACY_FRAMES) {
      nBlockMaxBits = 23;
      nFlags |= LZ4ULTRA_FLAG_INDEP_BLOCKS;
   }
   else {
      nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   }
   nBlockMaxSize = 1 << nBlockMaxBits;

   pInData = (unsigned char*)malloc(nBlockMaxSize + HISTORY_SIZE);
   if (!pInData) {
      return LZ4ULTRA_ERROR_MEMORY;
   }
   memset(pInData, 0, nBlockMaxSize + HISTORY_SIZE);

   pOutData = (unsigned char*)malloc(nBlockMaxSize);
   if (!pOutData) {
      free(pInData);
      pInData = NULL;

      return LZ4ULTRA_ERROR_MEMORY;
   }
   memset(pInData, 0, nBlockMaxSize);

   /* Load first block of input data */
   nPreloadedInDataSize = (int)pInStream->read(pInStream, pInData + HISTORY_SIZE, nBlockMaxSize);
   if (nPreloadedInDataSize < nBlockMaxSize && (nFlags & LZ4ULTRA_FLAG_LEGACY_FRAMES) == 0) {
      /* If the entire input data is shorter than the specified block size, try to reduce the
       * block size until is the smallest one that can fit the data */

      do {
         nBlockMaxBits = 8 + (nBlockMaxCode << 1);
         nBlockMaxSize = 1 << nBlockMaxBits;

         int nPrevBlockMaxBits = 8 + ((nBlockMaxCode - 1) << 1);
         int nPrevBlockMaxSize = 1 << nPrevBlockMaxBits;
         if (nBlockMaxCode > 4 && nPrevBlockMaxSize > nPreloadedInDataSize) {
            nBlockMaxCode--;
         }
         else
            break;
      } while (1);
   }

   nResult = lz4ultra_compressor_init(&compressor, nBlockMaxSize + HISTORY_SIZE, nFlags);
   if (nResult != 0) {
      free(pOutData);
      pOutData = NULL;

      free(pInData);
      pInData = NULL;

      return LZ4ULTRA_ERROR_MEMORY;
   }

   if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
      int nHeaderSize = lz4ultra_encode_header(cFrameData, 16, nFlags, nBlockMaxCode);
      if (nHeaderSize < 0)
         nError = LZ4ULTRA_ERROR_COMPRESSION;
      else {
         if (pOutStream->write(pOutStream, cFrameData, nHeaderSize) != nHeaderSize)
            nError = LZ4ULTRA_ERROR_DST;
         nCompressedSize += (long long)nHeaderSize;
      }
   }

   if (start)
      start(nBlockMaxCode, nFlags);

   int nPreviousBlockSize = 0;
   int nNumBlocks = 0;

   while ((nPreloadedInDataSize > 0 || (!pInStream->eof(pInStream))) && !nError) {
      int nInDataSize;

      if (nPreviousBlockSize) {
         memcpy(pInData + HISTORY_SIZE - nPreviousBlockSize, pInData + HISTORY_SIZE + (nBlockMaxSize - nPreviousBlockSize), nPreviousBlockSize);
      }
      else if (nDictionaryDataSize && pDictionaryData) {
         memcpy(pInData + HISTORY_SIZE - nDictionaryDataSize, pDictionaryData, nDictionaryDataSize);
         nPreviousBlockSize = nDictionaryDataSize;
      }

      if (nPreloadedInDataSize > 0) {
         nInDataSize = nPreloadedInDataSize;
         nPreloadedInDataSize = 0;
      }
      else {
         nInDataSize = (int)pInStream->read(pInStream, pInData + HISTORY_SIZE, nBlockMaxSize);
      }

      if (nInDataSize > 0) {
         if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) != 0 && (nNumBlocks || nInDataSize > 0x400000)) {
            nError = LZ4ULTRA_ERROR_RAW_TOOLARGE;
            break;
         }
         if (!(nFlags & LZ4ULTRA_FLAG_INDEP_BLOCKS))
            nDictionaryDataSize = 0;

         int nOutDataSize;

         nOutDataSize = lz4ultra_compressor_shrink_block(&compressor, pInData + HISTORY_SIZE - nPreviousBlockSize, nPreviousBlockSize, nInDataSize, pOutData, (nInDataSize >= nBlockMaxSize) ? nBlockMaxSize : nInDataSize);
         if (nOutDataSize >= 0) {
            int nFrameHeaderSize = 0;

            /* Write compressed block */

            if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
               nFrameHeaderSize = lz4ultra_encode_compressed_block_frame(cFrameData, 16, nFlags, nOutDataSize);
               if (nFrameHeaderSize < 0)
                  nError = LZ4ULTRA_ERROR_COMPRESSION;
               else {
                  if (pOutStream->write(pOutStream, cFrameData, nFrameHeaderSize) != (size_t)nFrameHeaderSize) {
                     nError = LZ4ULTRA_ERROR_DST;
                  }
               }
            }

            if (!nError) {
               if (pOutStream->write(pOutStream, pOutData, (size_t)nOutDataSize) != (size_t)nOutDataSize) {
                  nError = LZ4ULTRA_ERROR_DST;
               }
               else {
                  nOriginalSize += (long long)nInDataSize;
                  nCompressedSize += (long long)nFrameHeaderSize + (long long)nOutDataSize;
               }
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

            nFrameHeaderSize = lz4ultra_encode_uncompressed_block_frame(cFrameData, 16, nFlags, nInDataSize);
            if (nFrameHeaderSize < 0)
               nError = LZ4ULTRA_ERROR_COMPRESSION;
            else {
               if (pOutStream->write(pOutStream, cFrameData, nFrameHeaderSize) != (size_t)nFrameHeaderSize) {
                  nError = LZ4ULTRA_ERROR_DST;
               }
               else {
                  if (pOutStream->write(pOutStream, pInData + HISTORY_SIZE, (size_t)nInDataSize) != (size_t)nInDataSize) {
                     nError = LZ4ULTRA_ERROR_DST;
                  }
                  else {
                     nOriginalSize += (long long)nInDataSize;
                     nCompressedSize += (long long)nFrameHeaderSize + (long long)nInDataSize;
                  }
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

      if (!nError && !pInStream->eof(pInStream)) {
         if (progress)
            progress(nOriginalSize, nCompressedSize);
      }
   }

   int nFooterSize;

   if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) != 0) {
      nFooterSize = 0;
   }
   else {
      nFooterSize = lz4ultra_encode_footer_frame(cFrameData, 16, nFlags);
      if (nFooterSize < 0)
         nError = LZ4ULTRA_ERROR_COMPRESSION;
   }

   if (!nError) {
      if (pOutStream->write(pOutStream, cFrameData, nFooterSize) != nFooterSize)
         nError = LZ4ULTRA_ERROR_DST;
   }
   nCompressedSize += (long long)nFooterSize;

   if (progress)
      progress(nOriginalSize, nCompressedSize);

   int nCommandCount = lz4ultra_compressor_get_command_count(&compressor);
   lz4ultra_compressor_destroy(&compressor);

   free(pOutData);
   pOutData = NULL;

   free(pInData);
   pInData = NULL;

   if (nError) {
      return nError;
   }
   else {
      if (pOriginalSize)
         *pOriginalSize = nOriginalSize;
      if (pCompressedSize)
         *pCompressedSize = nCompressedSize;
      if (pCommandCount)
         *pCommandCount = nCommandCount;
      return LZ4ULTRA_OK;
   }
}
