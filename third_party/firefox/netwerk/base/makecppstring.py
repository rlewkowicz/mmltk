# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import sys


def main(output, filename):
    with open(filename) as file:
        output.write('R"(')
        for line in file:
            output.write(line)
        output.write(')"')


if __name__ == "__main__":
    main(sys.stdout, sys.argv[1])
