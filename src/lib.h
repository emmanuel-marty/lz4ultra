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

#include "stream.h"
#include "dictionary.h"
#include "shrink_context.h"
#include "shrink_streaming.h"
#include "shrink_inmem.h"
#include "expand_block.h"
#include "expand_streaming.h"
#include "expand_inmem.h"

/** High level status for compression and decompression */
typedef enum _lz4ultra_status_t {
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

#endif /* _LIB_H */
