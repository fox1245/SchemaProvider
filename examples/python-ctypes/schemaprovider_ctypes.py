"""Thin ctypes wrapper over the example C ABI in sp_capi.h.

A worked example, not an official SchemaProvider Python package. The SDK's typed conversation
(including its authenticated native replay state) stays inside the shared library; Python only
sends JSON turns and reads JSON answers.
"""
from __future__ import annotations

import ctypes
import json
import os
import sys
import threading
from pathlib import Path
from typing import Any, Optional, Sequence


class SchemaProviderError(RuntimeError):
    """A failed send. `kind` is the SDK's ErrorKind name (for example RateLimited); the attempt
    evidence says whether the request may have reached the provider (retry could duplicate work)."""

    def __init__(self, kind: str, message: str, **evidence: Any):
        super().__init__(f"{kind}: {message}")
        self.kind = kind
        self.message = message
        self.evidence = evidence


def _default_library() -> str:
    name = {"win32": "sp_capi.dll", "darwin": "libsp_capi.dylib"}.get(sys.platform, "libsp_capi.so")
    return os.environ.get("SP_CAPI_LIBRARY", str(Path(__file__).parent / "build" / name))


class _Api:
    def __init__(self, path: str):
        # CDLL releases the GIL for the duration of every foreign call, so sends from several
        # Python threads (LangGraph parallel nodes, a thread pool) overlap.
        lib = ctypes.CDLL(path)
        c_str, c_ptr, c_err = ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)
        lib.sp_capi_interface_revision.restype = ctypes.c_uint32
        lib.sp_capi_string_free.argtypes = [c_ptr]
        lib.sp_capi_string_free.restype = None
        lib.sp_capi_client_create.argtypes = [c_str, c_str, c_str, c_str, c_err]
        lib.sp_capi_client_create.restype = c_ptr
        lib.sp_capi_client_destroy.argtypes = [c_ptr]
        lib.sp_capi_client_destroy.restype = None
        lib.sp_capi_conversation_create.argtypes = [c_ptr, c_str, c_str, ctypes.c_uint32, c_str, c_err]
        lib.sp_capi_conversation_create.restype = c_ptr
        lib.sp_capi_conversation_destroy.argtypes = [c_ptr]
        lib.sp_capi_conversation_destroy.restype = None
        # c_void_p (not c_char_p) keeps the raw pointer so it can be freed by the library.
        lib.sp_capi_conversation_send.argtypes = [c_ptr, c_str, ctypes.c_int, ctypes.c_uint32]
        lib.sp_capi_conversation_send.restype = c_ptr
        self.lib = lib

    def take(self, pointer: Optional[int]) -> str:
        if not pointer:
            raise SchemaProviderError("Unknown", "the library returned no text")
        try:
            return ctypes.string_at(pointer).decode("utf-8")
        finally:
            self.lib.sp_capi_string_free(pointer)


_apis: dict[str, _Api] = {}
_apis_lock = threading.Lock()


def _api(path: str) -> _Api:
    with _apis_lock:
        if path not in _apis:
            _apis[path] = _Api(path)
        return _apis[path]


class Client:
    """One immutable descriptor and credential. Safe to share between threads."""

    def __init__(self, descriptor: dict[str, Any], *, api_key: Optional[str] = None,
                 dotenv_path: Optional[str] = None, api_key_name: Optional[str] = None,
                 library: Optional[str] = None):
        self._api = _api(library or _default_library())
        self._lock = threading.Lock()
        error = ctypes.c_void_p()
        self._handle = self._api.lib.sp_capi_client_create(
            json.dumps(descriptor).encode(), api_key.encode() if api_key else None,
            dotenv_path.encode() if dotenv_path else None, api_key_name.encode() if api_key_name else None,
            ctypes.byref(error))
        if not self._handle:
            raise SchemaProviderError("InvalidConfig", self._api.take(error.value))

    @property
    def interface_revision(self) -> int:
        return int(self._api.lib.sp_capi_interface_revision())

    def conversation(self, model: str, *, system: str = "", max_output_tokens: int = 256,
                     tools: Optional[Sequence[dict[str, Any]]] = None) -> "Conversation":
        return Conversation(self, model, system, max_output_tokens, tools)

    def close(self) -> None:
        with self._lock:
            if self._handle:
                self._api.lib.sp_capi_client_destroy(self._handle)
                self._handle = None

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class Conversation:
    """Typed history lives in C++; use one conversation per independent chat."""

    def __init__(self, client: Client, model: str, system: str, max_output_tokens: int,
                 tools: Optional[Sequence[dict[str, Any]]]):
        self._client = client
        self._lock = threading.Lock()
        self._api = client._api
        error = ctypes.c_void_p()
        with client._lock:
            if not client._handle:
                raise SchemaProviderError("Misuse", "client is closed")
            self._handle = self._api.lib.sp_capi_conversation_create(
                client._handle, model.encode(), system.encode(), max_output_tokens,
                json.dumps(list(tools)).encode() if tools else None, ctypes.byref(error))
            if not self._handle:
                raise SchemaProviderError("InvalidRequest", self._api.take(error.value))

    def _send(self, turn: dict[str, Any], streaming: bool, timeout_ms: int) -> dict[str, Any]:
        with self._lock:
            if not self._handle:
                raise SchemaProviderError("Misuse", "conversation is closed")
            text = self._api.take(self._api.lib.sp_capi_conversation_send(
                self._handle, json.dumps(turn).encode(), int(streaming), timeout_ms))
        outcome = json.loads(text)
        if not outcome["ok"]:
            raise SchemaProviderError(outcome["error"]["kind"], outcome["error"]["message"],
                                      **{k: v for k, v in outcome["error"].items() if k not in ("kind", "message")})
        return outcome

    def say(self, text: str, *, streaming: bool = False, timeout_ms: int = 30000) -> dict[str, Any]:
        return self._send({"user": text}, streaming, timeout_ms)

    def tool_results(self, results: Sequence[dict[str, str]], *, streaming: bool = False,
                     timeout_ms: int = 30000) -> dict[str, Any]:
        return self._send({"tool_results": list(results)}, streaming, timeout_ms)

    def close(self) -> None:
        with self._lock:
            if self._handle:
                self._api.lib.sp_capi_conversation_destroy(self._handle)
                self._handle = None

    def __enter__(self) -> "Conversation":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
