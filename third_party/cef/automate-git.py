# Copyright (c) 2014 The Chromium Embedded Framework Authors. All rights
# reserved. Use of this source code is governed by a BSD-style license that
# can be found in the LICENSE file.

from __future__ import absolute_import
from __future__ import print_function
import base64
from datetime import datetime
import hashlib
import importlib.util
from io import open
from optparse import OptionParser
import os
import queue
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import zipfile

if sys.version_info.major != 3:
  sys.stderr.write('Python3 is required!')
  sys.exit(1)

from urllib.request import urlopen

##
##

depot_tools_url = 'https://chromium.googlesource.com/chromium/tools/depot_tools.git'
depot_tools_archive_url = 'https://storage.googleapis.com/chrome-infra/depot_tools.zip'

cef_git_url = 'https://github.com/chromiumembedded/cef.git'

##
##

script_dir = os.path.dirname(__file__)

##
##

_COLOR_RED = '\033[31m'
_COLOR_BOLD = '\033[1m'
_COLOR_RESET = '\033[0m'


def _stderr_msg(msg):
  return _COLOR_RED + _COLOR_BOLD + msg + _COLOR_RESET


def _printer_thread(q):
  """ Serializes printing to stdout. """
  while True:
    message = q.get()

    if message is None:
      break
    print(message)

    q.task_done()


def _read_stdout_thread(process, name, q):
  """ Reads output from a process's stdout and prints it with a process identifier. """
  start_time = time.time()

  for line in iter(process.stdout.readline, b''):
    q.put(f"[{name}] {line.decode().strip()}")

  process.wait()

  elapsed_time = time.time() - start_time

  returncode = process.returncode
  if returncode == 0:
    q.put(f"[{name}] Exited with code 0")
  else:
    q.put(_stderr_msg(f"[{name}] ERROR Exited with code {returncode}"))

  q.put(f"[{name}] Execution time: {elapsed_time:.4f} seconds")


def _read_stderr_thread(process, name, q):
  """ Reads output from a process's stdout and prints it with a process identifier. """
  for line in iter(process.stderr.readline, b''):
    q.put(_stderr_msg(f"[{name}] {line.decode().strip()}"))


def run_parallel(commands):
  """ Run multiple commands (command_line, working_dir pairs) in parallel.
      Returns the number of commands that succeeded (returned exit code 0). """
  assert len(commands) > 1

  if options.dryrun:
    for i, command in enumerate(commands):
      print(f"[Process {i+1}] Running \"{command[0]}\" in \"{command[1]}\"")
    return len(commands)

  processes = []
  threads = []

  print_queue = queue.Queue()

  printer = threading.Thread(target=_printer_thread, args=(print_queue,))
  printer.daemon = True
  printer.start()

  for i, command in enumerate(commands):
    name = f"Process {i+1}"
    cmd = command[0]
    cwd = command[1]

    print(f"[{name}] Running \"{cmd}\" in \"{cwd}\"")

    args = shlex.split(cmd.replace('\\', '\\\\'))
    process = subprocess.Popen(
        args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    processes.append(process)

    thread = threading.Thread(
        target=_read_stdout_thread, args=(process, name, print_queue))
    thread.daemon = True
    thread.start()
    threads.append(thread)

    thread = threading.Thread(
        target=_read_stderr_thread, args=(process, name, print_queue))
    thread.daemon = True
    thread.start()
    threads.append(thread)

  for thread in threads:
    thread.join()

  print_queue.put(None)
  printer.join()

  return sum([process.returncode == 0 for process in processes])


##
##


def msg(message):
  """ Output a message. """
  sys.stdout.write('--> ' + message + "\n")


def run(command_line, working_dir, output_file=None):
  """ Runs the specified command. """
  sys.stdout.write('-------- Running "'+command_line+'" in "'+\
                   working_dir+'"...'+"\n")
  if not options.dryrun:
    args = shlex.split(command_line.replace('\\', '\\\\'))

    if not output_file:
      return subprocess.check_call(
          args, cwd=working_dir, shell=(sys.platform == 'win32'))
    try:
      msg('Writing %s' % output_file)
      with open(output_file, 'w', encoding='utf-8') as fp:
        return subprocess.check_call(
            args,
            cwd=working_dir,
            shell=(sys.platform == 'win32'),
            stderr=subprocess.STDOUT,
            stdout=fp)
    except subprocess.CalledProcessError:
      msg(_stderr_msg('ERROR Run failed. See %s for output.' % output_file))
      raise


def create_directory(path):
  """ Creates a directory if it doesn't already exist. """
  if not os.path.exists(path):
    msg("Creating directory %s" % (path))
    if not options.dryrun:
      os.makedirs(path)


def delete_directory(path):
  """ Removes an existing directory. """
  if os.path.exists(path):
    msg("Removing directory %s" % (path))
    if not options.dryrun:
      shutil.rmtree(path, onerror=onerror)


def copy_directory(source, target, allow_overwrite=False):
  """ Copies a directory from source to target. """
  if not options.dryrun and os.path.exists(target):
    if not allow_overwrite:
      raise Exception("Directory %s already exists" % (target))
    delete_directory(target)
  if os.path.exists(source):
    msg("Copying directory %s to %s" % (source, target))
    if not options.dryrun:
      shutil.copytree(source, target)


def move_directory(source, target, allow_overwrite=False):
  """ Copies a directory from source to target. """
  if not options.dryrun and os.path.exists(target):
    if not allow_overwrite:
      raise Exception("Directory %s already exists" % (target))
    delete_directory(target)
  if os.path.exists(source):
    msg("Moving directory %s to %s" % (source, target))
    if not options.dryrun:
      try:
        os.rename(source, target)
      except OSError:
        msg(
            _stderr_msg('ERROR Failed to move directory %s to %s' % (source,
                                                                     target)))
        raise


def copy_file(source, target, allow_overwrite=False):
  """ Copies a file from source to target. """
  if not os.path.exists(source):
    raise Exception("File %s does not exist" % (source))
  if not options.dryrun and os.path.exists(target):
    if not allow_overwrite:
      raise Exception("File %s already exists" % (target))
    os.remove(target)
  msg("Copying file %s to %s" % (source, target))
  if not options.dryrun:
    parent_dir = os.path.dirname(target)
    if len(parent_dir) > 0 and not os.path.isdir(parent_dir):
      os.makedirs(parent_dir)
    shutil.copy2(source, target)


def is_git_checkout(path):
  """ Returns true if the path represents a git checkout. """
  return os.path.exists(os.path.join(path, '.git'))


def exec_cmd(cmd, path):
  """ Execute the specified command and return the result. """
  out = ''
  err = ''
  sys.stdout.write("-------- Running \"%s\" in \"%s\"...\n" % (cmd, path))
  parts = cmd.split()
  try:
    process = subprocess.Popen(
        parts,
        cwd=path,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        shell=(sys.platform == 'win32'))
    out, err = process.communicate()
  except IOError as e:
    (errno, strerror) = e.args
    raise
  except:
    raise
  return {'out': out.decode('utf-8'), 'err': err.decode('utf-8')}


def get_git_hash(path, branch):
  """ Returns the git hash for the specified branch/tag/hash. """
  cmd = "%s rev-parse %s" % (git_exe, branch)
  result = exec_cmd(cmd, path)
  if result['out'] != '':
    return result['out'].strip()
  return 'Unknown'


def get_git_url(path):
  """ Returns the origin url for the specified path. """
  cmd = "%s config --get remote.origin.url" % (git_exe)
  result = exec_cmd(cmd, path)
  if result['out'] != '':
    return result['out'].strip()
  return 'Unknown'


def is_valid_sha256(hash_string):
  """ Returns true if |value| is a valid hex-encoded SHA256 hash. """
  if len(hash_string) != 64:
    return False
  if not re.fullmatch(r'[0-9a-fA-F]+', hash_string):
    return False
  return True


def sha256_of_file(file_path):
  """ Returns the lower-case hex-encoded SHA256 hash for |file_path| contents. """
  sha256_hash = hashlib.sha256()
  with open(file_path, "rb") as f:
    for chunk in iter(lambda: f.read(4096), b""):
      sha256_hash.update(chunk)
  return sha256_hash.hexdigest()


def download_and_extract(src, target, strip_components=0, sha256_hash=''):
  """ Extracts the contents of src, which may be a URL or local file, to the
      target directory. """
  temporary = False

  extension = None
  for ext in ('.zip', '.tar.bz2', '.tar.xz'):
    if src.lower().endswith(ext):
      extension = ext
      break
  if extension is None:
    raise Exception('Unsupported file extension for: ' + src)

  if src[:4] == 'http':
    msg('Downloading %s' % src)

    temporary = True
    handle, archive_path = tempfile.mkstemp(suffix=extension)
    with urlopen(src) as response:
      os.write(handle, response.read())
    os.close(handle)
  elif os.path.exists(src):
    archive_path = src
  else:
    raise Exception('Path type is unsupported or does not exist: ' + src)

  if len(sha256_hash) == 64:
    hash = sha256_of_file(archive_path)
    if hash != sha256_hash.lower():
      raise Exception('SHA256 hash check failed for: ' + archive_path)
    msg('SHA56 hash %s verified for %s' % (sha256_hash, archive_path))

  msg('Extracting ' + archive_path)

  if extension == '.zip':
    if not zipfile.is_zipfile(archive_path):
      raise Exception('Not a valid zip archive: ' + archive_path)

    try:
      os.makedirs(target)
      zf = zipfile.ZipFile(archive_path, 'r')
      zf.extractall(target)
    except:
      shutil.rmtree(target, onerror=onerror)
      raise
    zf.close()
  elif extension.startswith('.tar'):
    if sys.platform == 'win32' and sys.getwindowsversion().build < 22000:
      assert strip_components <= 1, strip_components

      def get_first_directory(path):
        """ Returns the full path of the first directory found within |path|. """
        for entry in os.scandir(path):
          if entry.is_dir():
            return os.path.join(path, entry.name)
        return None

      mode = ''
      if extension.endswith('.bz2'):
        mode = 'bz2'
      elif extension.endswith('.xz'):
        mode = 'xz'
      tmp_path = target + '.tmp'
      try:
        with tarfile.open(archive_path, 'r:' + mode) as tar:
          if strip_components == 0:
            os.makedirs(target)
            tar.extractall(path=target)
          elif strip_components == 1:
            tar.extractall(path=tmp_path)
            tmp_subdir_path = get_first_directory(tmp_path)
            if not tmp_subdir_path is None:
              shutil.move(tmp_subdir_path, target)
      except:
        shutil.rmtree(target, onerror=onerror)
        raise
      if os.path.isdir(tmp_path):
        shutil.rmtree(tmp_path, onerror=onerror)
    else:
      mode = ''
      if extension.endswith('.bz2'):
        mode = 'j'
      elif extension.endswith('.xz'):
        mode = 'J'
      # TODO: Add support for utilizing multiple cores.
      cmd = 'tar x%sf "%s" -C "%s"' % (mode, archive_path, target)
      if strip_components > 0:
        cmd += ' --strip-components=%d' % strip_components
      try:
        os.makedirs(target)
        run(cmd, target)
      except:
        shutil.rmtree(target, onerror=onerror)
        raise

  if temporary and os.path.exists(archive_path):
    os.remove(archive_path)


def read_file(path):
  """ Read a file. """
  if os.path.exists(path):
    with open(path, 'r', encoding='utf-8') as fp:
      return fp.read()
  else:
    raise Exception("Path does not exist: %s" % (path))


def write_file(path, data):
  """ Write a file. """
  msg('Writing %s' % path)
  if not options.dryrun:
    with open(path, 'w', encoding='utf-8') as fp:
      fp.write(data)


def read_config_file(path):
  """ Read a configuration file. """
  return eval(read_file(path), {'__builtins__': None}, None)


def write_config_file(path, contents):
  """ Write a configuration file. """
  data = "{\n"
  for key in sorted(contents.keys()):
    data += "  '%s': '%s',\n" % (key, contents[key])
  data += "}\n"
  write_file(path, data)


def read_branch_config_file(path):
  """ Read the CEF branch from the specified path. """
  config_file = os.path.join(path, 'cef.branch')
  if os.path.isfile(config_file):
    contents = read_config_file(config_file)
    if 'branch' in contents:
      return contents['branch']
  return ''


def write_branch_config_file(path, branch):
  """ Write the CEF branch to the specified path. """
  config_file = os.path.join(path, 'cef.branch')
  if not os.path.isfile(config_file):
    write_config_file(config_file, {'branch': branch})


def validate_mmltk_chromium_checkout_dir(path):
  """ Validate that the Chromium checkout directory can be safely reused. """
  if not os.path.exists(path):
    return

  if is_git_checkout(path):
    msg("Reusing existing Chromium checkout: %s" % path)
    return

  if os.path.isdir(path):
    entries = sorted(os.listdir(path))
    if len(entries) == 0:
      msg("Removing empty Chromium checkout directory %s" % path)
      delete_directory(path)
      return
    contents = ', '.join(entries[:10])
    if len(entries) > 10:
      contents += ', ...'
  else:
    contents = 'non-directory path'

  raise Exception(
      'Chromium checkout path %s exists but is not a valid git checkout. '
      'Refusing to bootstrap over a partial cache state. Contents: %s' %
      (path, contents))

_mmltk_make_cmake_modules = {}


def load_mmltk_make_cmake_module(src_root):
  """ Load the CEF make_cmake.py module for template rendering. """
  module_path = os.path.join(src_root, 'cef', 'tools', 'make_cmake.py')
  if module_path in _mmltk_make_cmake_modules:
    return _mmltk_make_cmake_modules[module_path]

  if not os.path.isfile(module_path):
    raise Exception('Missing CEF make_cmake.py at %s' % module_path)

  tools_dir = os.path.dirname(module_path)
  spec = importlib.util.spec_from_file_location('mmltk_make_cmake', module_path)
  module = importlib.util.module_from_spec(spec)
  old_sys_path = list(sys.path)
  sys.path.insert(0, tools_dir)
  try:
    spec.loader.exec_module(module)
  finally:
    sys.path[:] = old_sys_path

  _mmltk_make_cmake_modules[module_path] = module
  return module


def render_mmltk_cmake_template(src_root, input_path, output_path):
  """ Render a CEF CMake template for the packaged debug bundle. """
  module = load_mmltk_make_cmake_module(src_root)
  variables = {}
  variables.update(module.read_gypi_variables('cef_paths'))
  variables.update(module.read_gypi_variables('cef_paths2'))
  module.process_cmake_template(input_path, output_path, variables, quiet=True)


def copy_mmltk_required_path(source, target):
  """ Copy a required file or directory. """
  if not os.path.exists(source):
    raise Exception('Missing required build artifact: %s' % source)
  if os.path.isdir(source):
    copy_directory(source, target, allow_overwrite=True)
  else:
    copy_file(source, target, allow_overwrite=True)


def copy_mmltk_optional_path(source, target):
  """ Copy an optional file or directory if it exists. """
  if not os.path.exists(source):
    return
  if os.path.isdir(source):
    copy_directory(source, target, allow_overwrite=True)
  else:
    copy_file(source, target, allow_overwrite=True)


def generate_mmltk_debug_bundle(output_dir, archive_name):
  """ Package the Linux x64 Debug build into the repo-local archive layout. """
  if platform != 'linux':
    raise Exception('MMLTK packaging only supports Linux.')
  if not options.x64build:
    raise Exception('MMLTK packaging only supports Linux x64 builds.')

  src_root = chromium_src_dir
  build_dir = os.path.join(src_root, 'out', get_build_directory_name(True))
  generated_include_dir = os.path.join(build_dir, 'gen', 'cef', 'include')
  bundle_version = chromium_checkout
  if bundle_version.startswith('refs/tags/'):
    bundle_version = bundle_version[len('refs/tags/'):]
  bundle_name = 'cef_binary_%s_linux64_minimal_debug' % bundle_version

  if options.dryrun:
    msg('Dry-run: would package %s to %s/%s' %
        (bundle_name, output_dir, archive_name))
    return

  create_directory(output_dir)
  temp_root = tempfile.mkdtemp(prefix='mmltk-cef-bundle-', dir=download_dir)
  try:
    bundle_dir = os.path.join(temp_root, bundle_name)
    create_directory(bundle_dir)
    create_directory(os.path.join(bundle_dir, 'Debug'))
    create_directory(os.path.join(bundle_dir, 'Resources'))
    create_directory(os.path.join(bundle_dir, 'cmake'))

    copy_mmltk_required_path(os.path.join(build_dir, 'libcef.so'),
                             os.path.join(bundle_dir, 'Debug', 'libcef.so'))
    copy_mmltk_required_path(os.path.join(build_dir, 'snapshot_blob.bin'),
                             os.path.join(bundle_dir, 'Debug',
                                          'snapshot_blob.bin'))
    copy_mmltk_required_path(os.path.join(build_dir, 'v8_context_snapshot.bin'),
                             os.path.join(bundle_dir, 'Debug',
                                          'v8_context_snapshot.bin'))
    copy_mmltk_required_path(os.path.join(build_dir, 'libEGL.so'),
                             os.path.join(bundle_dir, 'Debug', 'libEGL.so'))
    copy_mmltk_required_path(os.path.join(build_dir, 'libGLESv2.so'),
                             os.path.join(bundle_dir, 'Debug', 'libGLESv2.so'))
    copy_mmltk_required_path(os.path.join(build_dir, 'resources.pak'),
                             os.path.join(bundle_dir, 'Resources',
                                          'resources.pak'))
    copy_mmltk_required_path(os.path.join(build_dir, 'chrome_100_percent.pak'),
                             os.path.join(bundle_dir, 'Resources',
                                          'chrome_100_percent.pak'))
    copy_mmltk_required_path(os.path.join(build_dir, 'chrome_200_percent.pak'),
                             os.path.join(bundle_dir, 'Resources',
                                          'chrome_200_percent.pak'))
    copy_mmltk_required_path(os.path.join(build_dir, 'icudtl.dat'),
                             os.path.join(bundle_dir, 'Resources', 'icudtl.dat'))
    copy_mmltk_required_path(os.path.join(build_dir, 'locales'),
                             os.path.join(bundle_dir, 'Resources', 'locales'))
    copy_mmltk_optional_path(os.path.join(build_dir, 'libvk_swiftshader.so'),
                             os.path.join(bundle_dir, 'Debug',
                                          'libvk_swiftshader.so'))
    copy_mmltk_optional_path(os.path.join(build_dir, 'vk_swiftshader_icd.json'),
                             os.path.join(bundle_dir, 'Debug',
                                          'vk_swiftshader_icd.json'))
    copy_mmltk_optional_path(os.path.join(build_dir, 'libvulkan.so.1'),
                             os.path.join(bundle_dir, 'Debug',
                                          'libvulkan.so.1'))
    if os.path.isfile(os.path.join(build_dir, 'chrome_sandbox')):
      copy_mmltk_required_path(os.path.join(build_dir, 'chrome_sandbox'),
                               os.path.join(bundle_dir, 'Debug',
                                            'chrome-sandbox'))
    elif os.path.isfile(os.path.join(build_dir, 'chrome-sandbox')):
      copy_mmltk_required_path(os.path.join(build_dir, 'chrome-sandbox'),
                               os.path.join(bundle_dir, 'Debug',
                                            'chrome-sandbox'))

    copy_mmltk_required_path(os.path.join(src_root, 'cef', 'include'),
                             os.path.join(bundle_dir, 'include'))
    copy_mmltk_required_path(os.path.join(src_root, 'cef', 'libcef_dll'),
                             os.path.join(bundle_dir, 'libcef_dll'))
    copy_mmltk_required_path(
        os.path.join(src_root, 'net', 'base', 'net_error_list.h'),
        os.path.join(bundle_dir, 'include', 'base', 'internal',
                     'cef_net_error_list.h'))

    for generated_header in (
        'cef_api_versions.h',
        'cef_color_ids.h',
        'cef_command_ids.h',
        'cef_config.h',
        'cef_pack_resources.h',
        'cef_pack_strings.h',
        'cef_version.h',
    ):
      copy_mmltk_required_path(
          os.path.join(generated_include_dir, generated_header),
          os.path.join(bundle_dir, 'include', generated_header))

    render_mmltk_cmake_template(
        src_root,
        os.path.join(src_root, 'cef', 'cmake', 'cef_macros.cmake.in'),
        os.path.join(bundle_dir, 'cmake', 'cef_macros.cmake'))
    render_mmltk_cmake_template(
        src_root,
        os.path.join(src_root, 'cef', 'cmake', 'cef_variables.cmake.in'),
        os.path.join(bundle_dir, 'cmake', 'cef_variables.cmake'))
    render_mmltk_cmake_template(
        src_root,
        os.path.join(src_root, 'cef', 'cmake', 'FindCEF.cmake.in'),
        os.path.join(bundle_dir, 'cmake', 'FindCEF.cmake'))
    render_mmltk_cmake_template(
        src_root,
        os.path.join(src_root, 'cef', 'libcef_dll', 'CMakeLists.txt.in'),
        os.path.join(bundle_dir, 'libcef_dll', 'CMakeLists.txt'))
    render_mmltk_cmake_template(
        src_root,
        os.path.join(src_root, 'cef', 'CMakeLists.txt.in'),
        os.path.join(bundle_dir, 'CMakeLists.txt'))

    archive_path = os.path.join(output_dir, archive_name)
    sha_path = archive_path + '.sha256'
    if os.path.exists(archive_path):
      os.remove(archive_path)
    if os.path.exists(sha_path):
      os.remove(sha_path)

    msg('Packaging MMLTK debug bundle to %s' % archive_path)
    with tarfile.open(archive_path, 'w:bz2') as tar:
      tar.add(bundle_dir, arcname=bundle_name)

    archive_hash = sha256_of_file(archive_path)
    write_file(sha_path, '%s  %s\n' % (archive_hash,
                                       os.path.basename(archive_path)))
  finally:
    if os.path.isdir(temp_root):
      shutil.rmtree(temp_root, onerror=onerror)


def bootstrap_mmltk_linux_base_packages():
  """ Install the minimum packages required to run automate-git in Ubuntu. """
  if not options.mmltkbootstraplinux:
    return
  if platform != 'linux':
    raise Exception('--mmltk-bootstrap-linux only supports Linux.')

  os.environ['DEBIAN_FRONTEND'] = 'noninteractive'
  run('apt-get update', download_dir)
  run(
      'apt-get install -y --no-install-recommends '
      'bzip2 ca-certificates file git lsb-release procps python3-pip '
      'software-properties-common sudo xz-utils', download_dir)
  run('add-apt-repository -y universe', download_dir)
  run('apt-get update', download_dir)


def bootstrap_mmltk_linux_build_deps():
  """ Install Chromium Linux build dependencies for the target checkout. """
  if not options.mmltkbootstraplinux:
    return
  if platform != 'linux':
    raise Exception('--mmltk-bootstrap-linux only supports Linux.')
  if not chromium_checkout.startswith('refs/tags/'):
    raise Exception(
        'MMLTK Linux bootstrap requires a Chromium tag checkout, found %s' %
        chromium_checkout)

  install_script_url = (
      'https://chromium.googlesource.com/chromium/src/+/%s/'
      'build/install-build-deps.py?format=TEXT' % chromium_checkout)
  install_script_path = os.path.join(tempfile.gettempdir(),
                                     'mmltk-install-build-deps.py')
  msg('Downloading %s' % install_script_url)
  if not options.dryrun:
    encoded = urlopen(install_script_url).read()
    with open(install_script_path, 'wb') as fp:
      fp.write(base64.b64decode(encoded))

  run('%s %s --no-prompt --no-arm --no-chromeos-fonts --no-nacl' %
      (python_exe, install_script_path), download_dir)
  run('apt-get install -y --no-install-recommends libx11-xcb-dev',
      download_dir)
  run('%s -m pip install dataclasses importlib_metadata' % python_exe,
      download_dir)


def read_version_file(path):
  """ Read and parse a version file (key=value pairs, one per line). """
  contents = read_file(path)
  if contents is None:
    return None
  result = {}
  lines = contents.split("\n")
  for line in lines:
    parts = line.split('=', 1)
    if len(parts) == 2:
      result[parts[0]] = parts[1]
  return result


def read_chrome_version_file(src_path):
  path = os.path.join(src_path, 'chrome', 'VERSION')
  parts = read_version_file(path)
  if parts is None:
    raise Exception('Failed to read %s' % path)
  return '%s.%s.%s.%s' % (parts['MAJOR'], parts['MINOR'], parts['BUILD'],
                          parts['PATCH'])


def apply_patch(name, patch_dir=None, required=False):
  patch_file = os.path.join(cef_dir, 'patch', 'patches', name)
  if patch_dir is None:
    patch_dir = chromium_src_dir
  patch_path = patch_file + ".patch"
  if os.path.exists(patch_path):
    patch_tool = os.path.join(cef_dir, 'tools', 'patcher.py')
    run('%s %s --patch-file "%s" --patch-dir "%s"' %
        (python_exe, patch_tool, patch_file, patch_dir), chromium_src_dir)
  elif required:
    raise Exception('Required patch file does not exist: ' + patch_path)


def apply_deps_patch():
  """ Patch the Chromium DEPS file before `gclient sync` if necessary. """
  deps_path = os.path.join(chromium_src_dir, deps_file)
  if os.path.isfile(deps_path):
    msg("Chromium DEPS file: %s" % (deps_path))
    apply_patch(deps_file)
  else:
    raise Exception("Path does not exist: %s" % (deps_path))


def apply_runhooks_patch():
  """ Patch the Chromium runhooks files before `gclient runhooks` if necessary. """
  apply_patch('runhooks')


def run_patch_updater(args='', output_file=None):
  """ Run the patch updater script. """
  tool = os.path.join(cef_src_dir, 'tools', 'patch_updater.py')
  if len(args) > 0:
    args = ' ' + args
  run('%s %s%s' % (python_exe, tool, args), cef_src_dir, output_file)


def onerror(func, path, exc_info):
  """
  Error handler for ``shutil.rmtree``.

  If the error is due to an access error (read only file)
  it attempts to add write permission and then retries.

  If the error is for another reason it re-raises the error.

  Usage : ``shutil.rmtree(path, onerror=onerror)``
  """
  import stat
  if not os.access(path, os.W_OK):
    os.chmod(path, stat.S_IWUSR)
    func(path)
  else:
    raise


def get_chromium_main_position(commit):
  """ Returns the closest main position for the specified Chromium commit. """
  cmd = "%s log -2 %s" % (git_exe, commit)
  result = exec_cmd(cmd, chromium_src_dir)
  if result['out'] != '':
    match = re.search(r'refs/heads/(?:master|main)@{#([\d]+)}', result['out'])
    assert match != None, 'Failed to find position'
    return int(match.groups()[0])
  return None


def get_chromium_main_commit(position):
  """ Returns the main commit for the specified Chromium commit position. """
  cmd = '%s log -1 --grep=refs/heads/master@{#%s} --grep=refs/heads/main@{#%s} origin/main' % (
      git_exe, str(position), str(position))
  result = exec_cmd(cmd, chromium_src_dir)
  if result['out'] != '':
    match = re.search(r'^commit ([a-f0-9]+)', result['out'])
    assert match != None, 'Failed to find commit'
    return match.groups()[0]
  return None


def get_build_compat_versions():
  """ Returns the compatible Chromium and (optionally) depot_tools versions
      specified by the CEF checkout. """
  compat_path = os.path.join(cef_dir, 'CHROMIUM_BUILD_COMPATIBILITY.txt')
  msg("Reading %s" % compat_path)
  config = read_config_file(compat_path)

  if not 'chromium_checkout' in config:
    raise Exception("Missing chromium_checkout value in %s" % (compat_path))
  return config


def get_build_directory_name(is_debug):
  build_dir = ('Debug' if is_debug else 'Release') + '_'

  if options.x64build:
    build_dir += 'GN_x64'
  elif options.armbuild:
    build_dir += 'GN_arm'
  elif options.arm64build:
    build_dir += 'GN_arm64'
  else:
    build_dir += 'GN_x86'
  return build_dir


def read_update_file():
  update_path = os.path.join(cef_src_dir, 'CHROMIUM_UPDATE.txt')
  if not os.path.exists(update_path):
    msg("Missing file: %s" % update_path)
    return None

  msg("Reading %s" % update_path)
  return read_config_file(update_path)


def log_chromium_changes():
  """ Evaluate the Chromium checkout for changes. """
  config = read_update_file()
  if config is None:
    msg("Skipping Chromium changes log.")
    return

  if 'files' in config:
    out_file = os.path.join(download_dir, 'chromium_update_changes.diff')
    if os.path.exists(out_file):
      os.remove(out_file)

    old_commit = get_chromium_main_commit(
        get_chromium_main_position(chromium_compat_version))
    new_commit = get_chromium_main_commit(
        get_chromium_main_position(chromium_checkout))

    cmd = '%s diff --relative --no-prefix %s..%s -- %s' % (
        git_exe, old_commit, new_commit, ' '.join(config['files']))
    result = exec_cmd(cmd, chromium_src_dir)
    if result['out'] != '':
      write_file(out_file, result['out'])


def check_pattern_matches(output_file=None):
  """ Evaluate the Chromium checkout for pattern matches. """
  config = read_update_file()
  if config is None:
    msg("Skipping Chromium pattern matching.")
    return

  if 'patterns' in config:
    if output_file is None:
      fp = sys.stdout
    else:
      msg('Writing %s' % output_file)
      fp = open(output_file, 'w', encoding='utf-8')

    has_output = False
    for entry in config['patterns']:
      msg("Evaluating pattern: %s" % entry['pattern'])

      pattern_handle, pattern_file = tempfile.mkstemp()
      os.write(pattern_handle, entry['pattern'])
      os.close(pattern_handle)

      cmd = '%s grep -n -f %s' % (git_exe, pattern_file)
      result = exec_cmd(cmd, chromium_src_dir)
      os.remove(pattern_file)

      if result['out'] != '':
        write_msg = True
        re_exclude = re.compile(
            entry['exclude_matches']) if 'exclude_matches' in entry else None

        for line in result['out'].split('\n'):
          line = line.strip()
          if len(line) == 0:
            continue
          skip = not re_exclude is None and re_exclude.match(line) != None
          if not skip:
            if write_msg:
              if has_output:
                fp.write('\n')
              fp.write('!!!! WARNING: FOUND PATTERN: %s\n' % entry['pattern'])
              if 'message' in entry:
                fp.write(entry['message'] + '\n')
              fp.write('\n')
              write_msg = False
            fp.write(line + '\n')
            has_output = True

    if not output_file is None:
      if has_output:
        msg(_stderr_msg('ERROR Matches found. See %s for output.' % out_file))
      else:
        fp.write('Good news! No matches.\n')
      fp.close()

    if has_output:
      sys.exit(1)


def invalid_options_combination(a, b):
  print("Invalid combination of options: '%s' and '%s'" % (a, b))
  parser.print_help(sys.stderr)
  sys.exit(1)


##
##

if __name__ != "__main__":
  sys.stderr.write('This file cannot be loaded as a module!')
  sys.exit(1)

disc = """
This utility implements automation for the download, update, build and
distribution of CEF.
"""

parser = OptionParser(description=disc)

parser.add_option(
    '--download-dir',
    dest='downloaddir',
    metavar='DIR',
    help='Download directory with no spaces [required].')
parser.add_option(
    '--depot-tools-dir',
    dest='depottoolsdir',
    metavar='DIR',
    help='Download directory for depot_tools.',
    default='')
parser.add_option(
    '--depot-tools-archive',
    dest='depottoolsarchive',
    help='Archive file that contains a single top-level depot_tools directory.',
    default='')
parser.add_option(
    '--depot-tools-archive-sha256',
    dest='depottoolsarchivesha256',
    help='SHA256 hex-encoded hash for the file passed to --depot-tools-archive.',
    default='')
parser.add_option('--branch', dest='branch',
                  help='Branch of CEF to build (master, 3987, ...). This '+\
                       'will be used to name the CEF download directory and '+\
                       'to identify the correct URL if --url is not '+\
                       'specified. The default value is master.',
                  default='master')
parser.add_option('--url', dest='url',
                  help='CEF download URL. If not specified the default URL '+\
                       'will be used.',
                  default='')
parser.add_option('--chromium-url', dest='chromiumurl',
                  help='Chromium download URL. If not specified the default '+\
                       'URL will be used.',
                  default='')
parser.add_option('--checkout', dest='checkout',
                  help='Version of CEF to checkout. If not specified the '+\
                       'most recent remote version of the branch will be used.',
                  default='')
parser.add_option('--chromium-checkout', dest='chromiumcheckout',
                  help='Version of Chromium to checkout (Git '+\
                       'branch/hash/tag). This overrides the value specified '+\
                       'by CEF in CHROMIUM_BUILD_COMPATIBILITY.txt.',
                  default='')
parser.add_option(
    '--no-chromium-history',
    action='store_true',
    dest='nochromiumhistory',
    default=False,
    help='Checkout Chromium without history.')
parser.add_option(
    '--chromium-archive',
    dest='chromiumarchive',
    help='Archive file that contains a single top-level chromium src directory.',
    default='')
parser.add_option(
    '--chromium-archive-sha256',
    dest='chromiumarchivesha256',
    help='SHA256 hex-encoded hash for the file passed to --chromium-archive.',
    default='')

parser.add_option(
    '--force-config',
    action='store_true',
    dest='forceconfig',
    default=False,
    help='Force creation of a new gclient config file.')
parser.add_option('--force-clean',
                  action='store_true', dest='forceclean', default=False,
                  help='Force a clean checkout of Chromium and CEF. This will'+\
                       ' trigger a new update, build and distribution.')
parser.add_option('--force-clean-deps',
                  action='store_true', dest='forcecleandeps', default=False,
                  help='Force a clean checkout of Chromium dependencies. Used'+\
                       ' in combination with --force-clean.')
parser.add_option(
    '--dry-run',
    action='store_true',
    dest='dryrun',
    default=False,
    help="Output commands without executing them.")
parser.add_option('--dry-run-platform', dest='dryrunplatform', default=None,
                  help='Simulate a dry run on the specified platform '+\
                       '(windows, mac, linux). Must be used in combination'+\
                       ' with the --dry-run flag.')

parser.add_option('--force-update',
                  action='store_true', dest='forceupdate', default=False,
                  help='Force a Chromium and CEF update. This will trigger a '+\
                       'new build and distribution.')
parser.add_option('--no-update',
                  action='store_true', dest='noupdate', default=False,
                  help='Do not update Chromium or CEF. Pass --force-build or '+\
                       '--force-distrib if you desire a new build or '+\
                       'distribution.')
parser.add_option('--no-cef-update',
                  action='store_true', dest='nocefupdate', default=False,
                  help='Do not update CEF. Pass --force-build or '+\
                       '--force-distrib if you desire a new build or '+\
                       'distribution.')
parser.add_option('--force-cef-update',
                  action='store_true', dest='forcecefupdate', default=False,
                  help='Force a CEF update. This will cause local changes in '+\
                       'the CEF checkout to be discarded and patch files to '+\
                       'be reapplied.')
parser.add_option(
    '--no-chromium-update',
    action='store_true',
    dest='nochromiumupdate',
    default=False,
    help='Do not update Chromium.')
parser.add_option(
    '--no-depot-tools-update',
    action='store_true',
    dest='nodepottoolsupdate',
    default=False,
    help='Do not update depot_tools.')
parser.add_option('--fast-update',
                  action='store_true', dest='fastupdate', default=False,
                  help='Update existing Chromium/CEF checkouts for fast incremental '+\
                       'builds by attempting to minimize the number of modified files. '+\
                       'The update will fail if there are unstaged CEF changes or if '+\
                       'Chromium changes are not included in a patch file.')
parser.add_option(
    '--force-patch-update',
    action='store_true',
    dest='forcepatchupdate',
    default=False,
    help='Force update of patch files.')
parser.add_option(
    '--resave',
    action='store_true',
    dest='resave',
    default=False,
    help='Resave patch files.')
parser.add_option(
    '--log-chromium-changes',
    action='store_true',
    dest='logchromiumchanges',
    default=False,
    help='Create a log of the Chromium changes.')

parser.add_option('--force-build',
                  action='store_true', dest='forcebuild', default=False,
                  help='Force CEF debug and release builds. This builds '+\
                       '[build-target] on all platforms and chrome_sandbox '+\
                       'on Linux.')
parser.add_option(
    '--no-build',
    action='store_true',
    dest='nobuild',
    default=False,
    help='Do not build CEF.')
parser.add_option(
    '--build-target',
    dest='buildtarget',
    default='cefclient',
    help='Target name(s) to build (defaults to "cefclient").')
parser.add_option(
    '--build-tests',
    action='store_true',
    dest='buildtests',
    default=False,
    help='Also build the test target specified via --test-target.')
parser.add_option(
    '--no-debug-build',
    action='store_true',
    dest='nodebugbuild',
    default=False,
    help="Don't perform the CEF debug build.")
parser.add_option(
    '--no-release-build',
    action='store_true',
    dest='noreleasebuild',
    default=False,
    help="Don't perform the CEF release build.")
parser.add_option(
    '--verbose-build',
    action='store_true',
    dest='verbosebuild',
    default=False,
    help='Show all command lines while building.')
parser.add_option(
    '--build-failure-limit',
    dest='buildfailurelimit',
    default=1,
    type="int",
    help='Keep going until N jobs fail.')
parser.add_option('--build-log-file',
                  action='store_true', dest='buildlogfile', default=False,
                  help='Write build logs to file. The file will be named '+\
                       '"build-[branch]-[debug|release].log" in the download '+\
                       'directory.')
parser.add_option(
    '--x64-build',
    action='store_true',
    dest='x64build',
    default=False,
    help='Create a 64-bit build.')
parser.add_option(
    '--arm-build',
    action='store_true',
    dest='armbuild',
    default=False,
    help='Create an ARM build.')
parser.add_option(
    '--arm64-build',
    action='store_true',
    dest='arm64build',
    default=False,
    help='Create an ARM64 build.')
parser.add_option(
    '--with-pgo-profiles',
    action='store_true',
    dest='withpgoprofiles',
    default=False,
    help='Download PGO profiles for the build.')

parser.add_option(
    '--run-tests',
    action='store_true',
    dest='runtests',
    default=False,
    help='Run the ceftests target.')
parser.add_option(
    '--no-debug-tests',
    action='store_true',
    dest='nodebugtests',
    default=False,
    help="Don't run debug build tests.")
parser.add_option(
    '--no-release-tests',
    action='store_true',
    dest='noreleasetests',
    default=False,
    help="Don't run release build tests.")
parser.add_option(
    '--test-target',
    dest='testtarget',
    default='ceftests',
    help='Test target name to build (defaults to "ceftests").')
parser.add_option(
    '--test-prefix',
    dest='testprefix',
    default='',
    help='Prefix for running the test executable (e.g. `xvfb-run` on Linux).')
parser.add_option(
    '--test-args',
    dest='testargs',
    default='',
    help='Arguments that will be passed to the test executable.')

parser.add_option(
    '--force-distrib',
    action='store_true',
    dest='forcedistrib',
    default=False,
    help='Force creation of a CEF binary distribution.')
parser.add_option(
    '--no-distrib',
    action='store_true',
    dest='nodistrib',
    default=False,
    help="Don't create a CEF binary distribution.")
parser.add_option(
    '--minimal-distrib',
    action='store_true',
    dest='minimaldistrib',
    default=False,
    help='Create a minimal CEF binary distribution.')
parser.add_option(
    '--minimal-distrib-only',
    action='store_true',
    dest='minimaldistribonly',
    default=False,
    help='Create a minimal CEF binary distribution only.')
parser.add_option(
    '--client-distrib',
    action='store_true',
    dest='clientdistrib',
    default=False,
    help='Create a client CEF binary distribution.')
parser.add_option(
    '--client-distrib-only',
    action='store_true',
    dest='clientdistribonly',
    default=False,
    help='Create a client CEF binary distribution only.')
parser.add_option(
    '--sandbox-distrib',
    action='store_true',
    dest='sandboxdistrib',
    default=False,
    help='Create a sandbox distribution.')
parser.add_option(
    '--sandbox-distrib-only',
    action='store_true',
    dest='sandboxdistribonly',
    default=False,
    help='Create a sandbox distribution only.')
parser.add_option(
    '--tools-distrib',
    action='store_true',
    dest='toolsdistrib',
    default=False,
    help='Create a tools distribution.')
parser.add_option(
    '--tools-distrib-only',
    action='store_true',
    dest='toolsdistribonly',
    default=False,
    help='Create a tools distribution only.')
parser.add_option(
    '--no-distrib-symbols',
    action='store_true',
    dest='nodistribsymbols',
    default=False,
    help="Don't create CEF symbol output directories.")
parser.add_option(
    '--no-distrib-docs',
    action='store_true',
    dest='nodistribdocs',
    default=False,
    help="Don't create CEF documentation.")
parser.add_option(
    '--no-distrib-archive',
    action='store_true',
    dest='nodistribarchive',
    default=False,
    help="Don't create archives for output directories.")
parser.add_option(
    '--clean-artifacts',
    action='store_true',
    dest='cleanartifacts',
    default=False,
    help='Clean the artifacts output directory.')
parser.add_option(
    '--distrib-subdir',
    dest='distribsubdir',
    default='',
    help='CEF distrib dir name, child of chromium/src/cef/binary_distrib')
parser.add_option(
    '--distrib-subdir-suffix',
    dest='distribsubdirsuffix',
    default='',
    help='CEF distrib dir name suffix, child of chromium/src/cef/binary_distrib'
)
parser.add_option(
    '--mmltk-bootstrap-linux',
    action='store_true',
    dest='mmltkbootstraplinux',
    default=False,
    help='Install the Linux host packages required for automate-git.py.')
parser.add_option(
    '--mmltk-patcher-source',
    dest='mmltkpatchersource',
    default='',
    help='Copy this patched patcher.py over chromium/src/cef/tools/patcher.py '
         'before the build hook runs.')
parser.add_option(
    '--mmltk-build-jobs',
    dest='mmltkbuildjobs',
    default='',
    help='Explicit -j value passed to autoninja for MMLTK builds.')
parser.add_option(
    '--mmltk-output-dir',
    dest='mmltkoutputdir',
    default='',
    help='Write the repo-local Linux debug archive and checksum to this '
         'directory after building.')
parser.add_option(
    '--mmltk-output-archive-name',
    dest='mmltkoutputarchivename',
    default='mmltk_cef_linux64_minimal_debug.tar.bz2',
    help='Archive file name written under --mmltk-output-dir.')

(options, args) = parser.parse_args()

if options.downloaddir is None:
  print("The --download-dir option is required.")
  parser.print_help(sys.stderr)
  sys.exit(1)

if options.noupdate:
  options.nocefupdate = True
  options.nochromiumupdate = True
  options.nodepottoolsupdate = True

if options.chromiumarchive != '':
  options.nochromiumhistory = True

if options.depottoolsarchivesha256 != '' and \
   not is_valid_sha256(options.depottoolsarchivesha256):
  print('Invalid --depot-tools-archive-sha256 value.')
  parser.print_help(sys.stderr)
  sys.exit(1)

if options.chromiumarchivesha256 != '' and \
   not is_valid_sha256(options.chromiumarchivesha256):
  print('Invalid --chromium-archive-sha256 value.')
  parser.print_help(sys.stderr)
  sys.exit(1)

if options.mmltkbuildjobs != '' and not options.mmltkbuildjobs.isdigit():
  print('Invalid --mmltk-build-jobs value: %s' % options.mmltkbuildjobs)
  parser.print_help(sys.stderr)
  sys.exit(1)

if options.mmltkoutputdir == '' and \
    options.mmltkoutputarchivename != 'mmltk_cef_linux64_minimal_debug.tar.bz2':
  print('--mmltk-output-archive-name requires --mmltk-output-dir.')
  parser.print_help(sys.stderr)
  sys.exit(1)

if options.runtests:
  options.buildtests = True

if (options.nochromiumupdate and options.forceupdate):
  invalid_options_combination('--no-chromium-update', '--force-update')
if (options.nocefupdate and options.forceupdate):
  invalid_options_combination('--no-cef-update', '--force-update')
if (options.nobuild and options.forcebuild):
  invalid_options_combination('--no-build', '--force-build')
if (options.nodistrib and options.forcedistrib):
  invalid_options_combination('--no-distrib', '--force-distrib')
if (options.forceclean and options.fastupdate):
  invalid_options_combination('--force-clean', '--fast-update')
if (options.forcecleandeps and options.fastupdate):
  invalid_options_combination('--force-clean-deps', '--fast-update')
if (options.nochromiumhistory and options.logchromiumchanges):
  invalid_options_combination('--no-chromium-history', '--log-chromium-changes')

if (options.noreleasebuild and \
     (options.minimaldistrib or options.minimaldistribonly or \
      options.clientdistrib or options.clientdistribonly)) or \
   (options.minimaldistribonly + options.clientdistribonly + options.sandboxdistribonly + options.toolsdistribonly > 1):
  print('Invalid combination of options.')
  parser.print_help(sys.stderr)
  sys.exit(1)

if options.x64build + options.armbuild + options.arm64build > 1:
  print('Invalid combination of options.')
  parser.print_help(sys.stderr)
  sys.exit(1)

if (options.buildtests or options.runtests) and len(options.testtarget) == 0:
  print("A test target must be specified via --test-target.")
  parser.print_help(sys.stderr)
  sys.exit(1)

if options.dryrun and options.dryrunplatform is not None:
  platform = options.dryrunplatform
  if not platform in ['windows', 'mac', 'linux']:
    print('Invalid dry-run-platform value: %s' % (platform))
    sys.exit(1)
elif sys.platform == 'win32':
  platform = 'windows'
elif sys.platform == 'darwin':
  platform = 'mac'
elif sys.platform.startswith('linux'):
  platform = 'linux'
else:
  print('Unknown operating system platform')
  sys.exit(1)

if options.clientdistrib or options.clientdistribonly:
  if platform == 'linux' or (platform == 'windows' and options.arm64build):
    client_app = 'cefsimple'
  else:
    client_app = 'cefclient'
  if options.buildtarget.find(client_app) == -1:
    print('A client distribution cannot be generated if --build-target ' +
          'excludes %s.' % client_app)
    parser.print_help(sys.stderr)
    sys.exit(1)

cef_branch = options.branch

branch_is_master = (cef_branch == 'master' or cef_branch == 'trunk')
if not branch_is_master:
  if not cef_branch.isdigit():
    print('Invalid branch value: %s' % cef_branch)
    sys.exit(1)

  if int(cef_branch) < 5060:
    print('The requested branch (%s) is too old to build using this tool. ' +
          'The minimum supported branch is 5060.' % cef_branch)
    sys.exit(1)

branch_is_7151_or_older = not branch_is_master and int(cef_branch) <= 7151

if options.armbuild:
  if platform != 'linux':
    print('The ARM build option is only supported on Linux.')
    sys.exit(1)

deps_file = 'DEPS'

if platform == 'mac' and not (options.x64build or options.arm64build):
  print('32-bit MacOS builds are not supported. ' +
        'Add --x64-build or --arm64-build flag to generate a 64-bit build.')
  sys.exit(1)

if options.mmltkbootstraplinux and platform != 'linux':
  print('--mmltk-bootstrap-linux only supports Linux.')
  sys.exit(1)

if options.mmltkoutputdir != '' and platform != 'linux':
  print('--mmltk-output-dir only supports Linux.')
  sys.exit(1)

sandbox_static_platforms = []

sandbox_shared_platforms = []

bootstrap_exe_platforms = []

if branch_is_7151_or_older:
  sandbox_static_platforms.extend(['windows', 'mac'])
else:
  bootstrap_exe_platforms.append('windows')
  sandbox_shared_platforms.append('mac')

if not platform in sandbox_static_platforms and \
   not platform in sandbox_shared_platforms and \
   not platform in bootstrap_exe_platforms and \
   (options.sandboxdistrib or options.sandboxdistribonly):
  print('The sandbox distribution is not supported on this platform.')
  sys.exit(1)

force_change = options.forceclean or options.forceupdate

discard_local_changes = force_change or options.forcecefupdate

if options.resave and (options.forcepatchupdate or discard_local_changes):
  print('--resave cannot be combined with options that modify or discard ' +
        'patches.')
  parser.print_help(sys.stderr)
  sys.exit(1)

if platform == 'windows':
  os.environ['DEPOT_TOOLS_WIN_TOOLCHAIN'] = '0'

download_dir = os.path.abspath(options.downloaddir)
chromium_dir = os.path.join(download_dir, 'chromium')
chromium_src_dir = os.path.join(chromium_dir, 'src')
out_src_dir = os.path.join(chromium_src_dir, 'out')
cef_src_dir = os.path.join(chromium_src_dir, 'cef')

if options.fastupdate and os.path.exists(cef_src_dir):
  cef_dir = cef_src_dir
else:
  cef_dir = os.path.join(download_dir, 'cef')

##
##

create_directory(download_dir)

msg("Download Directory: %s" % (download_dir))

bootstrap_mmltk_linux_base_packages()

##
##

if options.depottoolsdir != '':
  depot_tools_dir = os.path.abspath(options.depottoolsdir)
else:
  depot_tools_dir = os.path.join(download_dir, 'depot_tools')

msg("Depot Tools Directory: %s" % (depot_tools_dir))

if not os.path.exists(depot_tools_dir):
  if platform == 'windows' and options.depottoolsarchive == '':
    options.depottoolsarchive = depot_tools_archive_url

  if options.depottoolsarchive != '':
    msg('Extracting %s to %s' % (options.depottoolsarchive, depot_tools_dir))
    if not options.dryrun:
      download_and_extract(
          options.depottoolsarchive,
          depot_tools_dir,
          sha256_hash=options.depottoolsarchivesha256)
  else:
    run('git clone ' + depot_tools_url + ' ' + depot_tools_dir, download_dir)

assert os.path.isdir(depot_tools_dir), depot_tools_dir
os.environ['PATH'] = depot_tools_dir + os.pathsep + os.environ['PATH']

if not options.nodepottoolsupdate:
  msg('Updating depot_tools')
  if platform == 'windows':
    run('update_depot_tools.bat', depot_tools_dir)
  else:
    run('update_depot_tools', depot_tools_dir)

if platform == 'windows':
  git_exe = 'git.exe'
  python_bat = 'python3.bat'
  python_exe = os.path.join(depot_tools_dir, python_bat)
  if options.dryrun and not os.path.exists(python_exe):
    sys.stdout.write("WARNING: --dry-run assumes that depot_tools" \
                     " is already in your PATH. If it isn't\nplease" \
                     " specify a --depot-tools-dir value.\n")
    python_exe = python_bat
else:
  git_exe = 'git'
  python_exe = sys.executable

##
##

if options.forceclean and os.path.exists(cef_dir):
  delete_directory(cef_dir)

if os.path.exists(cef_dir) and not is_git_checkout(cef_dir):
  raise Exception("Not a valid CEF Git checkout: %s" % (cef_dir))

cef_url = options.url.strip()
if cef_url == '':
  cef_url = cef_git_url

if not options.nocefupdate and os.path.exists(cef_dir):
  cef_existing_url = get_git_url(cef_dir)
  if cef_url != cef_existing_url:
    raise Exception(
        'Requested CEF checkout URL %s does not match existing URL %s' %
        (cef_url, cef_existing_url))

msg("CEF Branch: %s" % (cef_branch))
msg("CEF URL: %s" % (cef_url))
msg("CEF Source Directory: %s" % (cef_dir))

if options.checkout == '':
  if branch_is_master:
    cef_checkout = 'origin/master'
  else:
    cef_checkout = 'origin/' + cef_branch
else:
  cef_checkout = options.checkout

if not options.nocefupdate and not os.path.exists(cef_dir):
  cef_checkout_new = True
  run('%s clone %s %s' % (git_exe, cef_url, cef_dir), download_dir)
else:
  cef_checkout_new = False

if not options.nocefupdate and os.path.exists(cef_dir):
  cef_current_hash = get_git_hash(cef_dir, 'HEAD')

  if not cef_checkout_new:
    run('%s fetch' % (git_exe), cef_dir)

  cef_desired_hash = get_git_hash(cef_dir, cef_checkout)
  cef_checkout_changed = cef_checkout_new or force_change or \
                         options.forcecefupdate or \
                         cef_current_hash != cef_desired_hash

  msg("CEF Current Checkout: %s" % (cef_current_hash))
  msg("CEF Desired Checkout: %s (%s)" % (cef_desired_hash, cef_checkout))

  if cef_checkout_changed:
    if cef_dir == cef_src_dir:
      run_patch_updater("--backup --revert")

    run('%s checkout %s%s' % (git_exe, '--force '
                              if discard_local_changes else '', cef_checkout),
        cef_dir)
else:
  cef_checkout_changed = False

build_compat_versions = get_build_compat_versions()

chromium_compat_version = build_compat_versions['chromium_checkout']
if len(options.chromiumcheckout) > 0:
  chromium_checkout = options.chromiumcheckout
else:
  chromium_checkout = chromium_compat_version

bootstrap_mmltk_linux_build_deps()

if not options.nodepottoolsupdate and \
    'depot_tools_checkout' in build_compat_versions:
  depot_tools_compat_version = build_compat_versions['depot_tools_checkout']
  run('%s checkout %s%s' % (git_exe, '--force '
                            if discard_local_changes else '',
                            depot_tools_compat_version), depot_tools_dir)

os.environ['DEPOT_TOOLS_UPDATE'] = '0'

##
##

out_dir = os.path.join(download_dir, 'out_' + cef_branch)

if options.forceclean and os.path.exists(out_dir):
  delete_directory(out_dir)

msg("CEF Output Directory: %s" % (out_dir))

##
##

create_directory(chromium_dir)
validate_mmltk_chromium_checkout_dir(chromium_src_dir)

gclient_file = os.path.join(chromium_dir, '.gclient')
force_config = not os.path.exists(gclient_file) or options.forceconfig

chromium_version = chromium_checkout.split('/')[2]

if options.nochromiumhistory and os.path.exists(chromium_src_dir):
  current_version = read_chrome_version_file(chromium_src_dir)
  if current_version != chromium_version:
    error = ''
    if options.nochromiumupdate:
      error += ' Remove --no-chromium-update.'
    if not force_change:
      error += ' Add --force-clean or --force-update.'
    if len(error) > 0:
      raise Exception(
          'Current Chromium checkout with --no-chromium-history is incorrect.' +
          error)

    delete_directory(chromium_src_dir)
    force_config = True

if options.chromiumurl != '':
  chromium_url = options.chromiumurl
else:
  chromium_url = 'https://chromium.googlesource.com/chromium/src.git'

if options.nochromiumhistory:
  chromium_url += '@' + chromium_version

if force_config:
  gclient_spec = (
      "solutions = [{"+
        "'managed': False, "+
        "'name': 'src', "+
        "'url': '" + chromium_url + "', "+
        "'custom_vars': {"+
          "'checkout_pgo_profiles': " + ('True' if options.withpgoprofiles else 'False') + ", "+
          "'source_tarball': " + ('True' if options.chromiumarchive != '' else 'False') + ", "+
          "'siso_version': 'latest', "+
        "}, "+
        "'custom_deps': {}, "+
        "'deps_file': '" + deps_file + "', "+
        "'safesync_url': ''"+
      "}]"
  )

  msg('Writing %s' % gclient_file)
  if not options.dryrun:
    with open(gclient_file, 'w', encoding='utf-8') as fp:
      fp.write(gclient_spec)

if not options.nochromiumupdate and not os.path.exists(chromium_src_dir):
  chromium_checkout_new = True
  sync_args = '--nohooks '

  if options.chromiumarchive != '':
    msg('Extracting %s to %s' % (options.chromiumarchive, chromium_src_dir))
    if not options.dryrun:
      download_and_extract(options.chromiumarchive, chromium_src_dir, strip_components=1,
                           sha256_hash=options.chromiumarchivesha256)

    apply_patch('tarball_deps', chromium_src_dir, required=True)
    apply_patch('tarball_gclient', depot_tools_dir, required=True)
  elif options.nochromiumhistory:
    sync_args += '--no-history'
  else:
    sync_args += '--with_branch_heads'

  run("gclient sync %s" % sync_args, chromium_dir)
else:
  chromium_checkout_new = False

if options.chromiumarchive:
  current_version = read_chrome_version_file(chromium_src_dir)
  if current_version != chromium_version:
    raise Exception('Found Chromium version %s, expected %s at: %s' %
                    (current_version, chromium_version, chromium_src_dir))
else:
  if not options.dryrun and not is_git_checkout(chromium_src_dir):
    raise Exception('Not a valid git checkout: %s' % (chromium_src_dir))

  if os.path.exists(chromium_src_dir):
    msg("Chromium URL: %s" % (get_git_url(chromium_src_dir)))

if options.nochromiumhistory:
  chromium_checkout_changed = chromium_checkout_new
  msg("Chromium Checkout (no history): %s" % chromium_checkout)
else:
  if not options.nochromiumupdate and os.path.exists(chromium_src_dir):
    run("%s fetch" % (git_exe), chromium_src_dir)
    run("%s fetch --tags" % (git_exe), chromium_src_dir)

  if not options.nochromiumupdate and os.path.exists(chromium_src_dir):
    chromium_current_hash = get_git_hash(chromium_src_dir, 'HEAD')
    chromium_desired_hash = get_git_hash(chromium_src_dir, chromium_checkout)
    chromium_checkout_changed = chromium_checkout_new or force_change or \
                                chromium_current_hash != chromium_desired_hash

    msg("Chromium Current Checkout: %s" % (chromium_current_hash))
    msg("Chromium Desired Checkout: %s (%s)" % \
        (chromium_desired_hash, chromium_checkout))
  else:
    chromium_checkout_changed = options.dryrun

if cef_checkout_changed:
  if cef_dir != cef_src_dir and os.path.exists(cef_src_dir):
    delete_directory(cef_src_dir)
elif chromium_checkout_changed and cef_dir == cef_src_dir:
  run_patch_updater("--backup --revert")

if options.forceclean and os.path.exists(out_src_dir):
  delete_directory(out_src_dir)

if os.path.exists(out_src_dir):
  old_branch = read_branch_config_file(out_src_dir)
  if old_branch != '' and (chromium_checkout_changed or
                           old_branch != cef_branch):
    old_out_dir = os.path.join(download_dir, 'out_' + old_branch)
    move_directory(out_src_dir, old_out_dir)

if chromium_checkout_changed:
  if not options.nochromiumhistory:
    if not chromium_checkout_new and not options.fastupdate:
      if options.forceclean and options.forcecleandeps:
        run("%s clean -dffx" % (git_exe), chromium_src_dir)
      else:
        run("gclient revert --nohooks", chromium_dir)

    run("%s checkout %s%s" %
      (git_exe, '--force ' if discard_local_changes else '', chromium_checkout),
      chromium_src_dir)

  apply_deps_patch()

  if not options.nochromiumhistory:
    run("gclient sync %s--nohooks --with_branch_heads" %
        ('--reset ' if discard_local_changes else ''), chromium_dir)

  apply_runhooks_patch()

  run("gclient runhooks", chromium_dir)

  delete_directory(out_src_dir)

if cef_dir == cef_src_dir:
  if cef_checkout_changed or chromium_checkout_changed:
    run_patch_updater("--reapply --restore")
elif os.path.exists(cef_dir) and not os.path.exists(cef_src_dir):
  copy_directory(cef_dir, cef_src_dir)

out_src_dir_exists = os.path.exists(out_src_dir)
if os.path.exists(out_dir) and not out_src_dir_exists:
  move_directory(out_dir, out_src_dir)
  out_src_dir_exists = True
elif not out_src_dir_exists:
  create_directory(out_src_dir)

write_branch_config_file(out_src_dir, cef_branch)

if options.logchromiumchanges and chromium_checkout != chromium_compat_version:
  log_chromium_changes()

if options.forcepatchupdate or ((chromium_checkout_new or not options.fastupdate) and \
                                chromium_checkout_changed and \
                                chromium_checkout != chromium_compat_version):
  if options.logchromiumchanges:
    out_file = os.path.join(download_dir, 'chromium_update_patches.txt')
    if os.path.exists(out_file):
      os.remove(out_file)
  else:
    out_file = None
  run_patch_updater(output_file=out_file)
elif options.resave:
  run_patch_updater("--resave")

if chromium_checkout != chromium_compat_version:
  if options.logchromiumchanges:
    out_file = os.path.join(download_dir, 'chromium_update_patterns.txt')
    if os.path.exists(out_file):
      os.remove(out_file)
  else:
    out_file = None
  check_pattern_matches(output_file=out_file)

if not branch_is_master and chromium_checkout_changed:
  msg('Checking for sandbox compat hash changes...')
  tool = os.path.join(cef_src_dir, 'tools', 'version_manager.py')
  run('%s %s -a' % (python_exe, tool), cef_src_dir)

##
##

if not options.nobuild and (chromium_checkout_changed or \
                            cef_checkout_changed or options.forcebuild or \
                            not out_src_dir_exists):
  options.forcedistrib = True

  if not options.dryrun and \
    not os.path.exists(os.path.join(cef_src_dir, 'BUILD.gn')):
    raise Exception('GN configuration does not exist.')

  for key in os.environ.keys():
    if key.startswith('CEF_') or key.startswith('GCLIENT_') or \
       key.startswith('GN_') or key.startswith('GYP_') or \
       key.startswith('DEPOT_TOOLS_'):
      msg('%s=%s' % (key, os.environ[key]))

  if options.mmltkpatchersource != '':
    copy_file(os.path.abspath(options.mmltkpatchersource),
              os.path.join(cef_src_dir, 'tools', 'patcher.py'),
              allow_overwrite=True)

  tool = os.path.join(cef_src_dir, 'tools', 'gclient_hook.py')
  run('%s %s' % (python_exe, tool), cef_src_dir)

  command = 'autoninja '
  if options.mmltkbuildjobs != '':
    command += '-j %s ' % options.mmltkbuildjobs
  if options.verbosebuild:
    command += '-v '
  if options.buildfailurelimit != 1:
    command += '-k %d ' % options.buildfailurelimit
  command += '-C '
  target = ' ' + options.buildtarget
  if options.buildtests:
    target += ' ' + options.testtarget
  if platform == 'linux':
    target += ' chrome_sandbox'
  if platform in bootstrap_exe_platforms:
    target += ' bootstrap bootstrapc'

  def run_out_build(build_path, build_target, log_suffix):
    args_path = os.path.join(chromium_src_dir, build_path, 'args.gn')
    msg(args_path + ' contents:\n' + read_file(args_path))
    run(command + build_path + build_target, chromium_src_dir,
        os.path.join(download_dir, 'build-%s-%s.log' % (cef_branch, log_suffix))
          if options.buildlogfile else None)

  def run_sandbox_build_if_present(build_path, log_suffix):
    if platform not in sandbox_static_platforms:
      return
    sandbox_build_path = build_path + '_sandbox'
    if os.path.exists(os.path.join(chromium_src_dir, sandbox_build_path)):
      run_out_build(sandbox_build_path, ' cef_sandbox', log_suffix + '-sandbox')

  if not options.nodebugbuild:
    build_path = os.path.join('out', get_build_directory_name(True))
    run_out_build(build_path, target, 'debug')
    run_sandbox_build_if_present(build_path, 'debug')

  if not options.noreleasebuild:
    build_path = os.path.join('out', get_build_directory_name(False))
    run_out_build(build_path, target, 'release')
    run_sandbox_build_if_present(build_path, 'release')

elif not options.nobuild:
  msg('Not building. The source hashes have not changed and ' +
      'the output folder "%s" already exists' % (out_src_dir))

##
##

if options.runtests:
  if platform == 'windows':
    test_exe = '%s.exe' % options.testtarget
  elif platform == 'mac':
    test_exe = '%s.app/Contents/MacOS/%s' % (options.testtarget,
                                             options.testtarget)
  elif platform == 'linux':
    test_exe = options.testtarget

  test_prefix = options.testprefix
  if len(test_prefix) > 0:
    test_prefix += ' '

  test_args = options.testargs
  if len(test_args) > 0:
    test_args = ' ' + test_args

  if not options.nodebugtests:
    build_path = os.path.join(out_src_dir, get_build_directory_name(True))
    test_path = os.path.join(build_path, test_exe)
    if os.path.exists(test_path):
      run(test_prefix + test_path + test_args, build_path)
    else:
      msg('Not running debug tests. Missing executable: %s' % test_path)

  if not options.noreleasetests:
    build_path = os.path.join(out_src_dir, get_build_directory_name(False))
    test_path = os.path.join(build_path, test_exe)
    if os.path.exists(test_path):
      run(test_prefix + test_path + test_args, build_path)
    else:
      msg('Not running release tests. Missing executable: %s' % test_path)

##
##

if not options.nodistrib and (chromium_checkout_changed or \
                              cef_checkout_changed or options.forcedistrib):
  artifacts_path = os.path.join(cef_src_dir, 'binary_distrib')
  if not options.forceclean and options.cleanartifacts:
    delete_directory(artifacts_path)
  create_directory(artifacts_path)

  distrib_types = []
  if options.minimaldistribonly:
    distrib_types.append('minimal')
  elif options.clientdistribonly:
    distrib_types.append('client')
  elif options.sandboxdistribonly:
    distrib_types.append('sandbox')
  elif options.toolsdistribonly:
    distrib_types.append('tools')
  else:
    distrib_types.append('standard')
    if options.minimaldistrib:
      distrib_types.append('minimal')
    if options.clientdistrib:
      distrib_types.append('client')
    if options.sandboxdistrib:
      distrib_types.append('sandbox')
    if options.toolsdistrib:
      distrib_types.append('tools')

  cef_tools_dir = os.path.join(cef_src_dir, 'tools')

  commands = []

  first_type = True
  for type in distrib_types:
    path = '%s make_distrib.py --output-dir="%s"' % (python_exe, artifacts_path)

    if options.nodebugbuild or options.noreleasebuild or type != 'standard':
      path += ' --allow-partial'
    path += ' --ninja-build'
    if options.x64build:
      path += ' --x64-build'
    elif options.armbuild:
      path += ' --arm-build'
    elif options.arm64build:
      path += ' --arm64-build'

    if type == 'minimal':
      path += ' --minimal'
    elif type == 'client':
      path += ' --client'
    elif type == 'sandbox':
      path += ' --sandbox'
    elif type == 'tools':
      path += ' --tools'

    if options.nodistribarchive:
      path += ' --no-archive'

    if options.distribsubdir != '':
      path += ' --distrib-subdir=' + options.distribsubdir
    if options.distribsubdirsuffix != '':
      path += ' --distrib-subdir-suffix=' + options.distribsubdirsuffix

    if platform in ('windows', 'mac') and type in ('standard','sandbox') and \
       not branch_is_7151_or_older:
      if not options.nodistribsymbols:
        if not options.nodebugbuild:
          commands.append((path + ' --debug-symbols-only --no-docs', cef_tools_dir))
        if not options.noreleasebuild:
          commands.append((path + ' --release-symbols-only --no-docs', cef_tools_dir))

      path += ' --no-symbols'

    if first_type:
      if options.nodistribdocs:
        path += ' --no-docs'
      if options.nodistribsymbols and branch_is_7151_or_older:
        path += ' --no-symbols'
      first_type = False
    else:
      path += ' --no-docs'
      if not ' --no-symbols' in path:
        path += ' --no-symbols'

    commands.append((path, cef_tools_dir))

  commands_ct = len(commands)
  if commands_ct == 1:
    command = commands[0]
    run(command[0], command[1])
  elif commands_ct > 1:
    success_ct = run_parallel(commands)
    if success_ct != commands_ct:
      print(_stderr_msg(f'ERROR {commands_ct-success_ct} of {commands_ct} commands failed!'))
      sys.exit(1)

if options.mmltkoutputdir != '':
  generate_mmltk_debug_bundle(options.mmltkoutputdir,
                              options.mmltkoutputarchivename)
