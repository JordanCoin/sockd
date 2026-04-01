use sockd::{AutoStart, Client};
use std::env;
use std::error::Error;
use std::path::PathBuf;

fn main() -> Result<(), Box<dyn Error + Send + Sync + 'static>> {
    let socket_path = env::var("SOCKD_SOCKET")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/tmp/sockd-example.sock"));
    let pid_path = env::var("SOCKD_PID")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/tmp/sockd-example.pid"));

    let mut args = env::args().skip(1);
    let mut shutdown = false;
    let mut auto_start = None::<String>;
    let mut payload = Vec::new();

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--shutdown" => shutdown = true,
            "--auto-start" => auto_start = args.next(),
            other => payload.push(other.to_owned()),
        }
    }

    let mut client = Client::new(socket_path).with_pid_file(pid_path);
    if let Some(program) = auto_start {
        client = client.with_auto_start_config(AutoStart::new(program));
    }

    if shutdown {
        let stopped = client.shutdown()?;
        println!("{stopped}");
        return Ok(());
    }

    let message = if payload.is_empty() {
        "hello from rust".to_owned()
    } else {
        payload.join(" ")
    };
    let response = client.request(message.as_bytes())?;
    println!("{}", String::from_utf8_lossy(&response));
    Ok(())
}
