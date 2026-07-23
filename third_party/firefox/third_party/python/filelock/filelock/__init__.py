"""
A platform independent file lock that supports the with-statement.

.. autodata:: filelock.__version__
   :no-value:

"""

from __future__ import annotations

import warnings
from typing import TYPE_CHECKING

from ._api import AcquireReturnProxy, BaseFileLock
from ._error import Timeout
from ._soft import SoftFileLock
from ._unix import UnixFileLock, has_fcntl
from .asyncio import (
    AsyncAcquireReturnProxy,
    AsyncSoftFileLock,
    AsyncUnixFileLock,
    BaseAsyncFileLock,
)
from .version import version

__version__: str = version


if has_fcntl:
    _FileLock: type[BaseFileLock] = UnixFileLock
    _AsyncFileLock: type[BaseAsyncFileLock] = AsyncUnixFileLock
else:
    _FileLock = SoftFileLock
    _AsyncFileLock = AsyncSoftFileLock
    warnings.warn("only soft file lock is available", stacklevel=2)

if TYPE_CHECKING:
    FileLock = SoftFileLock
    AsyncFileLock = AsyncSoftFileLock
else:
    FileLock = _FileLock
    AsyncFileLock = _AsyncFileLock


__all__ = [
    "AcquireReturnProxy",
    "AsyncAcquireReturnProxy",
    "AsyncFileLock",
    "AsyncSoftFileLock",
    "AsyncUnixFileLock",
    "BaseAsyncFileLock",
    "BaseFileLock",
    "FileLock",
    "SoftFileLock",
    "Timeout",
    "UnixFileLock",
    "__version__",
]
