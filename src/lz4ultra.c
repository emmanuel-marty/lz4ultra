/*
 * main.c - command line optimal compression utility for the lz4 format
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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <sys/timeb.h>
#else
#include <sys/time.h>
#endif
#include "frame.h"
#include "lib.h"
#include "xxhash.h"

#define HISTORY_SIZE 65536
#define OPT_VERBOSE 1
#define OPT_RAW     2

#define TOOL_VERSION "1.1.0"

/*---------------------------------------------------------------------------*/

static long long do_get_time() {
   long long nTime;

#ifdef _WIN32
   struct _timeb tb;
   _ftime(&tb);

   nTime = ((long long)tb.time * 1000LL + (long long)tb.millitm) * 1000LL;
#else
   struct timeval tm;
   gettimeofday(&tm, NULL);

   nTime = (long long)tm.tv_sec * 1000000LL + (long long)tm.tv_usec;
#endif
   return nTime;
}

/*---------------------------------------------------------------------------*/

static int do_compress(const char *pszInFilename, const char *pszOutFilename, const unsigned int nOptions, int nBlockMaxCode, bool bIndependentBlocks) {
   FILE *f_in, *f_out;
   unsigned char *pInData, *pOutData;
   lz4ultra_compressor compressor;
   long long nStartTime = 0LL, nEndTime = 0LL;
   long long nOriginalSize = 0LL, nCompressedSize = 0LL;
   long long nFileSize = 0LL;
   int nBlockMaxBits;
   int nBlockMaxSize;
   int nFlags;
   int nResult;
   unsigned char cFrameData[16];
   bool bError = false;

   memset(cFrameData, 0, 16);

   f_in = fopen(pszInFilename, "rb");
   if (!f_in) {
      fprintf(stderr, "error opening '%s' for reading\n", pszInFilename);
      return 100;
   }

   f_out = fopen(pszOutFilename, "wb");
   if (!f_out) {
      fprintf(stderr, "error opening '%s' for writing\n", pszOutFilename);
      return 100;
   }

   fseek(f_in, 0, SEEK_END);
#ifdef _WIN32
   nFileSize = (long long)_ftelli64(f_in);
#else
   nFileSize = (long long)ftell(f_in);
#endif
   fseek(f_in, 0, SEEK_SET);

   do {
      nBlockMaxBits = 8 + (nBlockMaxCode << 1);
      nBlockMaxSize = 1 << nBlockMaxBits;
      if (nBlockMaxCode > 4 && nBlockMaxSize > nFileSize) {
         nBlockMaxCode--;
      }
      else
         break;
   } while (1);

   pInData = (unsigned char*)malloc(nBlockMaxSize + HISTORY_SIZE);
   if (!pInData) {
      fclose(f_out);
      f_out = NULL;

      fclose(f_in);
      f_in = NULL;

      fprintf(stderr, "out of memory\n");
      return 100;
   }
   memset(pInData, 0, nBlockMaxSize + HISTORY_SIZE);

   pOutData = (unsigned char*)malloc(nBlockMaxSize);
   if (!pOutData) {
      free(pInData);
      pInData = NULL;

      fclose(f_out);
      f_out = NULL;

      fclose(f_in);
      f_in = NULL;

      fprintf(stderr, "out of memory\n");
      return 100;
   }
   memset(pInData, 0, nBlockMaxSize);

   nFlags = 0;
   if (nOptions & OPT_RAW)
      nFlags |= LZ4ULTRA_FLAG_RAW_BLOCK;

   nResult = lz4ultra_compressor_init(&compressor, nBlockMaxSize + HISTORY_SIZE, nFlags);
   if (nResult != 0) {
      free(pOutData);
      pOutData = NULL;

      free(pInData);
      pInData = NULL;

      fclose(f_out);
      f_out = NULL;

      fclose(f_in);
      f_in = NULL;

      fprintf(stderr, "error initializing compressor\n");
      return 100;
   }

   if ((nOptions & OPT_RAW) == 0) {
      int nHeaderSize = lz4ultra_encode_header(cFrameData, 16, nBlockMaxCode, bIndependentBlocks);

      bError = fwrite(cFrameData, 1, nHeaderSize, f_out) != nHeaderSize;
      nCompressedSize += (long long)nHeaderSize;
   }

   if (nOptions & OPT_VERBOSE) {
      nStartTime = do_get_time();
      fprintf(stdout, "Use %d Kb blocks, independent blocks: %s\n", nBlockMaxSize >> 10, bIndependentBlocks ? "yes" : "no");
   }

   int nPreviousBlockSize = 0;

   while (!feof(f_in) && !bError) {
      int nInDataSize;

      if (nPreviousBlockSize) {
         memcpy(pInData, pInData + HISTORY_SIZE + (nBlockMaxSize - HISTORY_SIZE), nPreviousBlockSize);
      }

      nInDataSize = (int)fread(pInData + HISTORY_SIZE, 1, nBlockMaxSize, f_in);
      if (nInDataSize > 0) {
         if (nPreviousBlockSize && (nOptions & OPT_RAW) != 0) {
            fprintf(stderr, "error: raw blocks can only be used with files <= 64 Kb\n");
            bError = true;
            break;
         }

         int nOutDataSize;

         nOutDataSize = lz4ultra_shrink_block(&compressor, pInData + HISTORY_SIZE - nPreviousBlockSize, nPreviousBlockSize, nInDataSize, pOutData, (nInDataSize >= nBlockMaxSize) ? nBlockMaxSize : nInDataSize);
         if (nOutDataSize >= 0) {
            int nFrameHeaderSize = 0;

            /* Write compressed block */

            if ((nOptions & OPT_RAW) == 0) {
               nFrameHeaderSize = lz4ultra_encode_compressed_block_frame(cFrameData, 16, nOutDataSize);
               if (fwrite(cFrameData, 1, nFrameHeaderSize, f_out) != (size_t)nFrameHeaderSize) {
                  bError = true;
               }
            }

            if (!bError) {
               if (fwrite(pOutData, 1, (size_t)nOutDataSize, f_out) != (size_t)nOutDataSize) {
                  bError = true;
               }
               else {
                  nOriginalSize += (long long)nInDataSize;
                  nCompressedSize += (long long)nFrameHeaderSize + (long long)nOutDataSize;
               }
            }
         }
         else {
            /* Write uncompressible, literal block */

            if ((nOptions & OPT_RAW) != 0) {
               fprintf(stderr, "error: data is incompressible, raw blocks only support compressed data\n");
               bError = true;
               break;
            }

            int nFrameHeaderSize;

            nFrameHeaderSize = lz4ultra_encode_uncompressed_block_frame(cFrameData, 16, nInDataSize);

            if (fwrite(cFrameData, 1, nFrameHeaderSize, f_out) != (size_t)nFrameHeaderSize) {
               bError = true;
            }
            else {
               if (fwrite(pInData + HISTORY_SIZE, 1, (size_t)nInDataSize, f_out) != (size_t)nInDataSize) {
                  bError = true;
               }
               else {
                  nOriginalSize += (long long)nInDataSize;
                  nCompressedSize += (long long)nFrameHeaderSize + (long long)nInDataSize;
               }
            }
         }

         if (!bIndependentBlocks) {
            nPreviousBlockSize = nInDataSize;
            if (nPreviousBlockSize > HISTORY_SIZE)
               nPreviousBlockSize = HISTORY_SIZE;
         }
         else {
            nPreviousBlockSize = 0;
         }
      }

      if (!bError && !feof(f_in) && nOriginalSize >= 1024 * 1024) {
         fprintf(stdout, "\r%lld => %lld (%g %%)", nOriginalSize, nCompressedSize, (double)(nCompressedSize * 100.0 / nOriginalSize));
         fflush(stdout);
      }
   }

   int nFooterSize;

   if ((nOptions & OPT_RAW) != 0) {
      nFooterSize = 0;
   }
   else {
      nFooterSize = lz4ultra_encode_footer_frame(cFrameData, 16);
   }

   if (!bError)
      bError = fwrite(cFrameData, 1, nFooterSize, f_out) != nFooterSize;
   nCompressedSize += (long long)nFooterSize;

   if (!bError && (nOptions & OPT_VERBOSE)) {
      nEndTime = do_get_time();

      double fDelta = ((double)(nEndTime - nStartTime)) / 1000000.0;
      double fSpeed = ((double)nOriginalSize / 1048576.0) / fDelta;
      int nCommands = lz4ultra_compressor_get_command_count(&compressor);
      fprintf(stdout, "\rCompressed '%s' in %g seconds, %.02g Mb/s, %d tokens (%lld bytes/token), %lld into %lld bytes ==> %g %%\n",
         pszInFilename, fDelta, fSpeed, nCommands, nOriginalSize / ((long long)nCommands),
         nOriginalSize, nCompressedSize, (double)(nCompressedSize * 100.0 / nOriginalSize));
   }

   lz4ultra_compressor_destroy(&compressor);

   free(pOutData);
   pOutData = NULL;

   free(pInData);
   pInData = NULL;

   fclose(f_out);
   f_out = NULL;

   fclose(f_in);
   f_in = NULL;

   if (bError) {
      fprintf(stderr, "\rcompression error for '%s'\n", pszInFilename);
      return 100;
   }
   else {
      return 0;
   }
}

/*---------------------------------------------------------------------------*/

static int do_decompress(const char *pszInFilename, const char *pszOutFilename, const unsigned int nOptions) {
   long long nStartTime = 0LL, nEndTime = 0LL;
   long long nOriginalSize = 0LL;
   unsigned int nFileSize = 0;
   int nBlockMaxCode = 4;
   bool bIndependentBlocks = false;
   unsigned char cFrameData[16];

   FILE *pInFile = fopen(pszInFilename, "rb");
   if (!pInFile) {
      fprintf(stderr, "error opening input file\n");
      return 100;
   }

   if ((nOptions & OPT_RAW) == 0) {
      memset(cFrameData, 0, 16);

      if (fread(cFrameData, 1, LZ4ULTRA_HEADER_SIZE, pInFile) != LZ4ULTRA_HEADER_SIZE) {
         fclose(pInFile);
         pInFile = NULL;
         fprintf(stderr, "error reading header in input file\n");
         return 100;
      }


      int nSuccess = lz4ultra_decode_header(cFrameData, LZ4ULTRA_HEADER_SIZE, &nBlockMaxCode, &bIndependentBlocks);
      if (nSuccess < 0) {
         fclose(pInFile);
         pInFile = NULL;
         if (nSuccess == LZ4ULTRA_DECODE_ERR_SUM)
            fprintf(stderr, "invalid checksum in input file\n");
         else
            fprintf(stderr, "invalid magic number, version, flags, or block size in input file\n");
         return 100;
      }
   }
   else {
      fseek(pInFile, 0, SEEK_END);
      nFileSize = (unsigned int)ftell(pInFile);
      fseek(pInFile, 0, SEEK_SET);

      if (nFileSize < 2) {
         fclose(pInFile);
         pInFile = NULL;
         fprintf(stderr, "invalid file size for raw block mode\n");
         return 100;
      }
   }

   FILE *pOutFile = fopen(pszOutFilename, "wb");
   if (!pOutFile) {
      fclose(pInFile);
      pInFile = NULL;
      fprintf(stderr, "error opening output file\n");
      return 100;
   }

   unsigned char *pInBlock;
   unsigned char *pOutData;
   int nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   int nBlockMaxSize = 1 << nBlockMaxBits;

   pInBlock = (unsigned char*)malloc(nBlockMaxSize);
   if (!pInBlock) {
      fclose(pOutFile);
      pOutFile = NULL;

      fclose(pInFile);
      pInFile = NULL;
      fprintf(stderr, "error opening output file\n");
      return 100;
   }

   pOutData = (unsigned char*)malloc(nBlockMaxSize + HISTORY_SIZE);
   if (!pOutData) {
      free(pInBlock);
      pInBlock = NULL;

      fclose(pOutFile);
      pOutFile = NULL;

      fclose(pInFile);
      pInFile = NULL;
      fprintf(stderr, "error opening output file\n");
      return 100;
   }

   if (nOptions & OPT_VERBOSE) {
      nStartTime = do_get_time();
   }

   int nDecompressionError = 0;
   int nPrevDecompressedSize = 0;

   while (!feof(pInFile) && !nDecompressionError) {
      unsigned int nBlockSize = 0;
      bool bIsUncompressed = false;

      if (nPrevDecompressedSize != 0) {
         memcpy(pOutData + HISTORY_SIZE - nPrevDecompressedSize, pOutData + HISTORY_SIZE + (nBlockMaxSize - HISTORY_SIZE), nPrevDecompressedSize);
      }

      if ((nOptions & OPT_RAW) == 0) {
         memset(cFrameData, 0, 16);
         if (fread(cFrameData, 1, LZ4ULTRA_FRAME_SIZE, pInFile) == LZ4ULTRA_FRAME_SIZE) {
            int nSuccess = lz4ultra_decode_frame(cFrameData, LZ4ULTRA_FRAME_SIZE, &nBlockSize, &bIsUncompressed);
            if (nSuccess < 0)
               nBlockSize = 0;
         }
         else {
            nBlockSize = 0;
         }
      }
      else {
         if (nFileSize >= 2)
            nBlockSize = nFileSize - 2;
         nFileSize = 0;
      }

      if (nBlockSize != 0) {
         int nDecompressedSize = 0;

         nBlockSize &= 0x7fffffff;
         if ((int)nBlockSize > nBlockMaxSize) {
            fprintf(stderr, "block size %d > max size %d\n", nBlockSize, nBlockMaxSize);
            nDecompressionError = 1;
            break;
         }
         if (fread(pInBlock, 1, nBlockSize, pInFile) == nBlockSize) {
            if (bIsUncompressed) {
               memcpy(pOutData + HISTORY_SIZE, pInBlock, nBlockSize);
               nDecompressedSize = nBlockSize;
            }
            else {
               unsigned int nBlockOffs = 0;

               nDecompressedSize = lz4ultra_expand_block(pInBlock, nBlockSize, pOutData, HISTORY_SIZE, nBlockMaxSize);
               if (nDecompressedSize < 0) {
                  nDecompressionError = nDecompressedSize;
                  break;
               }
            }

            if (nDecompressedSize != 0) {
               nOriginalSize += (long long)nDecompressedSize;

               fwrite(pOutData + HISTORY_SIZE, 1, nDecompressedSize, pOutFile);
               if (!bIndependentBlocks) {
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
      }
      else {
         break;
      }
   }

   free(pOutData);
   pOutData = NULL;

   free(pInBlock);
   pInBlock = NULL;

   fclose(pOutFile);
   pOutFile = NULL;

   fclose(pInFile);
   pInFile = NULL;

   if (nDecompressionError) {
      fprintf(stderr, "decompression error for '%s'\n", pszInFilename);
      return 100;
   }
   else {
      if (nOptions & OPT_VERBOSE) {
         nEndTime = do_get_time();
         double fDelta = ((double)(nEndTime - nStartTime)) / 1000000.0;
         double fSpeed = ((double)nOriginalSize / 1048576.0) / fDelta;
         fprintf(stdout, "Decompressed '%s' in %g seconds, %g Mb/s\n",
            pszInFilename, fDelta, fSpeed);
      }

      return 0;
   }
}

/*---------------------------------------------------------------------------*/

static int do_compare(const char *pszInFilename, const char *pszOutFilename, const unsigned int nOptions) {
   long long nStartTime = 0LL, nEndTime = 0LL;
   long long nOriginalSize = 0LL;
   long long nKnownGoodSize = 0LL;
   unsigned int nFileSize = 0;
   int nBlockMaxCode = 4;
   bool bIndependentBlocks = false;
   unsigned char cFrameData[16];

   FILE *pInFile = fopen(pszInFilename, "rb");
   if (!pInFile) {
      fprintf(stderr, "error opening compressed input file\n");
      return 100;
   }

   if ((nOptions & OPT_RAW) == 0) {
      memset(cFrameData, 0, 16);

      if (fread(cFrameData, 1, LZ4ULTRA_HEADER_SIZE, pInFile) != LZ4ULTRA_HEADER_SIZE) {
         fclose(pInFile);
         pInFile = NULL;
         fprintf(stderr, "error reading header in compressed input file\n");
         return 100;
      }

      int nSuccess = lz4ultra_decode_header(cFrameData, LZ4ULTRA_HEADER_SIZE, &nBlockMaxCode, &bIndependentBlocks);
      if (nSuccess < 0) {
         fclose(pInFile);
         pInFile = NULL;
         if (nSuccess == LZ4ULTRA_DECODE_ERR_SUM)
            fprintf(stderr, "invalid checksum in input file\n");
         else
            fprintf(stderr, "invalid magic number, version, flags, or block size in input file\n");
         return 100;
      }
   }
   else {
      fseek(pInFile, 0, SEEK_END);
      nFileSize = (unsigned int)ftell(pInFile);
      fseek(pInFile, 0, SEEK_SET);

      if (nFileSize < 2) {
         fclose(pInFile);
         pInFile = NULL;
         fprintf(stderr, "invalid file size for raw block mode\n");
         return 100;
      }
   }

   FILE *pOutFile = fopen(pszOutFilename, "rb");
   if (!pOutFile) {
      fclose(pInFile);
      pInFile = NULL;
      fprintf(stderr, "error opening original uncompressed file\n");
      return 100;
   }

   unsigned char *pInBlock;
   unsigned char *pOutData;
   unsigned char *pCompareData;
   int nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   int nBlockMaxSize = 1 << nBlockMaxBits;

   pInBlock = (unsigned char*)malloc(nBlockMaxSize);
   if (!pInBlock) {
      fclose(pOutFile);
      pOutFile = NULL;

      fclose(pInFile);
      pInFile = NULL;
      fprintf(stderr, "error opening output file\n");
      return 100;
   }

   pOutData = (unsigned char*)malloc(nBlockMaxSize + HISTORY_SIZE);
   if (!pOutData) {
      free(pInBlock);
      pInBlock = NULL;

      fclose(pOutFile);
      pOutFile = NULL;

      fclose(pInFile);
      pInFile = NULL;
      fprintf(stderr, "error opening output file\n");
      return 100;
   }

   pCompareData = (unsigned char*)malloc(nBlockMaxSize);
   if (!pCompareData) {
      free(pOutData);
      pOutData = NULL;

      free(pInBlock);
      pInBlock = NULL;

      fclose(pOutFile);
      pOutFile = NULL;

      fclose(pInFile);
      pInFile = NULL;
      fprintf(stderr, "error opening output file\n");
      return 100;
   }

   if (nOptions & OPT_VERBOSE) {
      nStartTime = do_get_time();
   }

   int nDecompressionError = 0;
   bool bComparisonError = false;
   int nPrevDecompressedSize = 0;

   while (!feof(pInFile) && !nDecompressionError && !bComparisonError) {
      unsigned int nBlockSize = 0;
      bool bIsUncompressed = false;

      if (nPrevDecompressedSize != 0) {
         memcpy(pOutData + HISTORY_SIZE - nPrevDecompressedSize, pOutData + HISTORY_SIZE + (nBlockMaxSize - HISTORY_SIZE), nPrevDecompressedSize);
      }

      int nBytesToCompare = (int)fread(pCompareData, 1, nBlockMaxSize, pOutFile);

      if ((nOptions & OPT_RAW) == 0) {
         memset(cFrameData, 0, 16);
         if (fread(cFrameData, 1, LZ4ULTRA_FRAME_SIZE, pInFile) == LZ4ULTRA_FRAME_SIZE) {
            int nSuccess = lz4ultra_decode_frame(cFrameData, LZ4ULTRA_FRAME_SIZE, &nBlockSize, &bIsUncompressed);
            if (nSuccess < 0)
               nBlockSize = 0;
         }
         else {
            nBlockSize = 0;
         }
      }
      else {
         if (nFileSize >= 2)
            nBlockSize = nFileSize - 2;
         nFileSize = 0;
      }

      if (nBlockSize != 0) {
         int nDecompressedSize = 0;

         if ((int)nBlockSize > nBlockMaxSize) {
            fprintf(stderr, "%s: block size %d > max size %d\n", pszInFilename, nBlockSize, nBlockMaxSize);
            nDecompressionError = 1;
            break;
         }
         if (fread(pInBlock, 1, nBlockSize, pInFile) == nBlockSize) {
            if (bIsUncompressed) {
               memcpy(pOutData + HISTORY_SIZE, pInBlock, nBlockSize);
               nDecompressedSize = nBlockSize;
            }
            else {
               unsigned int nBlockOffs = 0;

               nDecompressedSize = lz4ultra_expand_block(pInBlock, nBlockSize, pOutData, HISTORY_SIZE, nBlockMaxSize);
               if (nDecompressedSize < 0) {
                  nDecompressionError = nDecompressedSize;
                  break;
               }
            }

            if (nDecompressedSize == nBytesToCompare) {
               nKnownGoodSize = nOriginalSize;

               nOriginalSize += (long long)nDecompressedSize;

               if (memcmp(pOutData + HISTORY_SIZE, pCompareData, nBytesToCompare))
                  bComparisonError = true;
               if (!bIndependentBlocks) {
                  nPrevDecompressedSize = nDecompressedSize;
                  if (nPrevDecompressedSize > HISTORY_SIZE)
                     nPrevDecompressedSize = HISTORY_SIZE;
               }
               else {
                  nPrevDecompressedSize = 0;
               }
               nDecompressedSize = 0;
            }
            else {
               fprintf(stderr, "size difference: %d != %d\n", nDecompressedSize, nBytesToCompare);
               bComparisonError = true;
               break;
            }
         }
         else {
            break;
         }
      }
      else {
         break;
      }
   }

   free(pCompareData);
   pCompareData = NULL;

   free(pOutData);
   pOutData = NULL;

   free(pInBlock);
   pInBlock = NULL;

   fclose(pOutFile);
   pOutFile = NULL;

   fclose(pInFile);
   pInFile = NULL;

   if (nDecompressionError) {
      fprintf(stderr, "decompression error for '%s'\n", pszInFilename);
      return 100;
   }
   else if (bComparisonError) {
      fprintf(stderr, "error comparing compressed file '%s' with original '%s' starting at %lld\n", pszInFilename, pszOutFilename, nKnownGoodSize);
      return 100;
   }
   else {
      if (nOptions & OPT_VERBOSE) {
         nEndTime = do_get_time();
         double fDelta = ((double)(nEndTime - nStartTime)) / 1000000.0;
         double fSpeed = ((double)nOriginalSize / 1048576.0) / fDelta;
         fprintf(stdout, "Compared '%s' in %g seconds, %g Mb/s\n",
            pszInFilename, fDelta, fSpeed);
      }

      return 0;
   }
}

/*---------------------------------------------------------------------------*/

int main(int argc, char **argv) {
   int i;
   const char *pszInFilename = NULL;
   const char *pszOutFilename = NULL;
   bool bArgsError = false;
   bool bCommandDefined = false;
   bool bVerifyCompression = false;
   int nBlockMaxCode = 7;
   bool bBlockCodeDefined = false;
   bool bIndependentBlocks = false;
   bool bBlockDependenceDefined = false;
   char cCommand = 'z';
   unsigned int nOptions = 0;

   for (i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-d")) {
         if (!bCommandDefined) {
            bCommandDefined = true;
            cCommand = 'd';
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-z")) {
         if (!bCommandDefined) {
            bCommandDefined = true;
            cCommand = 'z';
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-c")) {
         if (!bVerifyCompression) {
            bVerifyCompression = true;
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-BD")) {
         if (!bBlockDependenceDefined) {
            bBlockDependenceDefined = true;
            bIndependentBlocks = false;
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-BI")) {
         if (!bBlockDependenceDefined) {
            bBlockDependenceDefined = true;
            bIndependentBlocks = true;
         }
         else
            bArgsError = true;
      }
      else if (!strncmp(argv[i], "-B", 2)) {
         if (!bBlockCodeDefined) {
            nBlockMaxCode = atoi(argv[i] + 2);
            if (nBlockMaxCode < 4 || nBlockMaxCode > 7)
               bArgsError = true;
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-v")) {
         if ((nOptions & OPT_VERBOSE) == 0) {
            nOptions |= OPT_VERBOSE;
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-r")) {
         if ((nOptions & OPT_RAW) == 0) {
            nOptions |= OPT_RAW;
         }
         else
            bArgsError = true;
      }
      else {
         if (!pszInFilename)
            pszInFilename = argv[i];
         else {
            if (!pszOutFilename)
               pszOutFilename = argv[i];
            else
               bArgsError = true;
         }
      }
   }

   if (bArgsError || !pszInFilename || !pszOutFilename) {
      fprintf(stderr, "lz4ultra v" TOOL_VERSION " by Emmanuel Marty and spke\n");
      fprintf(stderr, "usage: %s [-c] [-d] [-v] [-r] <infile> <outfile>\n", argv[0]);
      fprintf(stderr, "       -c: check resulting stream after compressing\n");
      fprintf(stderr, "       -d: decompress (default: compress)\n");
      fprintf(stderr, "   -B4..7: compress with 64, 256, 1024 or 4096 Kb blocks (defaults to -B7)\n");
      fprintf(stderr, "      -BD: use block-dependent compression (default)\n");
      fprintf(stderr, "      -BI: use block-independent compression\n");
      fprintf(stderr, "       -v: be verbose\n");
      fprintf(stderr, "       -r: raw block format (max. 64 Kb files)\n");
      return 100;
   }

   if (cCommand == 'z') {
      int nResult = do_compress(pszInFilename, pszOutFilename, nOptions, nBlockMaxCode, bIndependentBlocks);
      if (nResult == 0 && bVerifyCompression) {
         nResult = do_compare(pszOutFilename, pszInFilename, nOptions);
      }
   }
   else if (cCommand == 'd') {
      return do_decompress(pszInFilename, pszOutFilename, nOptions);
   }
   else {
      return 100;
   }
}
