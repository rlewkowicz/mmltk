/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SoundTouchAdapter.h"

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"

namespace mozilla {

void SoundTouchAdapter::setChannels(uint aChannels) {
  MOZ_RELEASE_ASSERT(aChannels > 0 && aChannels <= SOUNDTOUCH_MAX_CHANNELS);
  mChannels = aChannels;
  mProcessor.setChannels(aChannels);
}

void SoundTouchAdapter::putSamples(const AudioDataValue* aSamples,
                                   uint aNumSamples) {
  MOZ_RELEASE_ASSERT(mChannels > 0);
  CheckedInt<size_t> elements =
      CheckedInt<size_t>(mChannels) * aNumSamples;
  MOZ_RELEASE_ASSERT(elements.isValid());
  mProcessor.putSamples(aSamples, aNumSamples);
}

uint SoundTouchAdapter::receiveSamples(AudioDataValue* aOutput,
                                       uint aMaxSamples) {
  MOZ_RELEASE_ASSERT(mChannels > 0);
  uint written = mProcessor.receiveSamples(aOutput, aMaxSamples);
  MOZ_RELEASE_ASSERT(written <= aMaxSamples);
  return written;
}

}  
