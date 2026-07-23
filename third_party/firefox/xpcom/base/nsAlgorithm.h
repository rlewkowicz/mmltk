/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsAlgorithm_h_
#define nsAlgorithm_h_

template <class T>
inline const T& XPCOM_MIN(const T& aA, const T& aB) {
  return aB < aA ? aB : aA;
}

template <class T>
inline const T& XPCOM_MAX(const T& aA, const T& aB) {
  return aA > aB ? aA : aB;
}

#endif  // !defined(nsAlgorithm_h_)
