/*
 * expand_streaming.c - streaming decompression implementation
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
#include "expand_streaming.h"
#include "format.h"
#include "frame.h"
#include "lib.h"

/*-------------- File API -------------- */

/**
 * Decompress file
 *
 * @param pszInFilename name of input(compressed) file to decompress
 * @param pszOutFilename name of output(decompressed) file to generate
 * @param pszDictionaryFilename name of dictionary file, or NULL for none
 * @param nFlags compression flags (LZ4ULTRA_FLAG_RAW_BLOCK to decompress a raw block, or 0)
 * @param pOriginalSize pointer to returned output(decompressed) size, updated when this function is successful
 * @param pCompressedSize pointer to returned input(compressed) size, updated when this function is successful
 *
 * @return LZ4ULTRA_OK for success, or an error value from lz4ultra_status_t
 */
lz4ultra_status_t lz4ultra_decompress_file(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename, const unsigned int nFlags,
                                           long long *pOriginalSize, long long *pCompressedSize) {
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

   nStatus = lz4ultra_decompress_stream(&inStream, &outStream, pDictionaryData, nDictionaryDataSize, nFlags, pOriginalSize, pCompressedSize);

   lz4ultra_dictionary_free(&pDictionaryData);
   outStream.close(&outStream);
   inStream.close(&inStream);
   
   return nStatus;
}

/*-------------- Streaming API -------------- */

/**
 * Decompress stream
 *
 * @param pInStream input(compressed) stream to decompress
 * @param pOutStream output(decompressed) stream to write to
 * @param pDictionaryData dictionary contents, or NULL for none
 * @param nDictionaryDataSize size of dictionary contents, or 0
 * @param pOriginalSize pointer to returned output(decompressed) size, updated when this function is successful
 * @param pCompressedSize pointer to returned input(compressed) size, updated when this function is successful
 *
 * @return LZ4ULTRA_OK for success, or an error value from lz4ultra_status_t
 */
lz4ultra_status_t lz4ultra_decompress_stream(lz4ultra_stream_t *pInStream, lz4ultra_stream_t *pOutStream, const void *pDictionaryData, int nDictionaryDataSize, unsigned int nFlags,
                                             long long *pOriginalSize, long long *pCompressedSize) {
   long long nOriginalSize = 0LL;
   long long nCompressedSize = 0LL;
   int nBlockMaxCode = 7;
   unsigned char cFrameData[16];
   unsigned char *pInBlock;
   unsigned char *pOutData;

   if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
      memset(cFrameData, 0, 16);

      if (pInStream->read(pInStream, cFrameData, LZ4ULTRA_HEADER_SIZE) != LZ4ULTRA_HEADER_SIZE) {
         return LZ4ULTRA_ERROR_SRC;
      }

      int nExtraHeaderSize = lz4ultra_check_header(cFrameData, LZ4ULTRA_HEADER_SIZE);
      if (nExtraHeaderSize < 0)
         return LZ4ULTRA_ERROR_FORMAT;

      if (pInStream->read(pInStream, cFrameData + LZ4ULTRA_HEADER_SIZE, nExtraHeaderSize) != nExtraHeaderSize) {
         return LZ4ULTRA_ERROR_SRC;
      }

      int nSuccess = lz4ultra_decode_header(cFrameData, LZ4ULTRA_HEADER_SIZE + nExtraHeaderSize, &nBlockMaxCode, &nFlags);
      if (nSuccess < 0) {
         if (nSuccess == LZ4ULTRA_DECODE_ERR_SUM)
            return LZ4ULTRA_ERROR_CHECKSUM;
         else
            return LZ4ULTRA_ERROR_FORMAT;
      }

      nCompressedSize += (long long)(LZ4ULTRA_HEADER_SIZE + nExtraHeaderSize);
   }

   int nBlockMaxBits;
   if (nFlags & LZ4ULTRA_FLAG_LEGACY_FRAMES)
      nBlockMaxBits = 23;
   else
      nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   int nBlockMaxSize = 1 << nBlockMaxBits;

   pInBlock = (unsigned char*)malloc(nBlockMaxSize);
   if (!pInBlock) {
      return LZ4ULTRA_ERROR_MEMORY;
   }

   pOutData = (unsigned char*)malloc(nBlockMaxSize + HISTORY_SIZE);
   if (!pOutData) {
      free(pInBlock);
      pInBlock = NULL;

      return LZ4ULTRA_ERROR_MEMORY;
   }

   int nDecompressionError = 0;
   int nPrevDecompressedSize = 0;
   int nNumBlocks = 0;

   while (!pInStream->eof(pInStream) && !nDecompressionError) {
      unsigned int nBlockSize = 0;
      int nIsUncompressed = 0;

      if (nPrevDecompressedSize != 0) {
         memcpy(pOutData + HISTORY_SIZE - nPrevDecompressedSize, pOutData + HISTORY_SIZE + (nBlockMaxSize - nPrevDecompressedSize), nPrevDecompressedSize);
      }
      else if (nDictionaryDataSize != 0) {
         memcpy(pOutData + HISTORY_SIZE - nDictionaryDataSize, pDictionaryData, nDictionaryDataSize);
         nPrevDecompressedSize = nDictionaryDataSize;

         if (!(nFlags & LZ4ULTRA_FLAG_INDEP_BLOCKS))
            nDictionaryDataSize = 0;
      }

      if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
         memset(cFrameData, 0, 16);
         if (pInStream->read(pInStream, cFrameData, LZ4ULTRA_FRAME_SIZE) == LZ4ULTRA_FRAME_SIZE) {
            int nSuccess = lz4ultra_decode_frame(cFrameData, LZ4ULTRA_FRAME_SIZE, nFlags, &nBlockSize, &nIsUncompressed);
            if (nSuccess < 0)
               nBlockSize = 0;

            nCompressedSize += (long long)LZ4ULTRA_FRAME_SIZE;
         }
         else {
            nBlockSize = 0;
         }
      }
      else {
         if (!nNumBlocks)
            nBlockSize = nBlockMaxSize;
         else
            nBlockSize = 0;
      }

      if (nBlockSize != 0) {
         int nDecompressedSize = 0;

         if ((int)nBlockSize > nBlockMaxSize) {
            nDecompressionError = LZ4ULTRA_ERROR_FORMAT;
            break;
         }
         size_t nReadBytes = pInStream->read(pInStream, pInBlock, nBlockSize);
         if (nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) {
            if (nReadBytes > 2)
               nReadBytes -= 2;
            else
               nReadBytes = 0;
            nBlockSize = (unsigned int)nReadBytes;
         }

         if (nReadBytes == nBlockSize) {
            nCompressedSize += (long long)nReadBytes;

            if (nIsUncompressed) {
               memcpy(pOutData + HISTORY_SIZE, pInBlock, nBlockSize);
               nDecompressedSize = nBlockSize;
            }
            else {
               nDecompressedSize = lz4ultra_decompressor_expand_block(pInBlock, nBlockSize, pOutData, HISTORY_SIZE, nBlockMaxSize);
               if (nDecompressedSize < 0) {
                  nDecompressionError = LZ4ULTRA_ERROR_DECOMPRESSION;
                  break;
               }
            }

            if (nDecompressedSize != 0) {
               nOriginalSize += (long long)nDecompressedSize;

               if (pOutStream->write(pOutStream, pOutData + HISTORY_SIZE, nDecompressedSize) != nDecompressedSize)
                  nDecompressionError = LZ4ULTRA_ERROR_DST;

               if (!(nFlags & LZ4ULTRA_FLAG_INDEP_BLOCKS)) {
                  nPrevDecompressedSize = nDecompressedSize;
                  if (nPrevDecompressedSize > HISTORY_SIZE)
                     nPrevDecompressedSize = HISTORY_SIZE;
               }
               else {
                  nPrevDecompressedSize = 0;
               }
               nDecompressedSize = 0;
            }
         }
         else {
            break;
         }

         nNumBlocks++;
      }
      else {
         break;
      }
   }

   free(pOutData);
   pOutData = NULL;

   free(pInBlock);
   pInBlock = NULL;

   *pOriginalSize = nOriginalSize;
   *pCompressedSize = nCompressedSize;
   return nDecompressionError;
}
