/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_AtomAttributes_h_
#define mozilla_dom_AtomAttributes_h_

#include "nsGkAtoms.h"


// clang-format off

#define NS_IS_ATOM_ATTRIBUTE(aAtom)             \
  (aAtom == nsGkAtoms::lang ||                  \
   aAtom == nsGkAtoms::form ||                  \
   aAtom == nsGkAtoms::_for ||                  \
   aAtom == nsGkAtoms::aria_activedescendant || \
   aAtom == nsGkAtoms::id)

#define NS_IS_ATOM_ARRAY_ATTRIBUTE(aAtom)       \
  (aAtom == nsGkAtoms::_class ||                \
   aAtom == nsGkAtoms::part ||                  \
   aAtom == nsGkAtoms::aria_actions ||          \
   aAtom == nsGkAtoms::aria_controls ||         \
   aAtom == nsGkAtoms::aria_describedby ||      \
   aAtom == nsGkAtoms::aria_details ||          \
   aAtom == nsGkAtoms::aria_errormessage ||     \
   aAtom == nsGkAtoms::aria_flowto ||           \
   aAtom == nsGkAtoms::aria_labelledby ||       \
   aAtom == nsGkAtoms::aria_owns ||             \
   aAtom == nsGkAtoms::headers)

#define NS_IS_ATOM_ATTRIBUTE_HTML(aAtom)        \
  (aAtom == nsGkAtoms::popovertarget ||         \
   aAtom == nsGkAtoms::name ||                  \
   aAtom == nsGkAtoms::contenteditable ||       \
   aAtom == nsGkAtoms::translate)

#define NS_IS_ATOM_ARRAY_ATTRIBUTE_HTML(aAtom)  \
  (aAtom == nsGkAtoms::sandbox ||               \
   aAtom == nsGkAtoms::sizes ||                 \
   aAtom == nsGkAtoms::blocking ||              \
   aAtom == nsGkAtoms::rel)

// clang-format on


#endif  // mozilla_dom_AtomAttributes_h_
