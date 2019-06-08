/*
 * dictionary.c - dictionary implementation
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
#include "dictionary.h"
#include "format.h"
#include "lib.h"

/**
 * Load dictionary contents
 *
 * @param pszDictionaryFilename name of dictionary file, or NULL for none
 * @param ppDictionaryData pointer to returned dictionary contents, or NULL for none
 * @param pDictionaryDataSize pointer to returned size of dictionary contents, or 0
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
 * @param ppDictionaryData pointer to pointer to dictionary contents
 */
void lz4ultra_dictionary_free(void **ppDictionaryData) {
   if (*ppDictionaryData) {
      free(*ppDictionaryData);
      ppDictionaryData = NULL;
   }
}
