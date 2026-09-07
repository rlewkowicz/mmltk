// Copyright 2007-2010 Baptiste Lepilleur and The JsonCpp Authors
// Distributed under MIT license, or public domain if desired and
// See file LICENSE for detail or copy at http://jsoncpp.sourceforge.net/LICENSE

#ifndef JSON_FEATURES_H_INCLUDED
#define JSON_FEATURES_H_INCLUDED

#if !defined(JSON_IS_AMALGAMATION)
#include "forwards.h"
#endif // if !defined(JSON_IS_AMALGAMATION)

#pragma pack(push)
#pragma pack()

namespace Json {

class JSON_API Features {
public:
  static Features all();

  static Features strictMode();

  Features();

  bool allowComments_{true};

  bool strictRoot_{false};

  bool allowDroppedNullPlaceholders_{false};

  bool allowNumericKeys_{false};
};

} 

#pragma pack(pop)

#endif // JSON_FEATURES_H_INCLUDED
