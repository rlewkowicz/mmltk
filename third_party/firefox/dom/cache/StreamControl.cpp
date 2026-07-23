/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/StreamControl.h"

namespace mozilla::dom::cache {

void StreamControl::AddReadStream(
    SafeRefPtr<ReadStream::Controllable> aReadStream) {
  AssertOwningThread();
  MOZ_DIAGNOSTIC_ASSERT(aReadStream);
  MOZ_ASSERT(!mReadStreamList.Contains(aReadStream));
  mReadStreamList.AppendElement(std::move(aReadStream));
}

void StreamControl::ForgetReadStream(
    SafeRefPtr<ReadStream::Controllable> aReadStream) {
  AssertOwningThread();
  MOZ_ALWAYS_TRUE(mReadStreamList.RemoveElement(aReadStream));
}

void StreamControl::NoteClosed(SafeRefPtr<ReadStream::Controllable> aReadStream,
                               const nsID& aId) {
  AssertOwningThread();
  ForgetReadStream(std::move(aReadStream));
  NoteClosedAfterForget(aId);
}

StreamControl::~StreamControl() {
  MOZ_DIAGNOSTIC_ASSERT(mReadStreamList.IsEmpty());
}

void StreamControl::CloseAllReadStreams() {
  AssertOwningThread();

  auto readStreamList = mReadStreamList.Clone();
  for (const auto& stream : readStreamList.ForwardRange()) {
    stream->CloseStream();
  }
}

void StreamControl::CloseAllReadStreamsWithoutReporting() {
  AssertOwningThread();

  for (const auto& stream : mReadStreamList.ForwardRange()) {
    stream->CloseStreamWithoutReporting();
  }
}

bool StreamControl::HasEverBeenRead() const {
  const auto range = mReadStreamList.NonObservingRange();
  return std::any_of(range.begin(), range.end(), [](const auto& stream) {
    return stream->HasEverBeenRead();
  });
}

}  
