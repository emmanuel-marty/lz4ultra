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
#include <windows.h>
#include <sys/timeb.h>
#else
#include <sys/time.h>
#endif
#include "lib.h"
#include "inmem.h"

#define OPT_VERBOSE        1
#define OPT_FAVOR_RATIO    2
#define OPT_RAW            4

#define TOOL_VERSION "1.1.3"

/*---------------------------------------------------------------------------*/

#ifdef _WIN32
LARGE_INTEGER hpc_frequency;
BOOL hpc_available = FALSE;
#endif

static void do_init_time() {
#ifdef _WIN32
   hpc_frequency.QuadPart = 0;
   hpc_available = QueryPerformanceFrequency(&hpc_frequency);
#endif
}

static long long do_get_time() {
   long long nTime;

#ifdef _WIN32
   if (hpc_available) {
      LARGE_INTEGER nCurTime;

      /* Use HPC hardware for best precision */
      QueryPerformanceCounter(&nCurTime);
      nTime = (long long)(nCurTime.QuadPart * 1000000LL / hpc_frequency.QuadPart);
   }
   else {
      struct _timeb tb;
      _ftime(&tb);

      nTime = ((long long)tb.time * 1000LL + (long long)tb.millitm) * 1000LL;
   }
#else
   struct timeval tm;
   gettimeofday(&tm, NULL);

   nTime = (long long)tm.tv_sec * 1000000LL + (long long)tm.tv_usec;
#endif
   return nTime;
}

/*---------------------------------------------------------------------------*/

static void compression_start(int nBlockMaxCode, int nIsIndependentBlocks) {
   int nBlockMaxBits = 8 + (nBlockMaxCode << 1);
   int nBlockMaxSize = 1 << nBlockMaxBits;

   fprintf(stdout, "Use %d Kb blocks, independent blocks: %s\n", nBlockMaxSize >> 10, nIsIndependentBlocks ? "yes" : "no");
}

static void compression_progress(long long nOriginalSize, long long nCompressedSize) {
   fprintf(stdout, "\r%lld => %lld (%g %%)     \b\b\b\b\b", nOriginalSize, nCompressedSize, (double)(nCompressedSize * 100.0 / nOriginalSize));
   fflush(stdout);
}

static int do_compress(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename, const unsigned int nOptions, int nBlockMaxCode, int nIsIndependentBlocks) {
   long long nStartTime = 0LL, nEndTime = 0LL;
   long long nOriginalSize = 0LL, nCompressedSize = 0LL;
   lz4ultra_status_t nStatus;
   int nCommandCount = 0;
   int nFlags;

   nFlags = 0;
   if (nOptions & OPT_FAVOR_RATIO)
      nFlags |= LZ4ULTRA_FLAG_FAVOR_RATIO;
   if (nOptions & OPT_RAW)
      nFlags |= LZ4ULTRA_FLAG_RAW_BLOCK;

   if (nOptions & OPT_VERBOSE) {
      nStartTime = do_get_time();
   }

   nStatus = lz4ultra_compress_file(pszInFilename, pszOutFilename, pszDictionaryFilename, nFlags, nBlockMaxCode, nIsIndependentBlocks,
      (nOptions & OPT_VERBOSE) ? compression_start : NULL, compression_progress,
      &nOriginalSize, &nCompressedSize, &nCommandCount);
   switch (nStatus) {
   case LZ4ULTRA_ERROR_SRC: fprintf(stderr, "error reading '%s'\n", pszInFilename); break;
   case LZ4ULTRA_ERROR_DST: fprintf(stderr, "error writing '%s'\n", pszOutFilename); break;
   case LZ4ULTRA_ERROR_DICTIONARY: fprintf(stderr, "error reading dictionary '%s'\n", pszDictionaryFilename); break;
   case LZ4ULTRA_ERROR_MEMORY: fprintf(stderr, "out of memory\n"); break;
   case LZ4ULTRA_ERROR_COMPRESSION: fprintf(stderr, "internal compression error\n"); break;
   case LZ4ULTRA_ERROR_RAW_TOOLARGE: fprintf(stderr, "error: raw blocks can only be used with files <= 64 Kb\n"); break;
   case LZ4ULTRA_ERROR_RAW_UNCOMPRESSED: fprintf(stderr, "error: data is incompressible, raw blocks only support compressed data\n"); break;
   case LZ4ULTRA_OK: break;
   default: fprintf(stderr, "unknown compression error %d\n", nStatus); break;
   }

   if (nStatus)
      return 100;

   if (nOptions & OPT_VERBOSE) {
      nEndTime = do_get_time();

      double fDelta = ((double)(nEndTime - nStartTime)) / 1000000.0;
      double fSpeed = ((double)nOriginalSize / 1048576.0) / fDelta;
      fprintf(stdout, "\rCompressed '%s' in %g seconds, %.02g Mb/s, %d tokens (%lld bytes/token), %lld into %lld bytes ==> %g %%\n",
         pszInFilename, fDelta, fSpeed, nCommandCount, nCommandCount ? (nOriginalSize / ((long long)nCommandCount)) : 0,
         nOriginalSize, nCompressedSize, nOriginalSize ? (double)(nCompressedSize * 100.0 / nOriginalSize) : 100.0);
   }

   return 0;
}

/*---------------------------------------------------------------------------*/

static int do_decompress(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename, const unsigned int nOptions) {
   long long nStartTime = 0LL, nEndTime = 0LL;
   long long nOriginalSize = 0LL, nCompressedSize = 0LL;
   lz4ultra_status_t nStatus;
   int nFlags;

   nFlags = 0;
   if (nOptions & OPT_RAW)
      nFlags |= LZ4ULTRA_FLAG_RAW_BLOCK;

   if (nOptions & OPT_VERBOSE) {
      nStartTime = do_get_time();
   }

   nStatus = lz4ultra_decompress_file(pszInFilename, pszOutFilename, pszDictionaryFilename, nFlags, &nOriginalSize, &nCompressedSize);

   switch (nStatus) {
   case LZ4ULTRA_ERROR_SRC: fprintf(stderr, "error reading '%s'\n", pszInFilename); break;
   case LZ4ULTRA_ERROR_DST: fprintf(stderr, "error comparing compressed file '%s' with original '%s'\n", pszInFilename, pszOutFilename); break;
   case LZ4ULTRA_ERROR_DICTIONARY: fprintf(stderr, "error reading dictionary '%s'\n", pszDictionaryFilename); break;
   case LZ4ULTRA_ERROR_MEMORY: fprintf(stderr, "out of memory\n"); break;
   case LZ4ULTRA_ERROR_FORMAT: fprintf(stderr, "invalid magic number, version, flags, or block size in input file\n"); break;
   case LZ4ULTRA_ERROR_CHECKSUM: fprintf(stderr, "invalid checksum in input file\n"); break;
   case LZ4ULTRA_ERROR_DECOMPRESSION: fprintf(stderr, "internal decompression error\n"); break;
   case LZ4ULTRA_OK: break;
   default: fprintf(stderr, "unknown decompression error %d\n", nStatus); break;
   }

   if (nStatus) {
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

typedef struct {
   FILE *f;
   void *pCompareDataBuf;
   size_t nCompareDataSize;
} compare_stream_t;

void comparestream_close(lz4ultra_stream_t *stream) {
   if (stream->obj) {
      compare_stream_t *pCompareStream = (compare_stream_t *)stream->obj;
      if (pCompareStream->pCompareDataBuf) {
         free(pCompareStream->pCompareDataBuf);
         pCompareStream->pCompareDataBuf = NULL;
      }

      fclose(pCompareStream->f);
      free(pCompareStream);

      stream->obj = NULL;
      stream->read = NULL;
      stream->write = NULL;
      stream->eof = NULL;
      stream->close = NULL;
   }
}

size_t comparestream_read(lz4ultra_stream_t *stream, void *ptr, size_t size) {
   return 0;
}

size_t comparestream_write(lz4ultra_stream_t *stream, void *ptr, size_t size) {
   compare_stream_t *pCompareStream = (compare_stream_t *)stream->obj;

   if (!pCompareStream->pCompareDataBuf || pCompareStream->nCompareDataSize < size) {
      pCompareStream->nCompareDataSize = size;
      pCompareStream->pCompareDataBuf = realloc(pCompareStream->pCompareDataBuf, pCompareStream->nCompareDataSize);
      if (!pCompareStream->pCompareDataBuf)
         return 0;
   }

   size_t nReadBytes = fread(pCompareStream->pCompareDataBuf, 1, size, pCompareStream->f);
   if (nReadBytes != size) {
      return 0;
   }

   if (memcmp(ptr, pCompareStream->pCompareDataBuf, size)) {
      return 0;
   }

   return size;
}

int comparestream_eof(lz4ultra_stream_t *stream) {
   compare_stream_t *pCompareStream = (compare_stream_t *)stream->obj;
   return feof(pCompareStream->f);
}

int comparestream_open(lz4ultra_stream_t *stream, const char *pszCompareFilename, const char *pszMode) {
   compare_stream_t *pCompareStream;

   pCompareStream = (compare_stream_t*)malloc(sizeof(compare_stream_t));
   if (!pCompareStream)
      return -1;

   pCompareStream->pCompareDataBuf = NULL;
   pCompareStream->nCompareDataSize = 0;
   pCompareStream->f = (void*)fopen(pszCompareFilename, pszMode);

   if (pCompareStream->f) {
      stream->obj = pCompareStream;
      stream->read = comparestream_read;
      stream->write = comparestream_write;
      stream->eof = comparestream_eof;
      stream->close = comparestream_close;
      return 0;
   }
   else
      return -1;
}

static int do_compare(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename, const unsigned int nOptions) {
   lz4ultra_stream_t inStream, compareStream;
   long long nStartTime = 0LL, nEndTime = 0LL;
   long long nOriginalSize = 0LL, nCompressedSize = 0LL;
   void *pDictionaryData = NULL;
   int nDictionaryDataSize = 0;
   lz4ultra_status_t nStatus;
   int nFlags;

   if (lz4ultra_filestream_open(&inStream, pszInFilename, "rb") < 0) {
      fprintf(stderr, "error opening compressed input file\n");
      return 100;
   }

   if (comparestream_open(&compareStream, pszOutFilename, "rb") < 0) {
      fprintf(stderr, "error opening original uncompressed file\n");
      inStream.close(&inStream);
      return 100;
   }

   nStatus = lz4ultra_dictionary_load(pszDictionaryFilename, &pDictionaryData, &nDictionaryDataSize);
   if (nStatus) {
      compareStream.close(&compareStream);
      inStream.close(&inStream);
      fprintf(stderr, "error reading dictionary '%s'\n", pszDictionaryFilename);
      return 100;
   }

   nFlags = 0;
   if (nOptions & OPT_RAW)
      nFlags |= LZ4ULTRA_FLAG_RAW_BLOCK;

   if (nOptions & OPT_VERBOSE) {
      nStartTime = do_get_time();
   }

   nStatus = lz4ultra_decompress_stream(&inStream, &compareStream, pDictionaryData, nDictionaryDataSize, nFlags, &nOriginalSize, &nCompressedSize);
   switch (nStatus) {
   case LZ4ULTRA_ERROR_SRC: fprintf(stderr, "error reading '%s'\n", pszInFilename); break;
   case LZ4ULTRA_ERROR_DST: fprintf(stderr, "error comparing compressed file '%s' with original '%s'\n", pszInFilename, pszOutFilename); break;
   case LZ4ULTRA_ERROR_MEMORY: fprintf(stderr, "out of memory\n"); break;
   case LZ4ULTRA_ERROR_FORMAT: fprintf(stderr, "invalid magic number, version, flags, or block size in input file\n"); break;
   case LZ4ULTRA_ERROR_CHECKSUM: fprintf(stderr, "invalid checksum in input file\n"); break;
   case LZ4ULTRA_ERROR_DECOMPRESSION: fprintf(stderr, "internal decompression error\n"); break;
   case LZ4ULTRA_OK: break;
   default: fprintf(stderr, "unknown decompression error %d\n", nStatus); break;
   }
        
   lz4ultra_dictionary_free(&pDictionaryData);
   compareStream.close(&compareStream);
   inStream.close(&inStream);

   if (nStatus) {
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

static int do_benchmark(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename, const unsigned int nOptions) {
   size_t nFileSize, nMaxDecompressedSize;
   unsigned char *pFileData;
   unsigned char *pDecompressedData;
   int i;

   if (pszDictionaryFilename) {
      fprintf(stderr, "in-memory benchmarking does not support dictionaries\n");
      return 100;
   }
   
   /* Read the whole compressed file in memory */

   FILE *f_in = fopen(pszInFilename, "rb");
   if (!f_in) {
      fprintf(stderr, "error opening '%s' for reading\n", pszInFilename);
      return 100;
   }

   fseek(f_in, 0, SEEK_END);
   nFileSize = (size_t)ftell(f_in);
   fseek(f_in, 0, SEEK_SET);

   pFileData = (unsigned char*)malloc(nFileSize);
   if (!pFileData) {
      fclose(f_in);
      fprintf(stderr, "out of memory for reading '%s', %zd bytes needed\n", pszInFilename, nFileSize);
      return 100;
   }

   if (fread(pFileData, 1, nFileSize, f_in) != nFileSize) {
      free(pFileData);
      fclose(f_in);
      fprintf(stderr, "I/O error while reading '%s'\n", pszInFilename);
      return 100;
   }

   fclose(f_in);

   /* Allocate max decompressed size */

   nMaxDecompressedSize = lz4ultra_inmem_get_max_decompressed_size(pFileData, nFileSize);
   if (nMaxDecompressedSize == -1) {
      free(pFileData);
      fprintf(stderr, "invalid compressed format for file '%s'\n", pszInFilename);
      return 100;
   }

   pDecompressedData = (unsigned char*)malloc(nMaxDecompressedSize);
   if (!pDecompressedData) {
      free(pFileData);
      fprintf(stderr, "out of memory for decompressing '%s', %zd bytes needed\n", pszInFilename, nMaxDecompressedSize);
      return 100;
   }

   memset(pDecompressedData, 0, nMaxDecompressedSize);

   long long nBestDecTime = -1;

   size_t nActualDecompressedSize = 0;
   for (i = 0; i < 50; i++) {
      long long t0 = do_get_time();
      nActualDecompressedSize = lz4ultra_inmem_decompress_stream(pFileData, pDecompressedData, nFileSize, nMaxDecompressedSize);
      long long t1 = do_get_time();
      if (nActualDecompressedSize == -1) {
         free(pDecompressedData);
         free(pFileData);
         fprintf(stderr, "decompression error\n");
         return 100;
      }

      long long nCurDecTime = t1 - t0;
      if (nBestDecTime == -1 || nBestDecTime > nCurDecTime)
         nBestDecTime = nCurDecTime;
   }

   if (pszOutFilename) {
      FILE *f_out;

      /* Write whole decompressed file out */

      f_out = fopen(pszOutFilename, "wb");
      if (f_out) {
         fwrite(pDecompressedData, 1, nActualDecompressedSize, f_out);
         fclose(f_out);
      }
   }

   free(pDecompressedData);
   free(pFileData);

   fprintf(stdout, "decompressed size: %zd bytes\n", nActualDecompressedSize);
   fprintf(stdout, "decompression time: %lld microseconds (%g Mb/s)\n", nBestDecTime, ((double)nActualDecompressedSize / 1024.0) / ((double)nBestDecTime / 1000.0));

   return 0;
}

/*---------------------------------------------------------------------------*/

int main(int argc, char **argv) {
   int i;
   const char *pszInFilename = NULL;
   const char *pszOutFilename = NULL;
   const char *pszDictionaryFilename = NULL;
   bool bArgsError = false;
   bool bCommandDefined = false;
   bool bVerifyCompression = false;
   int nBlockMaxCode = 7;
   bool bBlockCodeDefined = false;
   int nIsIndependentBlocks = 0;
   bool bBlockDependenceDefined = false;
   char cCommand = 'z';
   unsigned int nOptions = OPT_FAVOR_RATIO;

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
      else if (!strcmp(argv[i], "-bd")) {
         if (!bCommandDefined) {
            bCommandDefined = true;
            cCommand = 'b';
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-D")) {
         if (!pszDictionaryFilename && (i + 1) < argc) {
            pszDictionaryFilename = argv[i + 1];
            i++;
         }
         else
            bArgsError = true;
      }
      else if (!strncmp(argv[i], "-D", 2)) {
         if (!pszDictionaryFilename) {
            pszDictionaryFilename = argv[i] + 2;
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-BD")) {
         if (!bBlockDependenceDefined) {
            bBlockDependenceDefined = true;
            nIsIndependentBlocks = 0;
         }
         else
            bArgsError = true;
      }
      else if (!strcmp(argv[i], "-BI")) {
         if (!bBlockDependenceDefined) {
            bBlockDependenceDefined = true;
            nIsIndependentBlocks = 1;
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
      else if (!strcmp(argv[i], "--favor-decSpeed")) {
         if ((nOptions & OPT_FAVOR_RATIO) != 0) {
            nOptions &= (~OPT_FAVOR_RATIO);
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
      fprintf(stderr, "              -c: check resulting stream after compressing\n");
      fprintf(stderr, "              -d: decompress (default: compress)\n");
      fprintf(stderr, "             -bd: benchmark in-memory decompression\n");
      fprintf(stderr, "          -B4..7: compress with 64, 256, 1024 or 4096 Kb blocks (defaults to -B7)\n");
      fprintf(stderr, "             -BD: use block-dependent compression (default)\n");
      fprintf(stderr, "             -BI: use block-independent compression\n");
      fprintf(stderr, "              -v: be verbose\n");
      fprintf(stderr, "              -r: raw block format (max. 64 Kb files)\n");
      fprintf(stderr, "--favor-decSpeed: trade some ratio for faster decompression\n");
      fprintf(stderr, "   -D <filename>: use dictionary file\n");
      return 100;
   }

   do_init_time();

   if (cCommand == 'z') {
      int nResult = do_compress(pszInFilename, pszOutFilename, pszDictionaryFilename, nOptions, nBlockMaxCode, nIsIndependentBlocks);
      if (nResult == 0 && bVerifyCompression) {
         nResult = do_compare(pszOutFilename, pszInFilename, pszDictionaryFilename, nOptions);
      }
   }
   else if (cCommand == 'd') {
      return do_decompress(pszInFilename, pszOutFilename, pszDictionaryFilename, nOptions);
   }
   else if (cCommand == 'b') {
      return do_benchmark(pszInFilename, pszOutFilename, pszDictionaryFilename, nOptions);
   }
   else {
      return 100;
   }
}
