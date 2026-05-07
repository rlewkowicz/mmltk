# Copyright (c) 2009 The Chromium Embedded Framework Authors. All rights
# reserved. Use of this source code is governed by a BSD-style license that
# can be found in the LICENSE file.

from __future__ import absolute_import
from optparse import OptionParser
import os
import sys
from git_util import git_apply_patch_file

if __name__ != "__main__":
  sys.stdout.write('This file cannot be loaded as a module!')
  sys.exit()

cef_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
cef_patch_dir = os.path.join(cef_dir, 'patch')
src_dir = os.path.abspath(os.path.join(cef_dir, os.pardir))
mmltk_patch_root = os.environ.get('MMLTK_CEF_PATCH_ROOT', '').strip()
if len(mmltk_patch_root) > 0:
  mmltk_patch_root = os.path.abspath(mmltk_patch_root)
else:
  mmltk_patch_root = None


def write_note(type, note):
  separator = '-' * 79 + '\n'
  sys.stdout.write(separator)
  sys.stdout.write('!!!! %s: %s\n' % (type, note))
  sys.stdout.write(separator)
  sys.stdout.flush()


def get_patch_config_file():
  if mmltk_patch_root is not None:
    config_file = os.path.join(mmltk_patch_root, 'patch.cfg')
    if not os.path.isfile(config_file):
      raise Exception('Patch config file %s does not exist.' % config_file)
    return config_file

  config_file = os.path.join(cef_patch_dir, 'patch.cfg')
  if not os.path.isfile(config_file):
    raise Exception('Patch config file %s does not exist.' % config_file)
  return config_file


def get_patch_file_path(patch_name):
  candidates = []
  if mmltk_patch_root is not None:
    candidates.append(os.path.join(mmltk_patch_root, 'patches', patch_name))
  candidates.append(os.path.join(cef_patch_dir, 'patches', patch_name))

  for candidate in candidates:
    if os.path.isfile(candidate):
      return candidate

  raise Exception('Patch file %s does not exist in %s.' %
                  (patch_name, ', '.join(candidates)))


def apply_patch_file(patch_file, patch_dir):
  ''' Apply a specific patch file in optional patch directory. '''
  patch_name = patch_file + '.patch'
  patch_path = get_patch_file_path(patch_name)

  if patch_dir is None or len(patch_dir) == 0:
    patch_dir = src_dir
  else:
    if not os.path.isabs(patch_dir):
      patch_dir = os.path.join(src_dir, patch_dir)
    patch_dir = os.path.abspath(patch_dir)
    if not os.path.isdir(patch_dir):
      sys.stdout.write('\nApply %s in %s\n' % (patch_name, patch_dir))
      sys.stdout.write('... target directory does not exist (skipping).\n')
      return 'skip'

  result = git_apply_patch_file(patch_path, patch_dir)
  if result == 'fail':
    write_note('ERROR',
               'This patch failed to apply. Your build will not be correct.')
  return result


def apply_patch_config():
  ''' Apply patch files based on a configuration file. '''
  if os.environ.get('MMLTK_CEF_PATCHES_ALREADY_APPLIED', '') == '1':
    sys.stdout.write(
        'Skipping CEF patch stack: MMLTK_CEF_PATCHES_ALREADY_APPLIED=1 '
        '(stock_authoring_checkout has already prepared the source).\n')
    sys.stdout.flush()
    return

  config_file = get_patch_config_file()
  if mmltk_patch_root is not None:
    sys.stdout.write('Using MMLTK patch root %s\n' % mmltk_patch_root)
    sys.stdout.flush()

  scope = {}
  exec(compile(open(config_file, "rb").read(), config_file, 'exec'), scope)
  patches = scope["patches"]

  results = {'apply': [], 'skip': [], 'fail': []}

  for patch in patches:
    patch_file = patch['name']
    dopatch = True

    if 'condition' in patch:
      if patch['condition'] not in os.environ:
        sys.stdout.write('\nSkipping patch file %s\n' % patch_file)
        dopatch = False

    if dopatch:
      result = apply_patch_file(patch_file, patch['path']
                                if 'path' in patch else None)
      results[result].append(patch_file)

      if 'note' in patch:
        write_note('NOTE', patch['note'])
    else:
      results['skip'].append(patch_file)

  sys.stdout.write('\n%d patches total (%d applied, %d skipped, %d failed)\n' % \
      (len(patches), len(results['apply']), len(results['skip']), len(results['fail'])))
  sys.stdout.flush()

  if len(results['fail']) > 0:
    sys.stdout.write('\n')
    write_note('ERROR',
               '%d patches failed to apply. Your build will not be correct.' %
               len(results['fail']))
    sys.stdout.write('\nTo manually revert failed patches run:' \
                     '\n$ %s ./tools/patch_updater.py --revert --patch %s\n' %
                     (os.path.basename(sys.executable), ' --patch '.join(results['fail'])))
    sys.exit(1)


disc = """
This utility applies patch files.
"""

parser = OptionParser(description=disc)
parser.add_option(
    '--patch-file', dest='patchfile', metavar='FILE', help='patch source file')
parser.add_option(
    '--patch-dir',
    dest='patchdir',
    metavar='DIR',
    help='patch target directory')
(options, args) = parser.parse_args()

if not options.patchfile is None:
  result = apply_patch_file(options.patchfile, options.patchdir)
  if result == 'fail':
    sys.exit(1)
else:
  apply_patch_config()
