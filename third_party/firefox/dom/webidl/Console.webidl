/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * For more information on this interface, please see
 * https://console.spec.whatwg.org/#console-namespace
 */

[Exposed=(Window,Worker,Worklet),
 ClassString="Console",
 ProtoObjectHack]
namespace console {

  // NOTE: if you touch this namespace, remember to update the ConsoleInstance
  // interface as well! - dom/chrome-webidl/ConsoleInstance.webidl

  // Logging
    undefined assert(optional boolean condition = false, any... data);
    undefined clear();
    undefined count(optional DOMString label = "default");
    undefined countReset(optional DOMString label = "default");
    undefined debug(any... data);
    undefined error(any... data);
    undefined info(any... data);
    undefined log(any... data);
    undefined table(any... data); // FIXME: The spec is still unclear about this.
    undefined trace(any... data);
    undefined warn(any... data);
    undefined dir(any... data); // FIXME: This doesn't follow the spec yet.
    undefined dirxml(any... data);

  // Grouping
    undefined group(any... data);
    undefined groupCollapsed(any... data);
    undefined groupEnd();

  // Timing
    undefined time(optional DOMString label = "default");
    undefined timeLog(optional DOMString label = "default", any... data);
    undefined timeEnd(optional DOMString label = "default");

  // Mozilla only or Webcompat methods

    undefined _exception(any... data);
    undefined timeStamp(optional any data);

    undefined profile(any... data);
    undefined profileEnd(any... data);

  [ChromeOnly]
  const boolean IS_NATIVE_CONSOLE = true;

  [Func="IsChromeOrWorkerDebugger", NewObject]
  ConsoleInstance createInstance(optional ConsoleInstanceOptions options = {});
};
