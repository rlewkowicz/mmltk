/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkQuads_DEFINED)
#define SkQuads_DEFINED

class SkQuads {
public:
    static double Discriminant(double A, double B, double C);

    struct RootResult {
        double discriminant;
        double root0;
        double root1;
    };

    static RootResult Roots(double A, double B, double C);

    static int RootsReal(double A, double B, double C, double solution[2]);

    static double EvalAt(double A, double B, double C, double t);
};

#endif
