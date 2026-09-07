#!/bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


QUIET=0

MAR=${MAR:-mar}
MBSDIFF=${MBSDIFF:-mbsdiff}
XZ=${XZ:-xz}
$XZ --version > /dev/null 2>&1
if [ $? -ne 0 ]; then
  XZ="$(dirname "$(dirname "$(dirname "$0")")")/xz/xz.exe"
  $XZ --version > /dev/null 2>&1
  if [ $? -ne 0 ]; then
    XZ="$(dirname "$(dirname "$(dirname "$(dirname "$0")")")")/xz/xz.exe"
    $XZ --version > /dev/null 2>&1
    if [ $? -ne 0 ]; then
      echo "xz was not found on this system!"
      echo "exiting"
      exit 1
    fi
  fi
fi
export XZ_OPT="-T1 -7e"


notice() {
  echo "$*" 1>&2
}

verbose_notice() {
  if [ $QUIET -eq 0 ]; then
    notice "$*"
  fi
}

get_file_size() {
  info=($(ls -ln "$1"))
  echo ${info[4]}
}

copy_perm() {
  reference="$1"
  target="$2"

  if [ -x "$reference" ]; then
    chmod 0755 "$target"
  else
    chmod 0644 "$target"
  fi
}

make_add_instruction() {
  f="$1"
  filev3="$2"

  if [ $4 ]; then
    forced=" (forced)"
  else
    forced=
  fi

  is_extension=$(echo "$f" | grep -c 'distribution/extensions/.*/')
  if [ $is_extension = "1" ]; then
    testdir=$(echo "$f" | sed 's/\(.*distribution\/extensions\/[^\/]*\)\/.*/\1/')
    verbose_notice "     add-if \"$testdir\" \"$f\""
    echo "add-if \"$testdir\" \"$f\"" >> "$filev3"
  else
    verbose_notice "        add \"$f\"$forced"
    echo "add \"$f\"" >> "$filev3"
  fi
}

check_for_add_if_not_update() {
  add_if_not_file_chk="$1"

  if [[ "$(basename "$add_if_not_file_chk")" = "channel-prefs.js" || \
        "$add_if_not_file_chk" =~ (^|/)ChannelPrefs\.framework/ || \
        "$(basename "$add_if_not_file_chk")" = "update-settings.ini" || \
        "$add_if_not_file_chk" =~ (^|/)UpdateSettings\.framework/ || \
        "$(basename "$add_if_not_file_chk")" = "distribution.ini" ]]; then
    return 0;
  fi
  return 1;
}

make_add_if_not_instruction() {
  f="$1"
  filev3="$2"

  verbose_notice " add-if-not \"$f\" \"$f\""
  echo "add-if-not \"$f\" \"$f\"" >> "$filev3"
}

make_patch_instruction() {
  f="$1"
  filev3="$2"

  is_extension=$(echo "$f" | grep -c 'distribution/extensions/.*/')
  if [ $is_extension = "1" ]; then
    testdir=$(echo "$f" | sed 's/\(.*distribution\/extensions\/[^\/]*\)\/.*/\1/')
    verbose_notice "   patch-if \"$testdir\" \"$f.patch\" \"$f\""
    echo "patch-if \"$testdir\" \"$f.patch\" \"$f\"" >> "$filev3"
  else
    verbose_notice "      patch \"$f.patch\" \"$f\""
    echo "patch \"$f.patch\" \"$f\"" >> "$filev3"
  fi
}

append_remove_instructions() {
  dir="$1"
  filev3="$2"

  if [ -f "$dir/removed-files" ]; then
    listfile="$dir/removed-files"
  elif [ -f "$dir/Contents/Resources/removed-files" ]; then
    listfile="$dir/Contents/Resources/removed-files"
  fi
  if [ -n "$listfile" ]; then
    files=($(cat "$listfile" | tr " " "|"  | sort -r))
    num_files=${#files[*]}
    for ((i=0; $i<$num_files; i=$i+1)); do
      f=$(echo ${files[$i]} | tr "|" " " | tr -d '\r')
      f=$(echo $f)
      if [ -n "$f" ]; then
        if [ ! $(echo "$f" | grep -c '^#') = 1 ]; then
          if [ $(echo "$f" | grep -c '\/$') = 1 ]; then
            verbose_notice "      rmdir \"$f\""
            echo "rmdir \"$f\"" >> "$filev3"
          elif [ $(echo "$f" | grep -c '\/\*$') = 1 ]; then
            f=$(echo "$f" | sed -e 's:\*$::')
            verbose_notice "    rmrfdir \"$f\""
            echo "rmrfdir \"$f\"" >> "$filev3"
          else
            verbose_notice "     remove \"$f\""
            echo "remove \"$f\"" >> "$filev3"
          fi
        fi
      fi
    done
  fi
}

list_files() {
  count=0
  temp_filelist=$(mktemp)
  find . -type f \
    ! -name "update.manifest" \
    ! -name "updatev2.manifest" \
    ! -name "updatev3.manifest" \
    | sed 's/\.\/\(.*\)/\1/' \
    | sort -r > "${temp_filelist}"
  while read file; do
    eval "${1}[$count]=\"$file\""
    (( count++ ))
  done < "${temp_filelist}"
  rm "${temp_filelist}"
}

list_dirs() {
  count=0
  temp_dirlist=$(mktemp)
  find . -type d \
    ! -name "." \
    ! -name ".." \
    | sed 's/\.\/\(.*\)/\1/' \
    | sort -r > "${temp_dirlist}"
  while read dir; do
    eval "${1}[$count]=\"$dir\""
    (( count++ ))
  done < "${temp_dirlist}"
  rm "${temp_dirlist}"
}
