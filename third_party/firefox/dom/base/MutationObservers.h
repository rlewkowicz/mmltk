/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_BASE_MUTATIONOBSERVERS_H_
#define DOM_BASE_MUTATIONOBSERVERS_H_

#include "mozilla/DoublyLinkedList.h"
#include "nsIContent.h"  // for use in inline function (NotifyParentChainChanged)
#include "nsIMutationObserver.h"  // for use in inline function (NotifyParentChainChanged)
#include "nsINode.h"

class nsAtom;
class nsAttrValue;
struct BatchRemovalState;

namespace mozilla::dom {
class Animation;
class Element;

class MutationObservers {
 public:
  static void NotifyCharacterDataWillChange(nsIContent* aContent,
                                            const CharacterDataChangeInfo&);

  static void NotifyCharacterDataChanged(nsIContent* aContent,
                                         const CharacterDataChangeInfo&);

  static void NotifyAttributeWillChange(mozilla::dom::Element* aElement,
                                        int32_t aNameSpaceID,
                                        nsAtom* aAttribute,
                                        AttrModType aModType);

  static void NotifyAttributeChanged(mozilla::dom::Element* aElement,
                                     int32_t aNameSpaceID, nsAtom* aAttribute,
                                     AttrModType aModType,
                                     const nsAttrValue* aOldValue);

  static void NotifyAttributeSetToCurrentValue(mozilla::dom::Element* aElement,
                                               int32_t aNameSpaceID,
                                               nsAtom* aAttribute);

  static void NotifyContentAppended(nsIContent* aContainer,
                                    nsIContent* aFirstNewContent,
                                    const ContentAppendInfo&);

  static void NotifyContentInserted(nsINode* aContainer, nsIContent* aChild,
                                    const ContentInsertInfo&);
  static void NotifyContentWillBeRemoved(nsINode* aContainer,
                                         nsIContent* aChild,
                                         const ContentRemoveInfo&);

  static inline void NotifyParentChainChanged(nsIContent* aContent) {
    mozilla::SafeDoublyLinkedList<nsIMutationObserver>* observers =
        aContent->GetMutationObservers();
    if (observers) {
      for (auto iter = observers->begin(); iter != observers->end(); ++iter) {
        if (iter->IsCallbackEnabled(nsIMutationObserver::kParentChainChanged)) {
          iter->ParentChainChanged(aContent);
        }
      }
    }
  }

  static void NotifyAnimationAdded(mozilla::dom::Animation* aAnimation);
  static void NotifyAnimationChanged(mozilla::dom::Animation* aAnimation);
  static void NotifyAnimationRemoved(mozilla::dom::Animation* aAnimation);

 private:
  enum class AnimationMutationType { Added, Changed, Removed };
  static void NotifyAnimationMutated(mozilla::dom::Animation* aAnimation,
                                     AnimationMutationType aMutatedType);
};
}  

#endif  // DOM_BASE_MUTATIONOBSERVERS_H_
