/*
 * lib.h - lz4ultra library definitions
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

#ifndef _LIB_H
#define _LIB_H

#include "divsufsort.h"
#include "stream.h"

/** High level status for compression and decompression */
typedef enum {
   LZ4ULTRA_OK = 0,                          /**< Success */
   LZ4ULTRA_ERROR_SRC,                       /**< Error reading input */
   LZ4ULTRA_ERROR_DST,                       /**< Error reading output */
   LZ4ULTRA_ERROR_DICTIONARY,                /**< Error reading dictionary */
   LZ4ULTRA_ERROR_MEMORY,                    /**< Out of memory */

   /* Compression-specific status codes */
   LZ4ULTRA_ERROR_COMPRESSION,               /**< Internal compression error */
   LZ4ULTRA_ERROR_RAW_TOOLARGE,              /**< Input is too large to be compressed to a raw block */
   LZ4ULTRA_ERROR_RAW_UNCOMPRESSED,          /**< Input is incompressible and raw blocks don't support uncompressed data */

   /* Decompression-specific status codes */
   LZ4ULTRA_ERROR_FORMAT,                    /**< Invalid input format or magic number when decompressing */
   LZ4ULTRA_ERROR_CHECKSUM,                  /**< Invalid checksum when decompressing */
   LZ4ULTRA_ERROR_DECOMPRESSION,             /**< Internal decompression error */
} lz4ultra_status_t;

/* Compression flags */
#define LZ4ULTRA_FLAG_FAVOR_RATIO    (1<<0)           /**< 1 to compress with the best ratio, 0 to trade some compression ratio for extra decompression speed */
#define LZ4ULTRA_FLAG_RAW_BLOCK      (1<<1)           /**< 1 to emit raw block */
#define LZ4ULTRA_FLAG_INDEP_BLOCKS   (1<<2)           /**< 1 if blocks are independent, 0 if using inter-block back references */
#define LZ4ULTRA_FLAG_LEGACY_FRAMES  (1<<3)           /**< 1 if using the legacy frames format, 0 if using the modern lz4 frame format */

/*-------------- Top level API -------------- */

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
lz4ultra_status_t lz4ultra_compress_file(const char *pszInFilename, const char *pszOutFilename, const char *pszDictionaryFilename, const unsigned int nFlags,
   int nBlockMaxCode,
   void(*start)(int nBlockMaxCode, const unsigned int nFlags),
   void(*progress)(long long nOriginalSize, long long nCompressedSize), long long *pOriginalSize, long long *pCompressedSize, int *pCommandCount);

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
   long long *pOriginalSize, long long *pCompressedSize);

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
int lz4ultra_dictionary_load(const char *pszDictionaryFilename, void **ppDictionaryData, int *pDictionaryDataSize);

/**
 * Free dictionary contents
 *
 * @param pDictionaryData pointer to pointer to dictionary contents
 */
void lz4ultra_dictionary_free(void **ppDictionaryData);

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
   void(*progress)(long long nOriginalSize, long long nCompressedSize), long long *pOriginalSize, long long *pCompressedSize, int *pCommandCount);

/**
 * Decompress stream
 *
 * @param pInStream input(compressed) stream to decompress
 * @param pOutStream output(decompressed) stream to write to
 * @param pDictionaryData dictionary contents, or NULL for none
 * @param nDictionaryDataSize size of dictionary contents, or 0
 * @param nFlags compression flags (LZ4ULTRA_FLAG_RAW_BLOCK to decompress a raw block, or 0)
 * @param pOriginalSize pointer to returned output(decompressed) size, updated when this function is successful
 * @param pCompressedSize pointer to returned input(compressed) size, updated when this function is successful
 *
 * @return LZ4ULTRA_OK for success, or an error value from lz4ultra_status_t
 */
lz4ultra_status_t lz4ultra_decompress_stream(lz4ultra_stream_t *pInStream, lz4ultra_stream_t *pOutStream, const void *pDictionaryData, int nDictionaryDataSize, unsigned int nFlags,
   long long *pOriginalSize, long long *pCompressedSize);

/*-------------- Block compression API --------------*/

#define LCP_BITS 15
#define LCP_MAX (1LL<<(LCP_BITS - 1))
#define LCP_SHIFT (39-LCP_BITS)
#define LCP_MASK (((1LL<<LCP_BITS) - 1) << LCP_SHIFT)
#define POS_MASK ((1LL<<LCP_SHIFT) - 1)

#define NMATCHES_PER_OFFSET 8
#define MATCHES_PER_OFFSET_SHIFT 3

#define LEAVE_ALONE_MATCH_SIZE 1000

#define LAST_MATCH_OFFSET 12
#define LAST_LITERALS 5

#define MODESWITCH_PENALTY 1

/** One match */
typedef struct _lz4ultra_match {
   unsigned int length;
   unsigned int offset;
} lz4ultra_match;

/** Compression context */
typedef struct _lz4ultra_compressor {
   divsufsort_ctx_t divsufsort_context;
   unsigned long long *intervals;
   unsigned long long *pos_data;
   unsigned long long *open_intervals;
   lz4ultra_match *match;
   int flags;
   int num_commands;
} lz4ultra_compressor;

/**
 * Initialize compression context
 *
 * @param pCompressor compression context to initialize
 * @param nMaxWindowSize maximum size of input data window (previously compressed bytes + bytes to compress)
 * @param nFlags compression flags
 *
 * @return 0 for success, non-zero for failure
 */
int lz4ultra_compressor_init(lz4ultra_compressor *pCompressor, const int nMaxWindowSize, const int nFlags);

/**
 * Clean up compression context and free up any associated resources
 *
 * @param pCompressor compression context to clean up
 */
void lz4ultra_compressor_destroy(lz4ultra_compressor *pCompressor);

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
int lz4ultra_compressor_shrink_block(lz4ultra_compressor *pCompressor, const unsigned char *pInWindow, const int nPreviousBlockSize, const int nInDataSize, unsigned char *pOutData, const int nMaxOutDataSize);

/**
 * Get the number of compression commands issued in compressed data blocks
 *
 * @return number of commands
 */
int lz4ultra_compressor_get_command_count(lz4ultra_compressor *pCompressor);

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
int lz4ultra_decompressor_expand_block(const unsigned char *pInBlock, int nBlockSize, unsigned char *pOutData, int nOutDataOffset, int nBlockMaxSize);

#endif /* _LIB_H */
