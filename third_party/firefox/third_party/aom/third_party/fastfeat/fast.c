// Copyright (c) 2006, 2008 Edward Rosten
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
//  *Redistributions of source code must retain the above copyright
//  *Redistributions in binary form must reproduce the above copyright
//  *Neither the name of the University of Cambridge nor the names of
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER

// clang-format off
#include <stdlib.h>
#include "fast.h"


xy* aom_fast9_detect_nonmax(const byte* im, int xsize, int ysize, int stride, int b,
                            int** ret_scores, int* ret_num_corners)
{
  xy* corners;
  int num_corners;
  int* scores;
  xy* nonmax;

  corners = aom_fast9_detect(im, xsize, ysize, stride, b, &num_corners);
  if(!corners)
  {
    *ret_num_corners = -1;
    return NULL;
  }
  scores = aom_fast9_score(im, stride, corners, num_corners, b);
  if(!scores && num_corners > 0)
  {
    free(corners);
    *ret_num_corners = -1;
    return NULL;
  }
  nonmax = aom_nonmax_suppression(corners, scores, num_corners, ret_scores, ret_num_corners);

  free(corners);
  free(scores);

  return nonmax;
}
// clang-format on
