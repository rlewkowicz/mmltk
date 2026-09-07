/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsGkAtoms.h"
#include "mozilla/HashFunctions.h"

namespace mozilla::detail {

extern constexpr GkAtoms gGkAtoms = {
#define GK_ATOM(name_, value_) u"" value_,
#include "nsGkAtomList.h"
#undef GK_ATOM
    {
#define GK_ATOM(name_, value_)                                                \
  nsStaticAtom(                                                               \
      sizeof(value_) - 1, mozilla::HashString(u"" value_),                    \
      offsetof(GkAtoms, mAtoms[static_cast<size_t>(GkAtoms::Atoms::name_)]) - \
          offsetof(GkAtoms, name_##_string),                                  \
      nsAtom::ComputeIsAsciiLowercase(u"" value_)),
#include "nsGkAtomList.h"
#undef GK_ATOM
    }};

}  

const nsStaticAtom* const nsGkAtoms::sAtoms = mozilla::detail::gGkAtoms.mAtoms;
