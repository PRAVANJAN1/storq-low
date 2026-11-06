"""Python interface for Storq native format operations."""

try:
    from . import _native
except ImportError:
    _native = None


def supported_page_sizes():
    return _native.supported_storq_page_sizes() if _native else [4096, 8192, 16384, 32768, 65536]


def create(path, page_size=4096):
    if _native is None or getattr(_native, "create_storq_database", None) is None:
        raise RuntimeError("The Storq native extension is not available")
    return _native.create_storq_database(str(path), page_size)


def inspect(path, read_only=True):
    del read_only
    if _native is None or getattr(_native, "inspect_storq_database", None) is None:
        raise RuntimeError("The Storq native extension is not available")
    return _native.inspect_storq_database(str(path))
