/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XPCJSMemoryReporter_h
#define XPCJSMemoryReporter_h

class nsISupports;
class nsIHandleReportCallback;

namespace xpc {

typedef nsTHashMap<nsUint64HashKey, nsCString> WindowPaths;

class JSReporter {
 public:
  static void CollectReports(WindowPaths* windowPaths,
                             WindowPaths* topWindowPaths,
                             nsIHandleReportCallback* handleReport,
                             nsISupports* data, bool anonymize);
};

}  

#endif
