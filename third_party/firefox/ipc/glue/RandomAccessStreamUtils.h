/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_RandomAccessStreamUtils_h
#define mozilla_ipc_RandomAccessStreamUtils_h

template <class T>
class nsCOMPtr;

class nsIInterfaceRequestor;
class nsIRandomAccessStream;

namespace mozilla {

template <class T>
class Maybe;

template <typename T>
class MovingNotNull;

template <typename V, typename E>
class Result;

namespace ipc {

class RandomAccessStreamParams;

RandomAccessStreamParams SerializeRandomAccessStream(
    MovingNotNull<nsCOMPtr<nsIRandomAccessStream>> aStream,
    nsIInterfaceRequestor* aCallbacks);

Maybe<RandomAccessStreamParams> SerializeRandomAccessStream(
    nsCOMPtr<nsIRandomAccessStream> aStream, nsIInterfaceRequestor* aCallbacks);

Result<MovingNotNull<nsCOMPtr<nsIRandomAccessStream>>, bool>
DeserializeRandomAccessStream(RandomAccessStreamParams& aStreamParams);

Result<nsCOMPtr<nsIRandomAccessStream>, bool> DeserializeRandomAccessStream(
    Maybe<RandomAccessStreamParams>& aStreamParams);

}  
}  

#endif  // mozilla_ipc_RandomAccessStreamUtils_h
