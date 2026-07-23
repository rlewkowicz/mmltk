/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ValueUsage_h
#define frontend_ValueUsage_h

namespace js {
namespace frontend {

enum class ValueUsage {
  WantValue,

  IgnoreValue
};

} 
} 

#endif /* frontend_ValueUsage_h */
