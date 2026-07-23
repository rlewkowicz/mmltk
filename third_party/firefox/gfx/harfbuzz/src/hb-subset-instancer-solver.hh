/*
 * Copyright © 2023  Behdad Esfahbod
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#ifndef HB_SUBSET_INSTANCER_SOLVER_HH
#define HB_SUBSET_INSTANCER_SOLVER_HH

#include "hb.hh"

struct TripleDistances
{
  TripleDistances (): negative (1.0), positive (1.0) {}
  TripleDistances (double neg_, double pos_): negative (neg_), positive (pos_) {}
  TripleDistances (double min, double default_, double max)
  {
    negative = default_ - min;
    positive = max - default_;
  }

  double negative;
  double positive;
};

struct Triple
{
  Triple () = default;

  Triple (double minimum_, double middle_, double maximum_) :
    minimum (minimum_), middle (middle_), maximum (maximum_) {}

  bool operator == (const Triple &o) const
  {
    return minimum == o.minimum &&
	   middle  == o.middle  &&
	   maximum == o.maximum;
  }

  bool operator != (const Triple o) const
  { return !(*this == o); }

  bool is_point () const
  { return minimum == middle && middle == maximum; }

  bool contains (double point) const
  { return minimum <= point && point <= maximum; }

  uint32_t hash () const
  {
    uint32_t current = 0x84222325;
    current = current ^ hb_hash (minimum);
    current = current * 16777619;

    current = current ^ hb_hash (middle);
    current = current * 16777619;

    current = current ^ hb_hash (maximum);
    current = current * 16777619;
    return current;
  }

  double minimum = 0;
  double middle = 0;
  double maximum = 0;
};

using rebase_tent_result_item_t = hb_pair_t<double, Triple>;
using rebase_tent_result_t = hb_vector_t<rebase_tent_result_item_t>;

HB_INTERNAL double renormalizeValue (double v, const Triple &triple,
                                    const TripleDistances &triple_distances,
                                    bool extrapolate = true);
HB_INTERNAL void rebase_tent (Triple tent,
			      Triple axisLimit,
			      TripleDistances axis_triple_distances,
			      rebase_tent_result_t &out,
			      rebase_tent_result_t &scratch);

#endif /* HB_SUBSET_INSTANCER_SOLVER_HH */
