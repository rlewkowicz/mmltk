# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import argparse
import ast
import difflib
import errno
import importlib.metadata
import shlex
import sys
import types
import uuid
from collections.abc import Iterable
from pathlib import Path
from typing import Optional, Union

from mozfile import load_source

from .base import MissingFileError, UnknownCommandError

INVALID_ENTRY_POINT = r"""
Entry points should return a list of command providers or directories
containing command providers. The following entry point is invalid:

    %s

You are seeing this because there is an error in an external module attempting
to implement a mach command. Please fix the error, or uninstall the module from
your system.
""".lstrip()


class MachCommandReference:
    """A reference to a mach command.

    Holds the metadata for a mach command.
    """

    module: Path

    def __init__(
        self,
        module: Union[str, Path],
        command_dependencies: Optional[list] = None,
    ):
        self.module = Path(module)
        self.command_dependencies = command_dependencies or []


MACH_COMMANDS = {
    "build": MachCommandReference(
        "python/mozbuild/mozbuild/build_commands.py",
    ),
    "build-backend": MachCommandReference(
        "python/mozbuild/mozbuild/build_commands.py",
    ),
    "cargo": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "clobber": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "configure": MachCommandReference("python/mozbuild/mozbuild/build_commands.py"),
    "doctor": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "environment": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "install": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "mmltk-stage-runtime": MachCommandReference(
        "python/mozbuild/mozbuild/mach_commands.py"
    ),
    "package": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "package-multi-locale": MachCommandReference(
        "python/mozbuild/mozbuild/mach_commands.py"
    ),
    "repackage": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "repackage-single-locales": MachCommandReference(
        "python/mozbuild/mozbuild/mach_commands.py"
    ),
    "resource-usage": MachCommandReference(
        "python/mozbuild/mozbuild/build_commands.py",
    ),
    "run": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "show-log": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "source-package": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "warnings-list": MachCommandReference("python/mozbuild/mozbuild/mach_commands.py"),
    "warnings-summary": MachCommandReference(
        "python/mozbuild/mozbuild/mach_commands.py"
    ),
    "watch": MachCommandReference(
        "python/mozbuild/mozbuild/mach_commands.py",
    ),
}


class DecoratorVisitor(ast.NodeVisitor):
    def __init__(self):
        self.results = {}

    def visit_FunctionDef(self, node):
        decorators = [
            decorator
            for decorator in node.decorator_list
            if isinstance(decorator, ast.Call)
            and isinstance(decorator.func, ast.Name)
            and decorator.func.id in ["SubCommand", "Command"]
        ]

        relevant_kwargs = ["command", "subcommand", "virtualenv_name"]

        for decorator in decorators:
            kwarg_dict = {}

            for name, arg in zip(["command", "subcommand"], decorator.args):
                kwarg_dict[name] = arg.value

            for keyword in decorator.keywords:
                if keyword.arg not in relevant_kwargs:
                    continue

                kwarg_dict[keyword.arg] = keyword.value.value

            command = kwarg_dict.pop("command")
            self.results.setdefault(command, {})

            sub_command = kwarg_dict.pop("subcommand", None)
            virtualenv_name = kwarg_dict.pop("virtualenv_name", None)

            if sub_command:
                self.results[command].setdefault("subcommands", {})
                sub_command_dict = self.results[command]["subcommands"].setdefault(
                    sub_command, {}
                )

                if virtualenv_name:
                    sub_command_dict["virtualenv_name"] = virtualenv_name
            elif virtualenv_name:
                self.results[command]["virtualenv_name"] = virtualenv_name

        self.generic_visit(node)


def command_virtualenv_info_for_module(module_path):
    with module_path.open("r", encoding="utf-8") as file:
        content = file.read()

    tree = ast.parse(content)
    visitor = DecoratorVisitor()
    visitor.visit(tree)

    return visitor.results


class DetermineCommandVenvAction(argparse.Action):
    def __init__(
        self,
        option_strings,
        dest,
        topsrcdir,
        required=True,
    ):
        self.topsrcdir = topsrcdir
        argparse.Action.__init__(
            self,
            option_strings,
            dest,
            required=required,
            help=argparse.SUPPRESS,
            nargs=argparse.REMAINDER,
        )

    def __call__(self, parser, namespace, values, option_string=None):
        if len(values) == 0:
            return

        command = values[0]

        aliases = namespace.mach_command_aliases

        if command in aliases:
            alias = aliases[command]
            arg_string = shlex.split(alias)
            command = arg_string.pop(0)

        if command == "help":
            return

        command_reference = MACH_COMMANDS.get(command)

        if not command_reference:
            suggested_command = suggest_command(command)

            sys.stderr.write(
                f"We're assuming the '{command}' command is '{suggested_command}' and we're executing it for you.\n\n"
            )

            command = suggested_command
            command_reference = MACH_COMMANDS.get(command)

        setattr(namespace, "command_name", command)

        if len(values) > 1:
            potential_sub_command_name = values[1]
        else:
            potential_sub_command_name = None

        module_path = Path(self.topsrcdir) / command_reference.module
        module_dict = command_virtualenv_info_for_module(module_path)
        command_dict = module_dict.get(command, {})

        if not command_dict:
            return

        site = command_dict.get("virtualenv_name", "common")

        if potential_sub_command_name and not potential_sub_command_name.startswith(
            "-"
        ):
            all_sub_commands_dict = command_dict.get("subcommands", {})

            if all_sub_commands_dict:
                sub_command_dict = all_sub_commands_dict.get(
                    potential_sub_command_name, {}
                )

                if sub_command_dict:
                    site = sub_command_dict.get("virtualenv_name", "common")

        setattr(namespace, "site_name", site)


def suggest_command(command):
    names = MACH_COMMANDS.keys()
    suggested_commands = difflib.get_close_matches(command, names, cutoff=0.8)
    if len(suggested_commands) != 1:
        suggested_commands = set(difflib.get_close_matches(command, names, cutoff=0.5))
        suggested_commands |= {cmd for cmd in names if cmd.startswith(command)}
        raise UnknownCommandError(command, "run", suggested_commands)

    return suggested_commands[0]


def load_commands_from_directory(path: Path):
    """Scan for mach commands from modules in a directory.

    This takes a path to a directory, loads the .py files in it, and
    registers and found mach command providers with this mach instance.
    """
    for f in sorted(path.iterdir()):
        if not f.suffix == ".py" or f.name == "__init__.py":
            continue

        full_path = path / f
        module_name = f"mach.commands.{str(f)[0:-3]}"

        load_commands_from_file(full_path, module_name=module_name)


def load_commands_from_file(path: Union[str, Path], module_name=None):
    """Scan for mach commands from a file.

    This takes a path to a file and loads it as a Python module under the
    module name specified. If no name is specified, a random one will be
    chosen.
    """
    if module_name is None:
        if "mach.commands" not in sys.modules:
            mod = types.ModuleType("mach.commands")
            sys.modules["mach.commands"] = mod

        module_name = f"mach.commands.{uuid.uuid4().hex}"

    try:
        load_source(module_name, str(path))
    except OSError as e:
        if e.errno != errno.ENOENT:
            raise

        raise MissingFileError(f"{path} does not exist")


def load_commands_from_spec(
    spec: dict[str, MachCommandReference], topsrcdir: str, missing_ok=False
):
    """Load mach commands based on the given spec.

    Takes a dictionary mapping command names to their metadata.
    """
    modules = {spec[command].module for command in spec}

    for path in modules:
        try:
            load_commands_from_file(Path(topsrcdir) / path)
        except MissingFileError:
            if not missing_ok:
                raise


def load_commands_from_entry_point(group="mach.providers"):
    """Scan installed packages for mach command provider entry points. An
    entry point is a function that returns a list of paths to files or
    directories containing command providers.

    This takes an optional group argument which specifies the entry point
    group to use. If not specified, it defaults to 'mach.providers'.
    """
    for entry in importlib.metadata.entry_points(group=group):
        paths = [Path(path) for path in entry.load()()]
        if not isinstance(paths, Iterable):
            print(INVALID_ENTRY_POINT % entry)
            sys.exit(1)

        for path in paths:
            if path.is_file():
                load_commands_from_file(path)
            elif path.is_dir():
                load_commands_from_directory(path)
            else:
                print(f"command provider '{path}' does not exist")


def load_command_module_from_command_name(command_name: str, topsrcdir: str):
    command_reference = MACH_COMMANDS.get(command_name)
    load_commands_from_spec(
        {command_name: command_reference}, topsrcdir, missing_ok=False
    )
