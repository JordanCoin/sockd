use crate::config::AutoStart;
use crate::error::{Error, Result};
use std::io;
use std::process::{Command, Stdio};

pub(crate) fn spawn_detached(auto_start: &AutoStart) -> Result<()> {
    let mut command = Command::new(&auto_start.program);
    command
        .args(&auto_start.args)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());

    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;

        unsafe {
            command.pre_exec(|| {
                if libc::setsid() == -1 {
                    return Err(io::Error::last_os_error());
                }
                Ok(())
            });
        }
    }

    command.spawn().map_err(Error::Io)?;
    Ok(())
}
