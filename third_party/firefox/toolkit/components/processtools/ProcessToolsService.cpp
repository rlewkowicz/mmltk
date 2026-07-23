/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/StaticPtr.h"
#include "nsCOMPtr.h"
#include "ProcessToolsService.h"

namespace {
extern "C" {
void new_process_tools_service(nsIProcessToolsService** result);
}

static mozilla::StaticRefPtr<nsIProcessToolsService> sProcessToolsService;
}  

already_AddRefed<nsIProcessToolsService> GetProcessToolsService() {
  nsCOMPtr<nsIProcessToolsService> processToolsService;

  if (sProcessToolsService) {
    processToolsService = sProcessToolsService;
  } else {
    new_process_tools_service(getter_AddRefs(processToolsService));
    sProcessToolsService = processToolsService;
    mozilla::ClearOnShutdown(&sProcessToolsService);
  }

  return processToolsService.forget();
}
