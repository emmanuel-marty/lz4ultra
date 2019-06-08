/*
 * shrink_inmem.h - in-memory compression definitions
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

#ifndef _SHRINK_INMEM_H
#define _SHRINK_INMEM_H

#include <stdlib.h>

/**
 * Get maximum compressed size of input(source) data
 *
 * @param nInputSize input(source) size in bytes
 * @param nFlags compression flags (LZ4ULTRA_FLAG_xxx)
 * @param nBlockMaxCode maximum block size code (4..7 for 64 Kb..4 Mb)
 *
 * @return maximum compressed size
 */
size_t lz4ultra_get_max_compressed_size_inmem(size_t nInputSize, unsigned int nFlags,
   int nBlockMaxCode);

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
size_t lz4ultra_compress_inmem(const unsigned char *pInputData, unsigned char *pOutBuffer, size_t nInputSize, size_t nMaxOutBufferSize, unsigned int nFlags,
   int nBlockMaxCode);

#endif /* _SHRINK_INMEM_H */
