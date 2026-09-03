"""Compatibility helpers for the supported Python 3.8+ runtime range."""

from __future__ import annotations

import argparse
import asyncio
import contextvars
import enum
import functools
from collections.abc import Callable
from typing import Any, TypeVar

T = TypeVar("T")


class StrEnum(str, enum.Enum):
    """Python 3.8-compatible subset of :class:`enum.StrEnum`."""

    def __str__(self) -> str:
        return str.__str__(self)

    def __format__(self, format_spec: str) -> str:
        return str.__format__(self, format_spec)


class BooleanOptionalAction(argparse.Action):
    """Backport of argparse.BooleanOptionalAction from Python 3.9."""

    def __init__(
        self,
        option_strings: list[str],
        dest: str,
        default: Any = None,
        required: bool = False,
        help: str | None = None,
    ) -> None:
        expanded = []
        for option in option_strings:
            expanded.append(option)
            if option.startswith("--"):
                expanded.append("--no-" + option[2:])
        super().__init__(
            option_strings=expanded,
            dest=dest,
            nargs=0,
            default=default,
            required=required,
            help=help,
        )

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: Any,
        option_string: str | None = None,
    ) -> None:
        del parser, values
        setattr(namespace, self.dest, not str(option_string).startswith("--no-"))

    def format_usage(self) -> str:
        return " | ".join(self.option_strings)


async def to_thread(function: Callable[..., T], /, *args: Any, **kwargs: Any) -> T:
    """Run a function in a worker while preserving the current context."""

    loop = asyncio.get_running_loop()
    context = contextvars.copy_context()
    call = functools.partial(context.run, function, *args, **kwargs)
    return await loop.run_in_executor(None, call)


def remove_prefix(value: str, prefix: str) -> str:
    """Return *value* without *prefix* when it is present."""

    return value[len(prefix) :] if value.startswith(prefix) else value


__all__ = ["BooleanOptionalAction", "StrEnum", "remove_prefix", "to_thread"]
