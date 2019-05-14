/*
 * frame.c - lz4 frame implementation
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

#include "frame.h"
#include "xxhash.h"

/**
 * Encode compressed stream header
 *
 * @param pFrameData encoding buffer
 * @param nMaxFrameDataSize max encoding buffer size, in bytes
 * @param nBlockMaxCode max block size code (4-7)
 * @param nIsIndependentBlocks nonzero if the stream contains independently compressed blocks, 0 if blocks back-reference the previous block
 *
 * @return number of encoded bytes, or -1 for failure
 */
int lz4ultra_encode_header(unsigned char *pFrameData, const int nMaxFrameDataSize, int nBlockMaxCode, int nIsIndependentBlocks) {
   if (nMaxFrameDataSize >= 7) {
      pFrameData[0] = 0x04;                              /* Magic number: 0x184D2204 */
      pFrameData[1] = 0x22;
      pFrameData[2] = 0x4D;
      pFrameData[3] = 0x18;

      pFrameData[4] = 0b01000000;                        /* Version.Hi Version.Lo !B.Indep B.Checksum Content.Size Content.Checksum Reserved.Hi Reserved.Lo */
      if (nIsIndependentBlocks)
         pFrameData[4] |= 0b00100000;                    /*                       B.Indep */
      pFrameData[5] = nBlockMaxCode << 4;                /* Block MaxSize */

      XXH32_hash_t headerSum = XXH32(pFrameData + 4, 2, 0);
      pFrameData[6] = (headerSum >> 8) & 0xff;           /* Header checksum */

      return 7;
   }
   else {
      return LZ4ULTRA_ENCODE_ERR;
   }
}

/**
 * Encode compressed block frame header
 *
 * @param pFrameData encoding buffer
 * @param nMaxFrameDataSize max encoding buffer size, in bytes
 * @param nBlockDataSize compressed block's data size, in bytes
 *
 * @return number of encoded bytes, or -1 for failure
 */
int lz4ultra_encode_compressed_block_frame(unsigned char *pFrameData, const int nMaxFrameDataSize, const int nBlockDataSize) {
   if (nMaxFrameDataSize >= 4 && (nBlockDataSize & 0x80000000) == 0) {
      pFrameData[0] = nBlockDataSize & 0xff;
      pFrameData[1] = (nBlockDataSize >> 8) & 0xff;
      pFrameData[2] = (nBlockDataSize >> 16) & 0xff;
      pFrameData[3] = (nBlockDataSize >> 24) & 0x7f;            /* Compressed block */
      return 4;
   }
   else {
      return LZ4ULTRA_ENCODE_ERR;
   }
}

/**
 * Encode uncompressed block frame header
 *
 * @param pFrameData encoding buffer
 * @param nMaxFrameDataSize max encoding buffer size, in bytes
 * @param nBlockDataSize uncompressed block's data size, in bytes
 *
 * @return number of encoded bytes, or -1 for failure
 */
int lz4ultra_encode_uncompressed_block_frame(unsigned char *pFrameData, const int nMaxFrameDataSize, const int nBlockDataSize) {
   if (nMaxFrameDataSize >= 4 && (nBlockDataSize & 0x80000000) == 0) {
      pFrameData[0] = nBlockDataSize & 0xff;
      pFrameData[1] = (nBlockDataSize >> 8) & 0xff;
      pFrameData[2] = (nBlockDataSize >> 16) & 0xff;
      pFrameData[3] = ((nBlockDataSize >> 24) & 0x7f) | 0x80;   /* Uncompressed block */
      return 4;
   }
   else {
      return LZ4ULTRA_ENCODE_ERR;
   }
}

/**
 * Encode terminal frame header
 *
 * @param pFrameData encoding buffer
 * @param nMaxFrameDataSize max encoding buffer size, in bytes
 *
 * @return number of encoded bytes, or -1 for failure
 */
int lz4ultra_encode_footer_frame(unsigned char *pFrameData, const int nMaxFrameDataSize) {
   if (nMaxFrameDataSize >= 4) {
      pFrameData[0] = 0x00;         /* EOD frame */
      pFrameData[1] = 0x00;
      pFrameData[2] = 0x00;
      pFrameData[3] = 0x00;
      return 4;
   }
   else {
      return LZ4ULTRA_ENCODE_ERR;
   }
}

/**
 * Decode compressed stream header
 *
 * @param pFrameData data bytes
 * @param nFrameDataSize number of bytes to decode
 * @param nBlockMaxCode pointer to max block size code (4-7), updated if this function succeeds
 * @param nIsIndependentBlocks returned flag that indicates if the stream contains independently compressed blocks
 *
 * @return LZ4ULTRA_DECODE_OK for success, or LZ4ULTRA_DECODE_ERR_xxx for failure
 */
int lz4ultra_decode_header(const unsigned char *pFrameData, const int nFrameDataSize, int *nBlockMaxCode, int *nIsIndependentBlocks) {
   if (nFrameDataSize == 7) {
      if (pFrameData[0] != 0x04 ||
         pFrameData[1] != 0x22 ||
         pFrameData[2] != 0x4D ||
         pFrameData[3] != 0x18 ||
         (pFrameData[4] & 0xc0) != 0b01000000 ||
         (pFrameData[5] & 0x0f) != 0) {
         return LZ4ULTRA_DECODE_ERR_FORMAT;
      }

      XXH32_hash_t headerSum = XXH32(pFrameData + 4, 2, 0);
      if (((headerSum >> 8) & 0xff) != pFrameData[6]) {
         return LZ4ULTRA_DECODE_ERR_SUM;
      }

      *nIsIndependentBlocks = (pFrameData[4] & 0x20) ? 1 : 0;
      *nBlockMaxCode = (pFrameData[5] >> 4);

      return LZ4ULTRA_DECODE_OK;
   }
   else {
      return LZ4ULTRA_DECODE_ERR_FORMAT;
   }
}

/**
 * Decode frame header
 *
 * @param pFrameData data bytes
 * @param nFrameDataSize number of bytes to decode
 * @param nBlockSize pointer to block size, updated if this function succeeds (set to 0 if this is the terminal frame)
 * @param nIsUncompressed pointer to compressed block flag, updated if this function succeeds
 *
 * @return LZ4ULTRA_DECODE_OK for success, or LZ4ULTRA_DECODE_ERR_FORMAT for failure
 */
int lz4ultra_decode_frame(const unsigned char *pFrameData, const int nFrameDataSize, unsigned int *nBlockSize, int *nIsUncompressed) {
   if (nFrameDataSize == 4) {
      *nBlockSize = ((unsigned int)pFrameData[0]) |
         (((unsigned int)pFrameData[1]) << 8) |
         (((unsigned int)pFrameData[2]) << 16) |
         (((unsigned int)pFrameData[3]) << 24);

      *nIsUncompressed = ((*nBlockSize) & 0x80000000) ? 1 : 0;
      (*nBlockSize) &= 0x7fffffff;
      return 0;
   }
   else {
      return LZ4ULTRA_DECODE_ERR_FORMAT;
   }
}
