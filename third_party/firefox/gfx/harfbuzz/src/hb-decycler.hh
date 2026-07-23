/*
 * Copyright © 2025 Behdad Esfahbod
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
 *
 * Author(s): Behdad Esfahbod
 */

#ifndef HB_DECYCLER_HH
#define HB_DECYCLER_HH

#include "hb.hh"


struct hb_decycler_node_t;

struct hb_decycler_t
{
  friend struct hb_decycler_node_t;

  private:
  bool tortoise_awake = false;
  hb_decycler_node_t *tortoise = nullptr;
  hb_decycler_node_t *hare = nullptr;
};

struct hb_decycler_node_t
{
  hb_decycler_node_t (hb_decycler_t &decycler)
  {
    u.decycler = &decycler;

    decycler.tortoise_awake = !decycler.tortoise_awake;

    if (!decycler.tortoise)
    {
      assert (decycler.tortoise_awake);
      assert (!decycler.hare);
      decycler.tortoise = decycler.hare = this;
      return;
    }

    if (decycler.tortoise_awake)
      decycler.tortoise = decycler.tortoise->u.next; 

    this->prev = decycler.hare;
    decycler.hare->u.next = this;
    decycler.hare = this;
  }

  ~hb_decycler_node_t ()
  {
    hb_decycler_t &decycler = *u.decycler;


    assert (decycler.hare == this);
    decycler.hare = prev;
    if (prev)
      prev->u.decycler = &decycler;

    assert (decycler.tortoise);
    if (decycler.tortoise_awake)
      decycler.tortoise = decycler.tortoise->prev;

    decycler.tortoise_awake = !decycler.tortoise_awake;
  }

  bool visit (uintptr_t value_)
  {
    value = value_;

    hb_decycler_t &decycler = *u.decycler;

    if (decycler.tortoise == this)
      return true; 

    if (decycler.tortoise->value == value)
      return false; 

    return true;
  }

  private:
  union {
    hb_decycler_t *decycler;
    hb_decycler_node_t *next;
  } u = {nullptr};
  hb_decycler_node_t *prev = nullptr;
  uintptr_t value = 0;
};

#endif /* HB_DECYCLER_HH */
