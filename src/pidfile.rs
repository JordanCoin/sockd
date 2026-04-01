use crate::error::{Error, Result};
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

pub(crate) struct PidGuard {
    path: PathBuf,
}

impl PidGuard {
    pub(crate) fn claim(path: &Path) -> Result<Self> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(Error::Io)?;
        }

        for _ in 0..3 {
            if path.exists() {
                if let Some(pid) = read_pid(path)? && is_process_alive(pid) {
                    return Err(Error::DaemonAlreadyRunning {
                        pid: Some(pid),
                        socket: None,
                    });
                }
                remove_if_exists(path)?;
            }

            match OpenOptions::new().create_new(true).write(true).open(path) {
                Ok(mut file) => {
                    writeln!(file, "{}", std::process::id()).map_err(Error::Io)?;
                    file.flush().map_err(Error::Io)?;
                    return Ok(Self {
                        path: path.to_path_buf(),
                    });
                }
                Err(err) if err.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(err) => return Err(Error::Io(err)),
            }
        }

        Err(Error::DaemonAlreadyRunning {
            pid: read_pid(path)?,
            socket: None,
        })
    }
}

impl Drop for PidGuard {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

pub(crate) fn read_pid(path: &Path) -> Result<Option<u32>> {
    let content = match fs::read_to_string(path) {
        Ok(content) => content,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(err) => return Err(Error::Io(err)),
    };

    let pid = match content.trim().parse::<u32>() {
        Ok(pid) => pid,
        Err(_) => return Ok(None),
    };
    Ok(Some(pid))
}

pub(crate) fn is_process_alive(pid: u32) -> bool {
    let rc = unsafe { libc::kill(pid as i32, 0) };
    if rc == 0 {
        return true;
    }

    matches!(io::Error::last_os_error().raw_os_error(), Some(libc::EPERM))
}

fn remove_if_exists(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(err) if err.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(err) => Err(Error::Io(err)),
    }
}
