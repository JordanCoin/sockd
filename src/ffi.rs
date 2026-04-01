use crate::{AutoStart, Client};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::ptr;
use std::slice;
use std::time::Duration;

const SOCKD_OK: c_int = 0;
const SOCKD_ERR_NULL: c_int = 1;
const SOCKD_ERR_UTF8: c_int = 2;
const SOCKD_ERR_OPERATION: c_int = 3;

#[repr(C)]
pub struct SockdClientConfig {
    socket_path: String,
    pid_file: Option<String>,
    auto_start_program: Option<String>,
    auto_start_args: Vec<String>,
    max_payload_len: usize,
    shutdown_timeout_ms: u64,
}

impl SockdClientConfig {
    fn build_client(&self) -> Client {
        let mut client = Client::new(self.socket_path.clone())
            .with_max_payload_len(self.max_payload_len)
            .with_shutdown_timeout(Duration::from_millis(self.shutdown_timeout_ms));

        if let Some(pid_file) = &self.pid_file {
            client = client.with_pid_file(pid_file.clone());
        }

        if let Some(program) = &self.auto_start_program {
            let auto_start = AutoStart::new(program.clone()).args(self.auto_start_args.clone());
            client = client.with_auto_start_config(auto_start);
        }

        client
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn sockd_client_config_new(socket_path: *const c_char) -> *mut SockdClientConfig {
    let socket_path = match c_string(socket_path) {
        Ok(value) => value,
        Err(_) => return ptr::null_mut(),
    };

    Box::into_raw(Box::new(SockdClientConfig {
        socket_path,
        pid_file: None,
        auto_start_program: None,
        auto_start_args: Vec::new(),
        max_payload_len: crate::frame::DEFAULT_MAX_PAYLOAD_LEN,
        shutdown_timeout_ms: 2_000,
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_config_free(config: *mut SockdClientConfig) {
    if !config.is_null() {
        let _ = unsafe { Box::from_raw(config) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_config_set_pid_file(
    config: *mut SockdClientConfig,
    pid_file: *const c_char,
    error_out: *mut *mut c_char,
) -> c_int {
    let config = match config_mut(config, error_out) {
        Some(config) => config,
        None => return SOCKD_ERR_NULL,
    };
    let pid_file = match c_string_result(pid_file, error_out) {
        Some(value) => value,
        None => return SOCKD_ERR_UTF8,
    };

    config.pid_file = Some(pid_file);
    SOCKD_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_config_set_auto_start(
    config: *mut SockdClientConfig,
    program: *const c_char,
    argc: usize,
    argv: *const *const c_char,
    error_out: *mut *mut c_char,
) -> c_int {
    let config = match config_mut(config, error_out) {
        Some(config) => config,
        None => return SOCKD_ERR_NULL,
    };
    let program = match c_string_result(program, error_out) {
        Some(value) => value,
        None => return SOCKD_ERR_UTF8,
    };

    let args = match c_string_vec(argc, argv, error_out) {
        Some(args) => args,
        None => return SOCKD_ERR_UTF8,
    };

    config.auto_start_program = Some(program);
    config.auto_start_args = args;
    SOCKD_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_config_set_max_payload_len(
    config: *mut SockdClientConfig,
    max_payload_len: usize,
    error_out: *mut *mut c_char,
) -> c_int {
    let config = match config_mut(config, error_out) {
        Some(config) => config,
        None => return SOCKD_ERR_NULL,
    };

    config.max_payload_len = max_payload_len;
    SOCKD_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_config_set_shutdown_timeout_ms(
    config: *mut SockdClientConfig,
    shutdown_timeout_ms: u64,
    error_out: *mut *mut c_char,
) -> c_int {
    let config = match config_mut(config, error_out) {
        Some(config) => config,
        None => return SOCKD_ERR_NULL,
    };

    config.shutdown_timeout_ms = shutdown_timeout_ms;
    SOCKD_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_ping(
    config: *const SockdClientConfig,
    error_out: *mut *mut c_char,
) -> c_int {
    let config = match config_ref(config, error_out) {
        Some(config) => config,
        None => return SOCKD_ERR_NULL,
    };

    match config.build_client().ping() {
        Ok(()) => SOCKD_OK,
        Err(err) => write_error(error_out, err.to_string(), SOCKD_ERR_OPERATION),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_request(
    config: *const SockdClientConfig,
    request_ptr: *const u8,
    request_len: usize,
    response_ptr_out: *mut *mut u8,
    response_len_out: *mut usize,
    error_out: *mut *mut c_char,
) -> c_int {
    let config = match config_ref(config, error_out) {
        Some(config) => config,
        None => return SOCKD_ERR_NULL,
    };
    if response_ptr_out.is_null() || response_len_out.is_null() {
        return write_error(
            error_out,
            "response output pointers must not be null",
            SOCKD_ERR_NULL,
        );
    }

    let request = if request_len == 0 {
        &[]
    } else if request_ptr.is_null() {
        return write_error(
            error_out,
            "request pointer must not be null when request_len > 0",
            SOCKD_ERR_NULL,
        );
    } else {
        slice_from_raw_parts(request_ptr, request_len)
    };

    match config.build_client().request(request) {
        Ok(mut response) => {
            let len = response.len();
            let ptr = response.as_mut_ptr();
            std::mem::forget(response);
            unsafe {
                *response_ptr_out = ptr;
                *response_len_out = len;
            }
            SOCKD_OK
        }
        Err(err) => write_error(error_out, err.to_string(), SOCKD_ERR_OPERATION),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_client_shutdown(
    config: *const SockdClientConfig,
    stopped_out: *mut bool,
    error_out: *mut *mut c_char,
) -> c_int {
    let config = match config_ref(config, error_out) {
        Some(config) => config,
        None => return SOCKD_ERR_NULL,
    };
    if stopped_out.is_null() {
        return write_error(
            error_out,
            "stopped_out pointer must not be null",
            SOCKD_ERR_NULL,
        );
    }

    match config.build_client().shutdown() {
        Ok(stopped) => {
            set_bool(stopped_out, stopped);
            SOCKD_OK
        }
        Err(err) => write_error(error_out, err.to_string(), SOCKD_ERR_OPERATION),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_buffer_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() {
        let _ = vec_from_raw_parts(ptr, len);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sockd_string_free(ptr: *mut c_char) {
    if !ptr.is_null() {
        let _ = c_string_from_raw(ptr);
    }
}

fn c_string(input: *const c_char) -> Result<String, &'static str> {
    if input.is_null() {
        return Err("pointer must not be null");
    }

    let input = c_str_from_ptr(input);
    input
        .to_str()
        .map(str::to_owned)
        .map_err(|_| "value must be valid UTF-8")
}

fn config_mut<'a>(
    config: *mut SockdClientConfig,
    error_out: *mut *mut c_char,
) -> Option<&'a mut SockdClientConfig> {
    if config.is_null() {
        write_error(error_out, "config pointer must not be null", SOCKD_ERR_NULL);
        return None;
    }

    Some(mut_ref_from_ptr(config))
}

fn config_ref<'a>(
    config: *const SockdClientConfig,
    error_out: *mut *mut c_char,
) -> Option<&'a SockdClientConfig> {
    if config.is_null() {
        write_error(error_out, "config pointer must not be null", SOCKD_ERR_NULL);
        return None;
    }

    Some(ref_from_ptr(config))
}

fn c_string_result(input: *const c_char, error_out: *mut *mut c_char) -> Option<String> {
    match c_string(input) {
        Ok(value) => Some(value),
        Err(message) => {
            write_error(error_out, message, SOCKD_ERR_UTF8);
            None
        }
    }
}

fn c_string_vec(
    argc: usize,
    argv: *const *const c_char,
    error_out: *mut *mut c_char,
) -> Option<Vec<String>> {
    if argc == 0 {
        return Some(Vec::new());
    }
    if argv.is_null() {
        write_error(
            error_out,
            "argv pointer must not be null when argc > 0",
            SOCKD_ERR_NULL,
        );
        return None;
    }

    let argv = slice_from_raw_parts(argv, argc);
    let mut out = Vec::with_capacity(argc);
    for arg in argv {
        let Some(value) = c_string_result(*arg, error_out) else {
            return None;
        };
        out.push(value);
    }
    Some(out)
}

fn c_str_from_ptr<'a>(ptr: *const c_char) -> &'a CStr {
    unsafe { CStr::from_ptr(ptr) }
}

fn mut_ref_from_ptr<'a, T>(ptr: *mut T) -> &'a mut T {
    unsafe { &mut *ptr }
}

fn ref_from_ptr<'a, T>(ptr: *const T) -> &'a T {
    unsafe { &*ptr }
}

fn slice_from_raw_parts<'a, T>(ptr: *const T, len: usize) -> &'a [T] {
    unsafe { slice::from_raw_parts(ptr, len) }
}

fn vec_from_raw_parts<T>(ptr: *mut T, len: usize) -> Vec<T> {
    unsafe { Vec::from_raw_parts(ptr, len, len) }
}

fn c_string_from_raw(ptr: *mut c_char) -> CString {
    unsafe { CString::from_raw(ptr) }
}

fn set_bool(ptr: *mut bool, value: bool) {
    unsafe { *ptr = value };
}

fn write_error(error_out: *mut *mut c_char, message: impl AsRef<str>, code: c_int) -> c_int {
    if !error_out.is_null() {
        let mut bytes: Vec<u8> = message.as_ref().bytes().filter(|byte| *byte != 0).collect();
        bytes.push(0);

        let c_string = unsafe { CString::from_vec_with_nul_unchecked(bytes) };
        unsafe {
            *error_out = c_string.into_raw();
        }
    }
    code
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Client, Daemon};
    use std::thread;
    use std::time::Instant;
    use tempfile::tempdir;

    #[test]
    fn ffi_request_and_shutdown_round_trip() {
        let temp = tempdir().unwrap();
        let socket_path = temp.path().join("ffi.sock");
        let pid_path = temp.path().join("ffi.pid");

        let daemon = Daemon::builder()
            .socket(&socket_path)
            .pid_file(&pid_path)
            .idle_timeout(Duration::from_secs(5))
            .poll_interval(Duration::from_millis(20))
            .on_request(|_, req| Ok(req.iter().map(u8::to_ascii_uppercase).collect()))
            .build()
            .unwrap();

        let handle = thread::spawn(move || daemon.run());

        let ready_client = Client::new(&socket_path).with_pid_file(&pid_path);
        let deadline = Instant::now() + Duration::from_secs(1);
        while Instant::now() < deadline {
            if ready_client.ping().is_ok() {
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }

        let socket_c = CString::new(socket_path.to_str().unwrap()).unwrap();
        let pid_c = CString::new(pid_path.to_str().unwrap()).unwrap();
        let request = b"ffi hello";

        let config = sockd_client_config_new(socket_c.as_ptr());
        assert!(!config.is_null());

        let mut error = ptr::null_mut();
        assert_eq!(
            unsafe { sockd_client_config_set_pid_file(config, pid_c.as_ptr(), &mut error) },
            SOCKD_OK
        );

        let mut response_ptr = ptr::null_mut();
        let mut response_len = 0usize;
        assert_eq!(
            unsafe {
                sockd_client_request(
                    config,
                    request.as_ptr(),
                    request.len(),
                    &mut response_ptr,
                    &mut response_len,
                    &mut error,
                )
            },
            SOCKD_OK
        );
        let response = unsafe { slice::from_raw_parts(response_ptr, response_len) };
        assert_eq!(response, b"FFI HELLO");
        unsafe { sockd_buffer_free(response_ptr, response_len) };

        let mut stopped = false;
        assert_eq!(
            unsafe { sockd_client_shutdown(config, &mut stopped, &mut error) },
            SOCKD_OK
        );
        assert!(stopped);

        unsafe { sockd_client_config_free(config) };
        let daemon_result = handle.join().unwrap();
        assert!(daemon_result.is_ok(), "{daemon_result:?}");
    }
}
