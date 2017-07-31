/*
Copyright (C) 2017  Brodie Gaslam

This file is part of "vetr - Trust, but Verify"

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Go to <https://www.r-project.org/Licenses/GPL-2> for a copy of the license.
*/
#include "cstringr.h"
/*
 * This appears to update with `Sys.setlocale`
 */
extern Rboolean utf8locale;

/*
 * Most of the functionality in this file already exists built in to R, so we
 * suggest you check out `nchar`, `substr`, etc.  This is mostly a learning
 * exercise for me.
 */
/*
 * Computes how many bytes a UTF8 point takes.
 *
 * If possible UTF8 outside ASCII, check up to the next 4 bytes and for and
 * offset bytes by length of maximal subpart of valid sequence, or length of
 * valid sequence.  Note that we've already advanced one byte above so all
 * failing cases don't need to advance further as they would advance by one.
 *
 * This is based on:
 * <http://www.unicode.org/versions/Unicode10.0.0/ch03.pdf#G7404>,
 * table 3-7, transcribed below
 *
 * Well-Formed UTF-8 Byte Sequences
 * Code Points        | Byte 1 | Byte 2 | Byte 3 | Byte 4
 * U+0000..U+007F     | 00..7F |
 * U+0080..U+07FF     | C2..DF | 80..BF
 * U+0800..U+0FFF     | E0     | A0..BF | 80..BF
 * U+1000..U+CFFF     | E1..EC | 80..BF | 80..BF
 * U+D000..U+D7FF     | ED     | 80..9F | 80..BF
 * U+E000..U+FFFF     | EE..EF | 80..BF | 80..BF
 * U+10000..U+3FFFF   | F0     | 90..BF | 80..BF | 80..BF
 * U+40000..U+FFFFF   | F1..F3 | 80..BF | 80..BF | 80..BF
 * U+100000..U+10FFFF | F4     | 80..8F | 80..BF | 80..BF
 */
static inline int utf8_offset(unsigned const char * char_ptr) {
  unsigned const char char_val = *(char_ptr);

  int byte_count = 1;  // Always at least one val

  if(char_val & 128) {
    if(UTF8_BW(char_ptr, 0xC2, 0xDF)) {
      // two byte sequence
      if(UTF8_IS_CONT(char_ptr + 1)) byte_count +=1;
    } else if(char_val == 0xE0) {
      // three byte sequence, exception 1
      if(UTF8_BW(char_ptr + 1, 0xA0, 0xBF)) {
        if(UTF8_IS_CONT(char_ptr + 2)) byte_count +=2;
        else byte_count +=1;
      }
    } else if(char_val == 0xED) {
      // three byte sequence, exception 2
      if(UTF8_BW(char_ptr + 1, 0x80, 0x9F)) {
        if(UTF8_IS_CONT(char_ptr + 2)) byte_count +=2;
        else byte_count +=1;
      }
    } else if (UTF8_BW(char_ptr, 0xE0, 0xEF)) {
      // three byte sequence normal, note by construction excluding E0, ED
      if(UTF8_IS_CONT(char_ptr + 1)) {
        if(UTF8_IS_CONT(char_ptr + 2)) byte_count +=2;
        else byte_count +=1;
      }
    } else if (char_val == 0xF0) {
      // four byte sequence, v1
      if(UTF8_BW(char_ptr + 1, 0x90, 0xBF)) {
        if(UTF8_IS_CONT(char_ptr + 2)) {
          if(UTF8_IS_CONT(char_ptr + 3)) {
            byte_count += 3;
          } else byte_count += 2;
        } else byte_count +=1;
      }
    } else if (UTF8_BW(char_ptr, 0xF1, 0xF3)) {
      // four byte sequence, v2
      if(UTF8_IS_CONT(char_ptr + 1)) {
        if(UTF8_IS_CONT(char_ptr + 2)) {
          if(UTF8_IS_CONT(char_ptr + 3)) {
            byte_count += 3;
          } else byte_count += 2;
        } else byte_count +=1;
      }
    } else if (char_val == 0xF4) {
      // four byte sequence, v2
      if(UTF8_BW(char_ptr + 1, 0x80, 0x8F)) {
        if(UTF8_IS_CONT(char_ptr + 2)) {
          if(UTF8_IS_CONT(char_ptr + 3)) {
            byte_count += 3;
          } else byte_count += 2;
        } else byte_count +=1;
    } }
  }
  return byte_count;
}
/*
 * Rather than try to handle all native encodings, we just convert directly
 * to UTF8 and don't worry about it.  This will be sub-optimal in LATIN1 or
 * windows 1252 locales where each character can be represented by a byte
 * and could potentially lead to copying of entire vectors. We'll have to
 * consider a mode where we let known 255 element encodings through...
 */
static inline unsigned const char * as_utf8_char(SEXP string, R_xlen_t i) {
  SEXP str_elt = STRING_ELT(string, i);
  const char * char_val;

  cetype_t chr_enc = getCharCE(str_elt);
  if(chr_enc == CE_UTF8 || (chr_enc == CE_NATIVE && utf8locale)) {
    char_val = CHAR(STRING_ELT(string, i));
  } else {
    char_val = translateCharUTF8(STRING_ELT(string, i));
  }
  return (unsigned const char *) char_val;
}
/*
 * Truncates strings to specified length.
 *
 * Will re-encode any encoded strings into UTF-8.
 *
 * string character vector of strings to truncate
 * len scalar integer of length to truncate to
 * mark_trunc scalar logical whether to append a ".." to indicate that
 *   a string was truncated
 *
 * Note that we always allocate a STRSXP of the same length as `string`, even if
 * we end up not trimming any of the elements.  We only create new CHARSXP for
 * the elements that are changed (assuming it's okay to re-use the same CHARSXP
 * in different strings).
 */

SEXP CSR_strsub(SEXP string, SEXP chars, SEXP mark_trunc) {
  if(CHAR_BIT != 8)
    error("Internal Error: can only work with 8 bit characters");

  if(TYPEOF(string) != STRSXP)
    error("Argument `string` must be a string.");
  if(TYPEOF(mark_trunc) != LGLSXP && xlength(mark_trunc) != 1)
    error("Argument `mark_trunc` must be a TRUE or FALSE.");

  if(
    TYPEOF(chars) != INTSXP || xlength(chars) != 1 || INTEGER(chars)[0] < 1
  )
    error(
      "Argument `chars` must be scalar integer, strictly positive, and not NA."
    );

  R_xlen_t i, len = xlength(string);
  int mark = asInteger(mark_trunc) > 0;
  int chars_int = asInteger(chars);

  // If you change this, you must adapt position tracking below to make sure you
  // keep track of the right byte position for offset -pad_len

  const char * pad = ".."; // padding for truncated strings
  const int pad_len = 2;   // make sure this aligns with `pad`

  if(chars_int - mark * pad_len < 1)
    error(
      "Argument `chars` must be greater than 2 when `mark_trunc` is TRUE."
    );

  SEXP res_string = PROTECT(allocVector(STRSXP, len));

  for(i = 0; i < len; ++i) {
    unsigned const char * char_start, * char_ptr;
    unsigned char char_val; // need for > 127

    char_start = as_utf8_char(string, i);
    R_xlen_t char_count = 0;

    size_t byte_pad = pad_len;

    // Limiting to 8 less than SIZE_T_MAX to make room for a last 4 byte UTF8
    // character, '..', and the NULL terminator

    size_t byte_count = 0, byte_count_prev, byte_count_prev_prev,
      size_t_lim = SIZE_T_MAX - 4 - byte_pad - 1;

    int is_utf8 = 0;

    // Loop while no NULL character

    while(
      (char_val = *(char_ptr = (char_start + byte_count))) &&
      char_count < chars_int
    ) {
      if(byte_count >= size_t_lim)
        error("Internal Error: size_t overflow."); // nocov, should never happen

      /* // visualize bytes debug code
      for(int jj = 8; jj > 0; --jj)
        Rprintf(
          "%d%s",
          (char_val & ((int)(pow((double) 2, (double) jj - 1)))) > 0,
          !((jj - 1) % 4) ? " " : ""
        );
      Rprintf(
        " %4u %c cc: %zd bc: %zu\n", char_val, (char) char_val, char_count,
        byte_count
      );
      */

      // Keep track of the byte position two characters ago

      if(char_count > 1) byte_count_prev_prev = byte_count_prev;
      if(char_count) byte_count_prev = byte_count;

      ++char_count;
      byte_count += utf8_offset(char_ptr);
      if(char_val > 127) is_utf8 = 1;
    }
    if(byte_count >= INT_MAX - byte_pad)
      // nocov start
      error(
        "%s - %s %s at index %.0f",
        "Internal Error: Encountered string longer than INT_MAX",
        CSR_num_as_chr((double) byte_pad, 1), (double) i
      );
      // nocov end

    // Check whether we got to end of string, and if we did truncate

    SEXP char_sxp;

    if(char_count >= chars_int && char_val) {
      char * char_res;
      char * char_trunc = CSR_strmcpy_int(
        (const char *) char_start, mark ? byte_count_prev_prev : byte_count, 0
      );
      if(mark) {
        // add an ellipsis at the end.  This is inefficient since we copy the
        // string again, but probably not worth the work to do it in one step.
        // Also probably don't need the CSR fun.

        char_res = R_alloc(byte_count + 1, sizeof(char));
        int snp_try = snprintf(
          char_res, byte_count + 1, "%s%s", char_trunc, pad
        );
        if(snp_try < 0)
          // nocov start
          error(
            "Internal Error: failed generating truncated string at index %.0f",
            (double) i
          );
          // nocov end
      } else char_res = char_trunc;

      char_sxp = PROTECT(
        is_utf8 ?  mkCharCE(char_res, CE_UTF8) : mkChar(char_res)
      );
    } else {
      char_sxp = PROTECT(STRING_ELT(string, i));
    }
    // Deal with incorrectly encoded strings? At this point we just let them
    // through since presumably `translateToUTF8` should have dealt with them

    SET_STRING_ELT(res_string, i, char_sxp);
    UNPROTECT(1);
  }
  UNPROTECT(1);
  return res_string;
}
/*
 * Like `nchar`, but has a homegrown implementation of UTF8 character counting.
 */

SEXP CSR_nchar_u(SEXP string) {
  if(TYPEOF(string) != STRSXP)
    error("Argument `string` must be a character vector.");

  R_xlen_t i, len = xlength(string);
  SEXP res = PROTECT(allocVector(INTSXP, len));

  for(i = 0; i < len; ++i) {
    // It would be nice to be able to skip the STRING_ELT stuff and access the
    // data directly as we do, but don't know how to get the size of
    // SEXPREC_ALIGN directly.

    unsigned const char * char_start, * char_ptr;
    unsigned char char_val;

    char_start = as_utf8_char(string, i);

    int byte_count = 0, char_count = 0;
    int too_long = 0; // track if any strings longer than INT_MAX

    while((char_val = *(char_ptr = (char_start + byte_count)))) {
      int byte_off = utf8_offset(char_ptr);
      if((byte_count > INT_MAX - byte_off) && !too_long) {
        // note this also catches the char_count overflow since utf8_offset will
        // always return 1 or more

        too_long = 1;
        warning("Some elements longer than INT_MAX, return NA for those.");
        break;
      }
      byte_count += byte_off;
      char_count++;
    }
    INTEGER(res)[i] = too_long ? NA_INTEGER : char_count;
  }
  UNPROTECT(1);
  return(res);
}