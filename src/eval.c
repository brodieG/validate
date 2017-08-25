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

#include "validate.h"

/* -------------------------------------------------------------------------- *\
\* -------------------------------------------------------------------------- */
/*
 * @param arg_lang the substituted language corresponding to the argument
 * @param arg_tag the argument name
 *
 * There is some really tricky busines swith the protection stack here. For each
 * VALC_res struct we create, we get a SEXP, and we want to keep those around
 * until we process the error in `VALC_evaluate`, so this function introduces a
 * stack balance issue that `VALC_evaluate` needs to rectify with the
 * information embedded in `res_list`.
 */

SEXP VALC_evaluate_recurse(
  SEXP lang, SEXP act_codes, SEXP arg_value, SEXP arg_lang, SEXP arg_tag,
  SEXP lang_full, struct VALC_settings set, struct VALC_res_list res_list
) {
  /*
  check act_codes:
    if 1 or 2
      recurse and:
        if return value is TRUE
          and act_code == 2, return TRUE
          and act_code == 1, continue
        if return value is character
          and act_code == 1,
            return
          and act_code == 2,
            record for later return if no TRUEs are met
        if return value is TRUE,
        if with mode set to corresponding value (does it matter)
    if 10, eval as is
      if returns character then return character
      if returns FALSE deparse into something like (`x` does not eval to TRUE`)
    if 999, eval as alike
  */
  int mode;

  if(TYPEOF(act_codes) == LISTSXP) {
    if(TYPEOF(lang) != LANGSXP && TYPEOF(lang) != LISTSXP) {
      // nocov start
      error("%s%s"
        "Internal Error: mismatched language and eval type tracking 1; contact ",
        "maintainer."
      );
      // nocov end
    }
    if(TYPEOF(CAR(act_codes)) != INTSXP) {
      // nocov start
      error("%s%s",
        "Internal error: no integer codes produced by parsing process, which ",
        "should not happen; contact maintainer."
      );
      // nocov end
    } else {
      mode=asInteger(CAR(act_codes));
    }
  } else {
    if(TYPEOF(lang) == LANGSXP || TYPEOF(lang) == LISTSXP) {
      // nocov start
      error("%s%s",
        "Internal Error: mismatched language and eval type tracking 2; contact ",
        "maintainer."
      );
      // nocov end
    }
    mode = asInteger(act_codes);
  }
  if(mode == 1 || mode == 2) {
    // Dealing with && or ||, so recurse on each element

    if(TYPEOF(lang) == LANGSXP) {
      int parse_count = 0;
      VALC_res_list eval_res;
      lang = CDR(lang);
      act_codes = CDR(act_codes);

      while(lang != R_NilValue) {
        res_list = VALC_evaluate_recurse(
          CAR(lang), CAR(act_codes), arg_value, arg_lang, arg_tag, lang_full,
          set, res_list
        );
        // recall res_list.idx points to next available slot, not last result
        struct VALC res_val = res_list[res_list.idx - 1];

        if(!res_val.success) {
          return(res_list);
        } else if (mode == 2) {
          // At least one succes in OR mode
          return(res_list);
        } else {
          // nocov start
          error("%s%s",
            "Internal Error: unexpected state when recursively ",
            "evaluating parse; contact maintainer."
          );
          // nocov end
        }
        lang = CDR(lang);
        act_codes = CDR(act_codes);
        parse_count++;
      }
      if(parse_count != 2) {
        // nocov start
        error("%s%s",
          "Internal Error: unexpected language structure for modes 1/2; ",
          "contact maintainer."
        );
        // nocov end
      }
      // Only way to get here is if none of previous actually returned TRUE and
      // mode is OR
      if(mode == 2) {
        return(err_list);
      }
      return(eval_res);  // Or if all returned TRUE and mode is AND
    } else {
      // nocov start
      error(
        "%s%s",
        "Internal Error: in mode c(1, 2), but not a language object; ",
        "contact maintainer."
      );
      // nocov end
    }
  } else if(mode == 10 || mode == 999) {
    struct VALC_res eval_res;
    SEXP eval_tmp;
    int err_val = 0;
    int eval_res_c = -1000;  // initialize to illegal value
    int * err_point = &err_val;
    eval_tmp = PROTECT(R_tryEval(lang, set.env, err_point));
    if(* err_point) {
      VALC_arg_error(
        arg_tag, lang_full,
        "Validation expression for argument `%s` produced an error (see previous error)."
      );
    }
    if(mode == 10) {
      eval_res_c = VALC_all(eval_tmp);
      eval_res.tpl = 0;
      eval_res.succes = eval_res_c > 0
      eval_res.dat = eval_tmp;
    } else {
      eval_res.tpl = 1;
      // bit of a complicated protection mess here, we don't want eval_res in
      // the protection stack when we're done

      eval_res.dat = ALIKEC_alike_internal(eval_tmp, arg_value, set);
      // THIS WON'T CAUSE GC!!!!  doesn't seem to now based un Rf_unprotect
      // but potentially could change in future; need to look at protect with
      // index or some such
      UNPROTECT(1);
      PROTECT(eval_res.alike.wrap);

      eval_res.success = eval_res.dat.success;
    }
    // Only add to res list if we had an error

    if(eval_res.success) UNPROTECT(1);
    else res_list = VALC_res_add(res_list, eval_res);

    return(res_list);
  } else {
    error("Internal Error: unexpected parse mode %d", mode);  // nocov
  }
  error("Internal Error: should never get here");             // nocov
  return(res_list);  // nocov Should never get here
}
/* -------------------------------------------------------------------------- *\
\* -------------------------------------------------------------------------- */
/*
TBD if this should call `VALC_parse` directly or not

@param lang the validator expression
@param arg_lang the substituted language being validated
@param arg_tag the variable name being validated
@param arg_value the value being validated
@param lang_full solely so that we can produce error message with original call
@param set the settings
*/
SEXP VALC_evaluate(
  SEXP lang, SEXP arg_lang, SEXP arg_tag, SEXP arg_value, SEXP lang_full,
  struct VALC_settings set
) {
  if(!IS_LANG(arg_lang))
    error("Internal Error: argument `arg_lang` must be language.");  // nocov

  SEXP lang_parsed = PROTECT(VALC_parse(lang, arg_lang, set));
  SEXP res = PROTECT(
    VALC_evaluate_recurse(
      VECTOR_ELT(lang_parsed, 0),
      VECTOR_ELT(lang_parsed, 1),
      arg_value, arg_lang, arg_tag, lang_full, set
  ) );
  // Now determine if we passed or failed
  /*
      if(mode == 10) {
        // Tried to do this as part of init originally so we don't have to repeat
        // wholesale creation of call, but the call elements kept getting over
        // written by other stuff.  Not sure how to protect in calls defined at
        // top level

        SEXP err_attrib;
        const char * err_call;

        // If message attribute defined, this is easy:

        if((err_attrib = getAttrib(lang, VALC_SYM_errmsg)) != R_NilValue) {
          if(TYPEOF(err_attrib) != STRSXP || XLENGTH(err_attrib) != 1) {
            VALC_arg_error(
              arg_tag, lang_full,
              "\"err.msg\" attribute for validation token for argument `%s` must be a one length character vector."
            );
          }
          err_call = ALIKEC_pad_or_quote(arg_lang, set.width, -1, set);

          // Need to make copy of string, modify it, and turn it back into
          // string

          const char * err_attrib_msg = CHAR(STRING_ELT(err_attrib, 0));
          char * err_attrib_mod = CSR_smprintf4(
            set.nchar_max, err_attrib_msg, err_call, "", "", ""
          );
          // not protecting mkString since assigning to protected object
          SETCAR(err_msg, mkString(err_attrib_mod));
        } else {
          // message attribute not defined, must construct error message based
          // on result of evaluation

          err_call = ALIKEC_pad_or_quote(lang, set.width, -1, set);

          char * err_str;
          char * err_tok;
          switch(eval_res_c) {
            case -6: {
                R_xlen_t eval_res_len = xlength(eval_res);
                err_tok = CSR_smprintf4(
                  set.nchar_max,
                  "chr%s \"%s\"%s%s",
                  eval_res_len > 1 ?
                    CSR_smprintf2(
                      set.nchar_max, " [1:%s]%s", CSR_len_as_chr(eval_res_len),
                      ""
                    ) : "",
                  CHAR(STRING_ELT(eval_res, 0)),
                  eval_res_len > 1 ? " ..." : "",
                  ""
                );
              }
              break;
            case -2: {
              const char * err_tok_tmp = type2char(TYPEOF(eval_tmp));
              const char * err_tok_base = "is \"%s\" instead of a \"logical\"";
              err_tok = R_alloc(
                strlen(err_tok_tmp) + strlen(err_tok_base), sizeof(char)
              );
              if(sprintf(err_tok, err_tok_base, err_tok_tmp) < 0)
                // nocov start
                error(
                  "Internal error: build token error failure; contact maintainer"
                );
                // nocov end
              }
              break;
            case -1: err_tok = "FALSE"; break;
            case -3: err_tok = "NA"; break;
            case -4: err_tok = "contains NAs"; break;
            case -5: err_tok = "zero length"; break;
            case 0: err_tok = "contains non-TRUE values"; break;
            default: {
              // nocov start
              error(
                "Internal Error: %s %d; contact maintainer.",
                "unexpected user exp eval value", eval_res_c
              );
              // nocov end
            }
          }
          const char * err_extra_a = "is not all TRUE";
          const char * err_extra_b = "is not TRUE"; // must be shorter than _a
          const char * err_extra;
          if(eval_res_c == 0) {
            err_extra = err_extra_a;
          } else {
            err_extra = err_extra_b;
          }
          const char * err_base = "%s%s (%s)";
          err_str = R_alloc(
            strlen(err_call) + strlen(err_base) + strlen(err_tok) +
            strlen(err_extra), sizeof(char)
          );
          // not sure why we're not using cstringr here
          if(sprintf(err_str, err_base, err_call, err_extra, err_tok) < 0) {
            // nocov start
            error(
              "%s%s", "Internal Error: could not construct error message; ",
              "contact maintainer."
            );
            // nocov end
          }
          SETCAR(err_msg, mkString(err_str));
        }
      } else { // must have been `alike` eval
        SETCAR(err_msg, eval_res);
      }
  */

  // Remove duplicates, if any

  switch(TYPEOF(res)) {
    case LGLSXP:  break;
    // this is likely the result of an OR evaluation, and should only get
    // produced if every check failed so we should have an error message for
    // every last one
    case LISTSXP: {
      SEXP res_cpy, res_next, res_next_next, res_val;
      for(
        res_cpy = res; res_cpy != R_NilValue;
        res_cpy = res_next
      ) {
        res_next = CDR(res_cpy);
        res_val = CAR(res_cpy);
        if(TYPEOF(res_val) != STRSXP) {
          // nocov start
          error(
            "%s%s",
            "Internal Error: non string value in return pairlist; contact ",
            "maintainer."
          );
          // nocov end
        }
        if(res_next == R_NilValue) break;
        // Need to remove res_next
        if(R_compute_identical(CAR(res_next), res_val, 16)) {
          res_next_next = CDR(res_next);
          SETCDR(res_cpy, res_next_next);
      } }
    } break;
    default: {
      // nocov start
      error(
        "Internal Error: unexpected evaluating return type; contact maintainer."
      );
      // nocov end
    }
  }
  UNPROTECT(2);  // This seems a bit stupid, PROTECT/UNPROTECT
  return(res);
}
SEXP VALC_evaluate_ext(
  SEXP lang, SEXP arg_lang, SEXP arg_tag, SEXP arg_value, SEXP lang_full,
  SEXP rho
) {
  struct VALC_settings set = VALC_settings_vet(R_NilValue, rho);
  return VALC_evaluate(lang, arg_lang, arg_tag, arg_value, lang_full, set);
}
