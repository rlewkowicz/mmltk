/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_REMOTE_NSREMOTECLIENT_H_
#define TOOLKIT_COMPONENTS_REMOTE_NSREMOTECLIENT_H_

#include "nscore.h"


class nsRemoteClient {
 public:
  virtual ~nsRemoteClient() = default;

  virtual nsresult Init() = 0;

  virtual nsresult SendCommandLine(const char* aProgram, const char* aProfile,
                                   int32_t argc, const char** argv,
                                   bool aRaise) = 0;
};

#endif  // TOOLKIT_COMPONENTS_REMOTE_NSREMOTECLIENT_H_
