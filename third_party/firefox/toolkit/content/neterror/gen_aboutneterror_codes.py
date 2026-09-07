#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

import re
import sys


MESSAGE_ID = re.compile(r"^([A-Za-z][A-Za-z0-9_-]*)\s*=", re.MULTILINE)


def find_error_ids(filename, known_strings):
    with open(filename, encoding="utf-8") as f:
        known_strings.extend(MESSAGE_ID.findall(f.read()))


def main(output, *filenames):
    known_strings = []
    for filename in filenames:
        find_error_ids(filename, known_strings)

    output.write("const KNOWN_ERROR_MESSAGE_IDS = new Set([\n")
    for known_string in known_strings:
        output.write(f'  "{known_string}",\n')
    output.write("]);\n")


if __name__ == "__main__":
    sys.exit(main(sys.stdout, *sys.argv[1:]))
