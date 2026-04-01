use sockd::{AutoStart, Client};
use std::thread;
use std::time::{Duration, Instant};
use tempfile::tempdir;

#[test]
fn client_auto_starts_daemon_and_waits_for_idle_shutdown() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("autostart.sock");
    let pid_path = temp.path().join("autostart.pid");
    let bin = env!("CARGO_BIN_EXE_sockd_test_daemon");

    let client = Client::new(&socket_path)
        .with_pid_file(&pid_path)
        .with_auto_start_config(
            AutoStart::new(bin)
                .args([
                    "--socket",
                    socket_path.to_str().unwrap(),
                    "--pid",
                    pid_path.to_str().unwrap(),
                    "--idle-ms",
                    "250",
                ])
                .ready_timeout(Duration::from_secs(2)),
        );

    let response = client.request(b"auto start path").unwrap();
    assert_eq!(response, b"AUTO START PATH");
    assert!(socket_path.exists());
    assert!(pid_path.exists());

    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline {
        if !socket_path.exists() && !pid_path.exists() {
            return;
        }
        thread::sleep(Duration::from_millis(20));
    }

    panic!(
        "daemon did not idle out in time: socket_exists={}, pid_exists={}",
        socket_path.exists(),
        pid_path.exists()
    );
}
