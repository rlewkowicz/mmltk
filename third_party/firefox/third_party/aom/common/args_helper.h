/*
 * Copyright (c) 2020, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_COMMON_ARGS_HELPER_H_)
#define AOM_COMMON_ARGS_HELPER_H_

#include "aom/aom_encoder.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define ARG_ERR_MSG_MAX_LEN 200

struct arg {
  char **argv;
  const char *name;
  const char *val;
  unsigned int argv_step;
  const struct arg_def *def;
};

struct arg_enum_list {
  const char *name;
  int val;
};
#define ARG_ENUM_LIST_END { 0 }

typedef struct arg_def {
  const char *short_name;
  const char *long_name;
  int has_val;  
  const char *desc;
  const struct arg_enum_list *enums;
} arg_def_t;
#define ARG_DEF(s, l, v, d) { s, l, v, d, NULL }
#define ARG_DEF_ENUM(s, l, v, d, e) { s, l, v, d, e }
#define ARG_DEF_LIST_END { 0 }

int arg_match_helper(struct arg *arg_, const struct arg_def *def, char **argv,
                     char *err_msg);

unsigned int arg_parse_uint_helper(const struct arg *arg, char *err_msg);
int arg_parse_int_helper(const struct arg *arg, char *err_msg);
struct aom_rational arg_parse_rational_helper(const struct arg *arg,
                                              char *err_msg);
int arg_parse_enum_helper(const struct arg *arg, char *err_msg);
int arg_parse_enum_or_int_helper(const struct arg *arg, char *err_msg);
int arg_parse_list_helper(const struct arg *arg, int *list, int n,
                          char *err_msg);

#if defined(__cplusplus)
}  
#endif

#endif
