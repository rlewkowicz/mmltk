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
#ifndef FAST_H
#define FAST_H

typedef struct { int x, y; } xy;
typedef unsigned char byte;

xy* aom_fast9_detect(const byte* im, int xsize, int ysize, int stride, int b, int* ret_num_corners);

int* aom_fast9_score(const byte* i, int stride, const xy* corners, int num_corners, int b);

xy* aom_fast9_detect_nonmax(const byte* im, int xsize, int ysize, int stride, int b,
                            int** ret_scores, int* ret_num_corners);

xy* aom_nonmax_suppression(const xy* corners, const int* scores, int num_corners,
                           int** ret_scores, int* ret_num_nonmax);


#endif
// clang-format on
