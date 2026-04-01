use sockd::Daemon;
use std::env;
use std::error::Error;
use std::path::PathBuf;
use std::time::Duration;

fn main() -> Result<(), Box<dyn Error + Send + Sync + 'static>> {
    let mut socket = None;
    let mut pid = None;
    let mut idle_ms = 1_000u64;

    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--socket" => socket = args.next().map(PathBuf::from),
            "--pid" => pid = args.next().map(PathBuf::from),
            "--idle-ms" => {
                if let Some(value) = args.next() {
                    idle_ms = value.parse()?;
                }
            }
            other => return Err(format!("unknown argument: {other}").into()),
        }
    }

    let socket = socket.ok_or("missing --socket")?;

    let mut builder = Daemon::builder()
        .socket(socket)
        .idle_timeout(Duration::from_millis(idle_ms))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.iter().map(u8::to_ascii_uppercase).collect()));

    if let Some(pid) = pid {
        builder = builder.pid_file(pid);
    }

    builder.build()?.run()?;
    Ok(())
}
