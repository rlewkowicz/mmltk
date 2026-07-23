# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from collections import namedtuple




GeckoProcessType = namedtuple(
    "GeckoProcessType",
    [
        "enum_value",
        "enum_name",
        "string_name",
        "proc_typename",
        "process_bin_type",
        "procinfo_typename",
        "webidl_typename",
        "allcaps_name",
        "crash_ping",
    ],
)

process_types = [
    GeckoProcessType(
        0,
        "Default",
        "default",
        "Parent",
        "Self",
        "Browser",
        "Browser",
        "DEFAULT",
        True,
    ),
    GeckoProcessType(
        2,
        "Content",
        "tab",
        "Content",
        "Self",
        "Content",
        "Content",
        "CONTENT",
        True,
    ),
    GeckoProcessType(
        3,
        "IPDLUnitTest",
        "ipdlunittest",
        "IPDLUnitTest",
        "Self",
        "IPDLUnitTest",
        "IpdlUnitTest",
        "IPDLUNITTEST",
        False,
    ),
    GeckoProcessType(
        5,
        "GPU",
        "gpu",
        "GPU",
        "Self",
        "GPU",
        "Gpu",
        "GPU",
        True,
    ),
    GeckoProcessType(
        7,
        "RDD",
        "rdd",
        "RDD",
        "Self",
        "RDD",
        "Rdd",
        "RDD",
        True,
    ),
    GeckoProcessType(
        8,
        "Socket",
        "socket",
        "Socket",
        "Self",
        "Socket",
        "Socket",
        "SOCKET",
        True,
    ),
    GeckoProcessType(
        10,
        "ForkServer",
        "forkserver",
        "ForkServer",
        "Self",
        "ForkServer",
        "ForkServer",
        "FORKSERVER",
        True,
    ),
    GeckoProcessType(
        11,
        "Utility",
        "utility",
        "Utility",
        "Self",
        "Utility",
        "Utility",
        "UTILITY",
        True,
    ),
]
