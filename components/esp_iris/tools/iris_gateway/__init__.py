"""ESP-Iris PC hub."""

from .protocol import Frame, FrameDecoder

__all__ = ["Frame", "FrameDecoder", "IrisHub"]
__version__ = "1.0.0"


def __getattr__(name: str):
    """Keep bundle-only tooling independent of Gateway runtime packages."""

    if name == "IrisHub":
        from .hub import IrisHub

        return IrisHub
    raise AttributeError(name)
