use sockd::Daemon;
use std::env;
use std::error::Error;
use std::path::PathBuf;
use std::time::Duration;

fn main() -> Result<(), Box<dyn Error + Send + Sync + 'static>> {
    let socket_path = env::var("SOCKD_SOCKET")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/tmp/sockd-example.sock"));
    let pid_path = env::var("SOCKD_PID")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/tmp/sockd-example.pid"));
    let idle_secs = env::var("SOCKD_IDLE_SECS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(300);

    eprintln!(
        "sockd echo daemon listening on {} (pid file: {})",
        socket_path.display(),
        pid_path.display()
    );

    let daemon = Daemon::builder()
        .socket(socket_path)
        .pid_file(pid_path)
        .idle_timeout(Duration::from_secs(idle_secs))
        .poll_interval(Duration::from_millis(50))
        .on_request(|_, req| Ok(req.iter().map(u8::to_ascii_uppercase).collect()))
        .build()?;

    daemon.run()?;
    Ok(())
}
