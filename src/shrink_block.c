/*
 * shrink_block.c - optimal LZ4 block compressor implementation
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
#include "lib.h"
#include "shrink_block.h"
#include "format.h"

/**
 * Get the number of extra bits required to represent a literals length
 *
 * @param nLength literals length
 *
 * @return number of extra bits required
 */
static inline int lz4ultra_get_literals_varlen_size(const int nLength) {
   return ((nLength - LITERALS_RUN_LEN + 255) / 255) << 3;
}

/**
 * Write extra literals length bytes to output (compressed) buffer. The caller must first check that there is enough
 * room to write the bytes.
 *
 * @param pOutData pointer to output buffer
 * @param nOutOffset current write index into output buffer
 * @param nLength literals length
 */
static inline int lz4ultra_write_literals_varlen(unsigned char *pOutData, int nOutOffset, int nLength) {
   if (nLength >= LITERALS_RUN_LEN) {
      nLength -= LITERALS_RUN_LEN;
      while (nLength >= 255) {
         pOutData[nOutOffset++] = 255;
         nLength -= 255;
      }
      pOutData[nOutOffset++] = nLength;
   }

   return nOutOffset;
}

/**
 * Get the number of extra bits required to represent an encoded match length
 *
 * @param nLength encoded match length (actual match length - MIN_MATCH_SIZE)
 *
 * @return number of extra bits required
 */
static inline int lz4ultra_get_match_varlen_size(const int nLength) {
   return ((nLength - MATCH_RUN_LEN + 255) / 255) << 3;
}

/**
 * Write extra encoded match length bytes to output (compressed) buffer. The caller must first check that there is enough
 * room to write the bytes.
 *
 * @param pOutData pointer to output buffer
 * @param nOutOffset current write index into output buffer
 * @param nLength encoded match length (actual match length - MIN_MATCH_SIZE)
 */
static inline int lz4ultra_write_match_varlen(unsigned char *pOutData, int nOutOffset, int nLength) {
   if (nLength >= MATCH_RUN_LEN) {
      nLength -= MATCH_RUN_LEN;
      while (nLength >= 255) {
         pOutData[nOutOffset++] = 255;
         nLength -= 255;
      }
      pOutData[nOutOffset++] = nLength;
   }

   return nOutOffset;
}

/**
 * Attempt to pick optimal matches, so as to produce the smallest possible output that decompresses to the same input
 *
 * @param pCompressor compression context
 * @param nStartOffset current offset in input window (typically the number of previously compressed bytes)
 * @param nEndOffset offset to end finding matches at (typically the size of the total input window in bytes
 */
static void lz4ultra_optimize_matches_lz4(lz4ultra_compressor *pCompressor, const int nStartOffset, const int nEndOffset) {
   int *cost = (int*)pCompressor->pos_data;  /* Reuse */
   int *score = (int*)pCompressor->intervals;  /* Reuse */
   int nExtraMatchScore = (pCompressor->flags & LZ4ULTRA_FLAG_FAVOR_RATIO) ? 1 : 5;
   int nLastLiteralsOffset;
   int i;

   cost[nEndOffset - 1] = 8;
   score[nEndOffset - 1] = 0;
   nLastLiteralsOffset = nEndOffset;

   for (i = nEndOffset - 2; i != (nStartOffset - 1); i--) {
      int nBestCost, nBestScore, nBestMatchLen, nBestMatchOffset;

      int nLiteralsLen = nLastLiteralsOffset - i;
      nBestCost = 8 + cost[i + 1];
      nBestScore = 1 + score[i + 1];
      if (nLiteralsLen >= LITERALS_RUN_LEN && ((nLiteralsLen - LITERALS_RUN_LEN) % 255) == 0) {
         /* Add to the cost of encoding literals as their number crosses a variable length encoding boundary.
          * The cost automatically accumulates down the chain. */
         nBestCost += 8;
      }
      if (pCompressor->match[i + 1].length >= MIN_MATCH_SIZE)
         nBestCost += MODESWITCH_PENALTY;
      nBestMatchLen = 0;
      nBestMatchOffset = 0;

      lz4ultra_match *pMatch = pCompressor->match + i;

      if (pMatch->length >= MIN_MATCH_SIZE) {
         if (pMatch->length >= LEAVE_ALONE_MATCH_SIZE) {
            int nCurCost, nCurScore;
            int nMatchLen = pMatch->length;

            if ((i + nMatchLen) > (nEndOffset - LAST_LITERALS))
               nMatchLen = nEndOffset - LAST_LITERALS - i;

            nCurCost = 8 + 16 + lz4ultra_get_match_varlen_size(nMatchLen - MIN_MATCH_SIZE);
            nCurCost += cost[i + nMatchLen];
            if (pCompressor->match[i + nMatchLen].length >= MIN_MATCH_SIZE)
               nCurCost += MODESWITCH_PENALTY;
            nCurScore = nExtraMatchScore + score[i + nMatchLen];

            if (nBestCost > nCurCost || (nBestCost == nCurCost && nBestScore > nCurScore)) {
               nBestCost = nCurCost;
               nBestScore = nCurScore;
               nBestMatchLen = nMatchLen;
               nBestMatchOffset = pMatch->offset;
            }
         }
         else {
            int nMatchLen = pMatch->length;
            int k;

            if ((i + nMatchLen) > (nEndOffset - LAST_LITERALS))
               nMatchLen = nEndOffset - LAST_LITERALS - i;

            if ((pCompressor->flags & LZ4ULTRA_FLAG_FAVOR_RATIO) == 0) {
               /* If the match is just above the size where it would use the fast decompression path, shorten it so it does use it,
                * giving up some ratio for extra decompression speed */
               if (nMatchLen > (MATCH_RUN_LEN + MIN_MATCH_SIZE - 1) && nMatchLen <= (2 * (MATCH_RUN_LEN + MIN_MATCH_SIZE - 1)))
                  nMatchLen = MATCH_RUN_LEN + MIN_MATCH_SIZE - 1;
            }

            for (k = nMatchLen; k >= (MATCH_RUN_LEN + MIN_MATCH_SIZE); k--) {
               int nCurCost, nCurScore;

               nCurCost = 8 + 16 + lz4ultra_get_match_varlen_size(k - MIN_MATCH_SIZE);
               nCurCost += cost[i + k];
               if (pCompressor->match[i + k].length >= MIN_MATCH_SIZE)
                  nCurCost += MODESWITCH_PENALTY;
               nCurScore = nExtraMatchScore + score[i + k];

               if (nBestCost > nCurCost || (nBestCost == nCurCost && nBestScore > nCurScore)) {
                  nBestCost = nCurCost;
                  nBestScore = nCurScore;
                  nBestMatchLen = k;
                  nBestMatchOffset = pMatch->offset;
               }
            }

            for (;  k >= MIN_MATCH_SIZE; k--) {
               int nCurCost, nCurScore;

               nCurCost = 8 + 16 /* no extra match len bytes */;
               nCurCost += cost[i + k];
               if (pCompressor->match[i + k].length >= MIN_MATCH_SIZE)
                  nCurCost += MODESWITCH_PENALTY;
               nCurScore = nExtraMatchScore + score[i + k];

               if (nBestCost > nCurCost || (nBestCost == nCurCost && nBestScore > nCurScore)) {
                  nBestCost = nCurCost;
                  nBestScore = nCurScore;
                  nBestMatchLen = k;
                  nBestMatchOffset = pMatch->offset;
               }
            }
         }
      }

      if (nBestMatchLen >= MIN_MATCH_SIZE)
         nLastLiteralsOffset = i;

      cost[i] = nBestCost;
      score[i] = nBestScore;
      pMatch->length = nBestMatchLen;
      pMatch->offset = nBestMatchOffset;
   }
}

/**
 * Attempt to minimize the number of commands issued in the compressed data block, in order to speed up decompression without
 * impacting the compression ratio
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param nStartOffset current offset in input window (typically the number of previously compressed bytes)
 * @param nEndOffset offset to end finding matches at (typically the size of the total input window in bytes
 */
static void lz4ultra_optimize_command_count_lz4(lz4ultra_compressor *pCompressor, const unsigned char *pInWindow, const int nStartOffset, const int nEndOffset) {
   int i;
   int nNumLiterals = 0;

   for (i = nStartOffset; i < nEndOffset; ) {
      lz4ultra_match *pMatch = pCompressor->match + i;

      if (pMatch->length >= MIN_MATCH_SIZE) {
         int nMatchLen = pMatch->length;
         int nReduce = 0;

         if (nMatchLen <= 19 && (i + nMatchLen) < nEndOffset) {
            int nEncodedMatchLen = nMatchLen - MIN_MATCH_SIZE;
            int nCommandSize = 8 /* token */ + lz4ultra_get_literals_varlen_size(nNumLiterals) + 16 /* match offset */ + lz4ultra_get_match_varlen_size(nEncodedMatchLen);

            if (pCompressor->match[i + nMatchLen].length >= MIN_MATCH_SIZE) {
               if (nCommandSize >= ((nMatchLen << 3) + lz4ultra_get_literals_varlen_size(nNumLiterals + nMatchLen))) {
                  /* This command is a match; the next command is also a match. The next command currently has no literals; replacing this command by literals will
                   * make the next command eat the cost of encoding the current number of literals, + nMatchLen extra literals. The size of the current match command is
                   * at least as much as the number of literal bytes + the extra cost of encoding them in the next match command, so we can safely replace the current
                   * match command by literals, the output size will not increase and it will remove one command. */
                  nReduce = 1;
               }
            }
            else {
               int nCurIndex = i + nMatchLen;
               int nNextNumLiterals = 0;

               do {
                  nCurIndex++;
                  nNextNumLiterals++;
               } while (nCurIndex < nEndOffset && pCompressor->match[nCurIndex].length < MIN_MATCH_SIZE);

               if (nCommandSize >= ((nMatchLen << 3) + lz4ultra_get_literals_varlen_size(nNumLiterals + nNextNumLiterals + nMatchLen) - lz4ultra_get_literals_varlen_size(nNextNumLiterals))) {
                  /* This command is a match, and is followed by literals, and then another match or the end of the input data. If encoding this match as literals doesn't take
                   * more room than the match, and doesn't grow the next match command's literals encoding, go ahead and remove the command. */
                  nReduce = 1;
               }
            }
         }

         if (nReduce) {
            int j;

            for (j = 0; j < nMatchLen; j++) {
               pCompressor->match[i + j].length = 0;
            }
            nNumLiterals += nMatchLen;
            i += nMatchLen;
         }
         else {
            if ((i + nMatchLen) < nEndOffset && pMatch->offset > 0 && nMatchLen >= 2 &&
               pCompressor->match[i + nMatchLen].offset > 0 &&
               pCompressor->match[i + nMatchLen].length >= 2 &&
               (nMatchLen + pCompressor->match[i + nMatchLen].length) >= LEAVE_ALONE_MATCH_SIZE &&
               (nMatchLen + pCompressor->match[i + nMatchLen].length) <= 65535 &&
               (i + nMatchLen) >= (int)pMatch->offset &&
               (i + nMatchLen) >= (int)pCompressor->match[i + nMatchLen].offset &&
               (i + nMatchLen + (int)pCompressor->match[i + nMatchLen].length) <= nEndOffset &&
               !memcmp(pInWindow + i + nMatchLen - pMatch->offset,
                  pInWindow + i + nMatchLen - pCompressor->match[i + nMatchLen].offset,
                  pCompressor->match[i + nMatchLen].length)) {

               /* Join */

               pMatch->length += pCompressor->match[i + nMatchLen].length;
               pCompressor->match[i + nMatchLen].offset = 0;
               pCompressor->match[i + nMatchLen].length = -1;
               continue;
            }

            nNumLiterals = 0;
            i += nMatchLen;
         }
      }
      else {
         nNumLiterals++;
         i++;
      }
   }
}

/**
 * Emit block of compressed data
 *
 * @param pCompressor compression context
 * @param pInWindow pointer to input data window (previously compressed bytes + bytes to compress)
 * @param nStartOffset current offset in input window (typically the number of previously compressed bytes)
 * @param nEndOffset offset to end finding matches at (typically the size of the total input window in bytes
 * @param pOutData pointer to output buffer
 * @param nMaxOutDataSize maximum size of output buffer, in bytes
 *
 * @return size of compressed data in output buffer, or -1 if the data is uncompressible
 */
static int lz4ultra_write_block_lz4(lz4ultra_compressor *pCompressor, const unsigned char *pInWindow, const int nStartOffset, const int nEndOffset, unsigned char *pOutData, const int nMaxOutDataSize) {
   int i;
   int nNumLiterals = 0;
   int nInFirstLiteralOffset = 0;
   int nOutOffset = 0;

   for (i = nStartOffset; i < nEndOffset; ) {
      lz4ultra_match *pMatch = pCompressor->match + i;

      if (pMatch->length >= MIN_MATCH_SIZE) {
         int nMatchOffset = pMatch->offset;
         int nMatchLen = pMatch->length;
         int nEncodedMatchLen = nMatchLen - MIN_MATCH_SIZE;
         int nTokenLiteralsLen = (nNumLiterals >= LITERALS_RUN_LEN) ? LITERALS_RUN_LEN : nNumLiterals;
         int nTokenMatchLen = (nEncodedMatchLen >= MATCH_RUN_LEN) ? MATCH_RUN_LEN : nEncodedMatchLen;
         int nCommandSize = 8 /* token */ + lz4ultra_get_literals_varlen_size(nNumLiterals) + (nNumLiterals << 3) + 16 /* match offset */ + lz4ultra_get_match_varlen_size(nEncodedMatchLen);

         if ((nOutOffset + (nCommandSize >> 3)) > nMaxOutDataSize)
            return -1;
         if (nMatchOffset < MIN_OFFSET || nMatchOffset > MAX_OFFSET)
            return -1;

         pOutData[nOutOffset++] = (nTokenLiteralsLen << 4) | nTokenMatchLen;
         nOutOffset = lz4ultra_write_literals_varlen(pOutData, nOutOffset, nNumLiterals);

         if (nNumLiterals != 0) {
            memcpy(pOutData + nOutOffset, pInWindow + nInFirstLiteralOffset, nNumLiterals);
            nOutOffset += nNumLiterals;
            nNumLiterals = 0;
         }

         pOutData[nOutOffset++] = nMatchOffset & 0xff;
         pOutData[nOutOffset++] = nMatchOffset >> 8;
         nOutOffset = lz4ultra_write_match_varlen(pOutData, nOutOffset, nEncodedMatchLen);
         i += nMatchLen;

         pCompressor->num_commands++;
      }
      else {
         if (nNumLiterals == 0)
            nInFirstLiteralOffset = i;
         nNumLiterals++;
         i++;
      }
   }

   {
      int nTokenLiteralsLen = (nNumLiterals >= LITERALS_RUN_LEN) ? LITERALS_RUN_LEN : nNumLiterals;
      int nCommandSize = 8 /* token */ + lz4ultra_get_literals_varlen_size(nNumLiterals) + (nNumLiterals << 3);

      if ((nOutOffset + (nCommandSize >> 3)) > nMaxOutDataSize)
         return -1;

      pOutData[nOutOffset++] = (nTokenLiteralsLen << 4);
      nOutOffset = lz4ultra_write_literals_varlen(pOutData, nOutOffset, nNumLiterals);

      if (nNumLiterals != 0) {
         memcpy(pOutData + nOutOffset, pInWindow + nInFirstLiteralOffset, nNumLiterals);
         nOutOffset += nNumLiterals;
         nNumLiterals = 0;
      }

      if (pCompressor->flags & LZ4ULTRA_FLAG_RAW_BLOCK) {
         if ((nOutOffset + 2) > nMaxOutDataSize)
            return -1;

         /* Emit zero match offset as an EOD marker for raw blocks */
         pOutData[nOutOffset++] = 0;
         pOutData[nOutOffset++] = 0;
      }

      pCompressor->num_commands++;
   }

   return nOutOffset;
}

/**
 * Select the most optimal matches, reduce the token count if possible, and then emit a block of compressed LZ4 data
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
int lz4ultra_optimize_and_write_block(lz4ultra_compressor *pCompressor, const unsigned char *pInWindow, const int nPreviousBlockSize, const int nInDataSize, unsigned char *pOutData, const int nMaxOutDataSize) {
   lz4ultra_optimize_matches_lz4(pCompressor, nPreviousBlockSize, nPreviousBlockSize + nInDataSize);
   lz4ultra_optimize_command_count_lz4(pCompressor, pInWindow, nPreviousBlockSize, nPreviousBlockSize + nInDataSize);

   return lz4ultra_write_block_lz4(pCompressor, pInWindow, nPreviousBlockSize, nPreviousBlockSize + nInDataSize, pOutData, nMaxOutDataSize);
}
