# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Pinned bootstrap inputs for the owned Linux browser source tree."""

import functools
import json
from pathlib import Path


@functools.cache
def toolchain_task_definitions():
    # This source distribution deliberately omits the CI taskgraph. Keep its
    # selected bootstrap artifacts as data; installation and verification stay
    # with configure's ordinary bootstrap owner.
    manifest = Path(__file__).resolve().parents[3] / "build" / "minimal-toolchains.json"
    with manifest.open() as stream:
        return json.load(stream)["tasks"]
