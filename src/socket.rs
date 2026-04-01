use crate::error::{Error, Result};
use std::fs;
use std::io;
use std::os::unix::fs::FileTypeExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Path;

pub(crate) fn connect(path: &Path) -> io::Result<UnixStream> {
    UnixStream::connect(path)
}

pub(crate) fn bind(path: &Path) -> Result<UnixListener> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(Error::Io)?;
    }

    if path.exists() {
        let metadata = fs::symlink_metadata(path).map_err(Error::Io)?;
        if !metadata.file_type().is_socket() {
            return Err(Error::Io(io::Error::new(
                io::ErrorKind::AlreadyExists,
                format!("socket path exists and is not a socket: {}", path.display()),
            )));
        }

        match UnixStream::connect(path) {
            Ok(_) => {
                return Err(Error::DaemonAlreadyRunning {
                    pid: None,
                    socket: Some(path.to_path_buf()),
                });
            }
            Err(err)
                if matches!(
                    err.kind(),
                    io::ErrorKind::ConnectionRefused
                        | io::ErrorKind::ConnectionAborted
                        | io::ErrorKind::NotFound
                ) =>
            {
                fs::remove_file(path).map_err(Error::Io)?;
            }
            Err(err) => return Err(Error::Io(err)),
        }
    }

    let listener = UnixListener::bind(path).map_err(Error::Io)?;
    listener.set_nonblocking(true).map_err(Error::Io)?;
    Ok(listener)
}

pub(crate) fn cleanup(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(err) if err.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(err) => Err(Error::Io(err)),
    }
}
