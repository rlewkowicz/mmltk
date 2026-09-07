/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BTTypes_h
#define BTTypes_h

#include <vector>

#include "Buffer.h"

namespace mozilla {
namespace ct {


struct InclusionProofDataV2 {
  Buffer logId;
  uint64_t treeSize;
  uint64_t leafIndex;
  std::vector<Buffer> inclusionPath;
};


struct SignedTreeHeadDataV2 {
  Buffer logId;
  uint64_t timestamp;
  uint64_t treeSize;
  Buffer rootHash;
};

}  
}  

#endif  // BTTypes_h
