/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsCycleCollectionContainerParticipant_h_
#define nsCycleCollectionContainerParticipant_h_

#include <type_traits>

template <typename, template <typename...> typename>
struct ImplCycleCollectionIsContainerT : std::false_type {};

template <template <typename...> typename Container, typename... Args>
struct ImplCycleCollectionIsContainerT<Container<Args...>, Container>
    : std::true_type {};

template <typename T, template <typename...> typename Container>
constexpr bool ImplCycleCollectionIsContainer =
    ImplCycleCollectionIsContainerT<std::remove_cvref_t<T>, Container>::value;

template <typename T, template <typename...> typename Container>
using EnableCycleCollectionIf =
    typename std::enable_if_t<ImplCycleCollectionIsContainer<T, Container>>*;

#endif  // nsCycleCollectionContainerParticipant_h_
