/*
 * lib.c - lz4ultra library implementation
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
#include "lib.h"
#include "matchfinder.h"
#include "frame.h"
#include "shrink.h"
#include "expand.h"
#include "format.h"

#define HISTORY_SIZE 65536

/*-------------- Top level API -------------- */

/**
 * Compress file
 *
 * @param pszInFilename name of input(source) file to compress
 * @param pszOutFilename name of output(compressed) file to generate
 * @param pszDictionaryFilename name of dictionary file, or NULL for none
 * @param nFlags compression flags (LZ4ULTRA_FLAG_xxx)
 * @param nBlockMaxCode maximum block size code (4..7 for 64 Kb..4 Mb)
 * @param nIsIndependentBlocks nonzero to compress using independent blocks, 0 to compress with inter-block back references
 * @param start start function, called when the max block size is finalized and compression is about to start, or NULL for none
 * @param progress progress function, called after compressing each block, or NULL for none
 * @param pOriginalSize pointer to returned input(source) size, updated when this function is successful
 * @param pCompressedSize pointer to returned output(compressed) size, updated when this function is successful
 * @param pCommandCount pointer to returned token(compression commands) count, updated when this function is successful
 *
 * @return LZ4ULTRA_OK for success, or an error value from lz4ultra_status_t
 */
lz4ultra_status_t lz4ultra_compress_file(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename,
                                         const unsigned int nFlags, int nBlockMaxCode, int nIsIndependentBlocks,
                                         void(*start)(int nBlockMaxCode, int nIsIndependentBlocks),
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

   nStatus = lz4ultra_compress_stream(&inStream, &outStream, pDictionaryData, nDictionaryDataSize, nFlags, nBlockMaxCode, nIsIndependentBlocks, start, progress, pOriginalSize, pCompressedSize, pCommandCount);
   
   lz4ultra_dictionary_free(&pDictionaryData);
   outStream.close(&outStream);
   inStream.close(&inStream);
   return nStatus;
}

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
 * Load dictionary contents
 *
 * @param pszDictionaryFilename name of dictionary file, or NULL for none
 * @param pDictionaryData pointer to returned dictionary contents, or NULL for none
 * @param nDictionaryDataSize pointer to returned size of dictionary contents, or 0
 *
 * @return LZSA_OK for success, or an error value from lz4ultra_status_t
 */
int lz4ultra_dictionary_load(const char *pszDictionaryFilename, void **ppDictionaryData, int *pDictionaryDataSize) {
   unsigned char *pDictionaryData = NULL;
   int nDictionaryDataSize = 0;


   if (pszDictionaryFilename) {
      pDictionaryData = (unsigned char *)malloc(HISTORY_SIZE);
      if (!pDictionaryData) {
         return LZ4ULTRA_ERROR_MEMORY;
      }

      FILE *f_dictionary = fopen(pszDictionaryFilename, "rb");
      if (!f_dictionary) {
         free(pDictionaryData);
         pDictionaryData = NULL;

         return LZ4ULTRA_ERROR_DICTIONARY;
      }

      fseek(f_dictionary, 0, SEEK_END);
#ifdef _WIN32
      __int64 nDictionaryFileSize = _ftelli64(f_dictionary);
#else
      off_t nDictionaryFileSize = ftello(f_dictionary);
#endif
      if (nDictionaryFileSize > HISTORY_SIZE) {
         /* Use the last HISTORY_SIZE bytes of the dictionary */
         fseek(f_dictionary, -HISTORY_SIZE, SEEK_END);
      }
      else {
         fseek(f_dictionary, 0, SEEK_SET);
      }

      nDictionaryDataSize = (int)fread(pDictionaryData, 1, HISTORY_SIZE, f_dictionary);
      if (nDictionaryDataSize < 0)
         nDictionaryDataSize = 0;

      fclose(f_dictionary);
      f_dictionary = NULL;
   }

   *ppDictionaryData = pDictionaryData;
   *pDictionaryDataSize = nDictionaryDataSize;
   return LZ4ULTRA_OK;
}

/**
 * Free dictionary contents
 *
 * @param pDictionaryData pointer to pointer to dictionary contents
 */
void lz4ultra_dictionary_free(void **ppDictionaryData) {
   if (*ppDictionaryData) {
      free(*ppDictionaryData);
      ppDictionaryData = NULL;
   }
}

/**
 * Compress stream
 *
 * @param pInStream input(source) stream to compress
 * @param pOutStream output(compressed) stream to write to
 * @param pDictionaryData dictionary contents, or NULL for none
 * @param nDictionaryDataSize size of dictionary contents, or 0
 * @param nFlags compression flags (LZ4ULTRA_FLAG_xxx)
 * @param nBlockMaxCode maximum block size code (4..7 for 64 Kb..4 Mb)
 * @param nIsIndependentBlocks nonzero to compress using independent blocks, 0 to compress with inter-block back references
 * @param start start function, called when the max block size is finalized and compression is about to start, or NULL for none
 * @param progress progress function, called after compressing each block, or NULL for none
 * @param pOriginalSize pointer to returned input(source) size, updated when this function is successful
 * @param pCompressedSize pointer to returned output(compressed) size, updated when this function is successful
 * @param pCommandCount pointer to returned token(compression commands) count, updated when this function is successful
 *
 * @return LZ4ULTRA_OK for success, or an error value from lz4ultra_status_t
 */
lz4ultra_status_t lz4ultra_compress_stream(lz4ultra_stream_t *pInStream, lz4ultra_stream_t *pOutStream, const void *pDictionaryData, int nDictionaryDataSize, const unsigned int nFlags,
                                           int nBlockMaxCode, int nIsIndependentBlocks,
                                           void(*start)(int nBlockMaxCode, int nIsIndependentBlocks),
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

   nBlockMaxBits = 8 + (nBlockMaxCode << 1);
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
   if (nPreloadedInDataSize < nBlockMaxSize) {
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
      int nHeaderSize = lz4ultra_encode_header(cFrameData, 16, nBlockMaxCode, nIsIndependentBlocks);
      if (nHeaderSize < 0)
         nError = LZ4ULTRA_ERROR_COMPRESSION;
      else {
         if (pOutStream->write(pOutStream, cFrameData, nHeaderSize) != nHeaderSize)
            nError = LZ4ULTRA_ERROR_DST;
         nCompressedSize += (long long)nHeaderSize;
      }
   }

   if (start)
      start(nBlockMaxCode, nIsIndependentBlocks);

   int nPreviousBlockSize = 0;
   int nNumBlocks = 0;

   while ((nPreloadedInDataSize > 0 || (!pInStream->eof(pInStream))) && !nError) {
      int nInDataSize;

      if (nPreviousBlockSize) {
         memcpy(pInData + HISTORY_SIZE - nPreviousBlockSize, pInData + HISTORY_SIZE + (nBlockMaxSize - HISTORY_SIZE), nPreviousBlockSize);
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
         if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) != 0 && (nNumBlocks || nInDataSize > 65536)) {
            nError = LZ4ULTRA_ERROR_RAW_TOOLARGE;
            break;
         }
         if (!nIsIndependentBlocks)
            nDictionaryDataSize = 0;

         int nOutDataSize;

         nOutDataSize = lz4ultra_compressor_shrink_block(&compressor, pInData + HISTORY_SIZE - nPreviousBlockSize, nPreviousBlockSize, nInDataSize, pOutData, (nInDataSize >= nBlockMaxSize) ? nBlockMaxSize : nInDataSize);
         if (nOutDataSize >= 0) {
            int nFrameHeaderSize = 0;

            /* Write compressed block */

            if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
               nFrameHeaderSize = lz4ultra_encode_compressed_block_frame(cFrameData, 16, nOutDataSize);
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

            nFrameHeaderSize = lz4ultra_encode_uncompressed_block_frame(cFrameData, 16, nInDataSize);
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

         if (!nIsIndependentBlocks) {
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
      nFooterSize = lz4ultra_encode_footer_frame(cFrameData, 16);
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
lz4ultra_status_t lz4ultra_decompress_stream(lz4ultra_stream_t *pInStream, lz4ultra_stream_t *pOutStream, const void *pDictionaryData, int nDictionaryDataSize, const unsigned int nFlags,
                                             long long *pOriginalSize, long long *pCompressedSize) {
   long long nOriginalSize = 0LL;
   long long nCompressedSize = 0LL;
   int nBlockMaxCode = 4;
   int nIsIndependentBlocks = 0;
   unsigned char cFrameData[16];
   unsigned char *pInBlock;
   unsigned char *pOutData;

   if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
      memset(cFrameData, 0, 16);

      if (pInStream->read(pInStream, cFrameData, LZ4ULTRA_HEADER_SIZE) != LZ4ULTRA_HEADER_SIZE) {
         return LZ4ULTRA_ERROR_SRC;
      }

      int nSuccess = lz4ultra_decode_header(cFrameData, LZ4ULTRA_HEADER_SIZE, &nBlockMaxCode, &nIsIndependentBlocks);
      if (nSuccess < 0) {
         if (nSuccess == LZ4ULTRA_DECODE_ERR_SUM)
            return LZ4ULTRA_ERROR_CHECKSUM;
         else
            return LZ4ULTRA_ERROR_FORMAT;
      }

      nCompressedSize += (long long)LZ4ULTRA_HEADER_SIZE;
   }

   int nBlockMaxBits = 8 + (nBlockMaxCode << 1);
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
         memcpy(pOutData + HISTORY_SIZE - nPrevDecompressedSize, pOutData + HISTORY_SIZE + (nBlockMaxSize - HISTORY_SIZE), nPrevDecompressedSize);
      }
      else if (nDictionaryDataSize != 0) {
         memcpy(pOutData + HISTORY_SIZE - nDictionaryDataSize, pDictionaryData, nDictionaryDataSize);
         nPrevDecompressedSize = nDictionaryDataSize;

         if (!nIsIndependentBlocks)
            nDictionaryDataSize = 0;
      }

      if ((nFlags & LZ4ULTRA_FLAG_RAW_BLOCK) == 0) {
         memset(cFrameData, 0, 16);
         if (pInStream->read(pInStream, cFrameData, LZ4ULTRA_FRAME_SIZE) == LZ4ULTRA_FRAME_SIZE) {
            int nSuccess = lz4ultra_decode_frame(cFrameData, LZ4ULTRA_FRAME_SIZE, &nBlockSize, &nIsUncompressed);
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
               unsigned int nBlockOffs = 0;

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

               if (!nIsIndependentBlocks) {
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

/*-------------- Block compression API --------------*/

/**
 * Initialize compression context
 *
 * @param pCompressor compression context to initialize
 * @param nMaxWindowSize maximum size of input data window (previously compressed bytes + bytes to compress)
 * @param nFlags compression flags
 *
 * @return 0 for success, non-zero for failure
 */
int lz4ultra_compressor_init(lz4ultra_compressor *pCompressor, const int nMaxWindowSize, const int nFlags) {
   int nResult;

   nResult = divsufsort_init(&pCompressor->divsufsort_context);
   pCompressor->intervals = NULL;
   pCompressor->pos_data = NULL;
   pCompressor->open_intervals = NULL;
   pCompressor->match = NULL;
   pCompressor->flags = nFlags;
   pCompressor->num_commands = 0;

   if (!nResult) {
      pCompressor->intervals = (unsigned long long *)malloc(nMaxWindowSize * sizeof(unsigned long long));

      if (pCompressor->intervals) {
         pCompressor->pos_data = (unsigned long long *)malloc(nMaxWindowSize * sizeof(unsigned long long));

         if (pCompressor->pos_data) {
            pCompressor->open_intervals = (unsigned long long *)malloc((LCP_MAX + 1) * sizeof(unsigned long long));

            if (pCompressor->open_intervals) {
               pCompressor->match = (lz4ultra_match *)malloc(nMaxWindowSize * NMATCHES_PER_OFFSET * sizeof(lz4ultra_match));

               if (pCompressor->match)
                  return 0;
            }
         }
      }
   }

   lz4ultra_compressor_destroy(pCompressor);
   return 100;
}

/**
 * Clean up compression context and free up any associated resources
 *
 * @param pCompressor compression context to clean up
 */
void lz4ultra_compressor_destroy(lz4ultra_compressor *pCompressor) {
   divsufsort_destroy(&pCompressor->divsufsort_context);

   if (pCompressor->match) {
      free(pCompressor->match);
      pCompressor->match = NULL;
   }

   if (pCompressor->open_intervals) {
      free(pCompressor->open_intervals);
      pCompressor->open_intervals = NULL;
   }

   if (pCompressor->pos_data) {
      free(pCompressor->pos_data);
      pCompressor->pos_data = NULL;
   }

   if (pCompressor->intervals) {
      free(pCompressor->intervals);
      pCompressor->intervals = NULL;
   }
}

/**
 * Compress one block of data
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param nPreviousBlockSize number of previously compressed bytes (or 0 for none)
 * @param nInDataSize number of input bytes to compress
 * @param pOutData pointer to output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 *
 * @return size of compressed data in output buffer, or -1 if the data is uncompressible
 */
int lz4ultra_compressor_shrink_block(lz4ultra_compressor *pCompressor, const unsigned char *pInWindow, const int nPreviousBlockSize, const int nInDataSize, unsigned char *pOutData, const int nMaxOutDataSize) {
   if (lz4ultra_build_suffix_array(pCompressor, pInWindow, nPreviousBlockSize + nInDataSize))
      return -1;
   if (nPreviousBlockSize) {
      lz4ultra_skip_matches(pCompressor, 0, nPreviousBlockSize);
   }
   lz4ultra_find_all_matches(pCompressor, nPreviousBlockSize, nPreviousBlockSize + nInDataSize);
   return lz4ultra_optimize_and_write_block_lz4(pCompressor, pInWindow, nPreviousBlockSize, nInDataSize, pOutData, nMaxOutDataSize);
}

/**
 * Get the number of compression commands issued in compressed data blocks
 *
 * @return number of commands
 */
int lz4ultra_compressor_get_command_count(lz4ultra_compressor *pCompressor) {
   return pCompressor->num_commands;
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
int lz4ultra_decompressor_expand_block(const unsigned char *pInBlock, int nBlockSize, unsigned char *pOutData, int nOutDataOffset, int nBlockMaxSize) {
   return lz4ultra_decompressor_expand_block_lz4(pInBlock, nBlockSize, pOutData, nOutDataOffset, nBlockMaxSize);
}
