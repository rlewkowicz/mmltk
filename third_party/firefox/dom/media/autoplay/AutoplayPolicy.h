/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(AutoplayPolicy_h_)
#  define AutoplayPolicy_h_

#  include <cstdint>

class nsIPrincipal;
class nsPIDOMWindowInner;

namespace mozilla::dom {

class HTMLMediaElement;
class AudioContext;
class Document;
enum class AutoplayPolicy : uint8_t;
enum class AutoplayPolicyMediaType : uint8_t;

}  

namespace mozilla::media {
class AutoplayPolicy {
 public:
  static bool IsAllowedToPlay(const dom::HTMLMediaElement& aElement);

  static bool IsAllowedToPlay(const dom::AudioContext& aContext);

  static bool IsAudioInterruptedByPlatform(nsPIDOMWindowInner* aWindow);

  static uint32_t GetSiteAutoplayPermission(nsIPrincipal* aPrincipal);

  static dom::AutoplayPolicy GetAutoplayPolicy(
      const dom::HTMLMediaElement& aElement);

  static dom::AutoplayPolicy GetAutoplayPolicy(
      const dom::AudioContext& aContext);

  static dom::AutoplayPolicy GetAutoplayPolicy(
      const dom::AutoplayPolicyMediaType& aType, const dom::Document& aDoc);
};

}  

#endif
