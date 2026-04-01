#!/usr/bin/env python3
import argparse
import ctypes
import pathlib
import platform
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_SOCKET = "/tmp/sockd-example.sock"
DEFAULT_PID = "/tmp/sockd-example.pid"


def default_library_path() -> pathlib.Path:
    system = platform.system()
    if system == "Darwin":
        return ROOT / "target" / "release" / "libsockd.dylib"
    if system == "Linux":
        return ROOT / "target" / "release" / "libsockd.so"
    if system == "Windows":
        return ROOT / "target" / "release" / "sockd.dll"
    raise RuntimeError(f"unsupported platform: {system}")


class Sockd:
    def __init__(self, library_path: pathlib.Path):
        self.lib = ctypes.CDLL(str(library_path))
        self.lib.sockd_client_config_new.argtypes = [ctypes.c_char_p]
        self.lib.sockd_client_config_new.restype = ctypes.c_void_p

        self.lib.sockd_client_config_free.argtypes = [ctypes.c_void_p]
        self.lib.sockd_client_config_free.restype = None

        self.lib.sockd_client_config_set_pid_file.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.sockd_client_config_set_pid_file.restype = ctypes.c_int

        self.lib.sockd_client_config_set_auto_start.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.sockd_client_config_set_auto_start.restype = ctypes.c_int

        self.lib.sockd_client_request.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.sockd_client_request.restype = ctypes.c_int

        self.lib.sockd_client_shutdown.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_bool),
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.sockd_client_shutdown.restype = ctypes.c_int

        self.lib.sockd_buffer_free.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
        self.lib.sockd_buffer_free.restype = None

        self.lib.sockd_string_free.argtypes = [ctypes.c_char_p]
        self.lib.sockd_string_free.restype = None

    def check(self, rc: int, error_ptr: ctypes.c_char_p) -> None:
        if rc == 0:
            return
        message = "sockd operation failed"
        if error_ptr.value:
            message = error_ptr.value.decode("utf-8")
        self.lib.sockd_string_free(error_ptr)
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description="sockd Python FFI example client")
    parser.add_argument("message", nargs="*", help="message to send")
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    parser.add_argument("--pid", default=DEFAULT_PID)
    parser.add_argument("--shutdown", action="store_true")
    parser.add_argument("--auto-start", help="path to daemon binary for auto-start")
    parser.add_argument("--lib", dest="library_path", default=str(default_library_path()))
    args = parser.parse_args()

    sockd = Sockd(pathlib.Path(args.library_path))
    config = sockd.lib.sockd_client_config_new(args.socket.encode("utf-8"))
    if not config:
        raise RuntimeError("failed to allocate sockd client config")

    error = ctypes.c_char_p()
    try:
        sockd.check(
            sockd.lib.sockd_client_config_set_pid_file(
                config, args.pid.encode("utf-8"), ctypes.byref(error)
            ),
            error,
        )

        if args.auto_start:
            argv = (ctypes.c_char_p * 0)()
            sockd.check(
                sockd.lib.sockd_client_config_set_auto_start(
                    config,
                    args.auto_start.encode("utf-8"),
                    0,
                    argv,
                    ctypes.byref(error),
                ),
                error,
            )

        if args.shutdown:
            stopped = ctypes.c_bool(False)
            sockd.check(
                sockd.lib.sockd_client_shutdown(
                    config, ctypes.byref(stopped), ctypes.byref(error)
                ),
                error,
            )
            print(str(bool(stopped.value)).lower())
            return 0

        message = " ".join(args.message) if args.message else "hello from python"
        payload = message.encode("utf-8")
        payload_array = (ctypes.c_uint8 * len(payload)).from_buffer_copy(payload)
        response_ptr = ctypes.POINTER(ctypes.c_uint8)()
        response_len = ctypes.c_size_t()
        sockd.check(
            sockd.lib.sockd_client_request(
                config,
                payload_array,
                len(payload),
                ctypes.byref(response_ptr),
                ctypes.byref(response_len),
                ctypes.byref(error),
            ),
            error,
        )

        response = ctypes.string_at(response_ptr, response_len.value)
        print(response.decode("utf-8"))
        sockd.lib.sockd_buffer_free(response_ptr, response_len.value)
        return 0
    finally:
        sockd.lib.sockd_client_config_free(config)


if __name__ == "__main__":
    sys.exit(main())
