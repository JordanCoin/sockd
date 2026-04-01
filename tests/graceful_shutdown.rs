use sockd::{AutoStart, Client, Daemon};
use std::thread;
use std::time::{Duration, Instant};
use tempfile::tempdir;

#[test]
fn client_shutdown_gracefully_stops_running_daemon() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("shutdown.sock");
    let pid_path = temp.path().join("shutdown.pid");

    let daemon = Daemon::builder()
        .socket(&socket_path)
        .pid_file(&pid_path)
        .idle_timeout(Duration::from_secs(5))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.to_vec()))
        .build()
        .unwrap();

    let handle = thread::spawn(move || daemon.run());

    let client = Client::new(&socket_path)
        .with_pid_file(&pid_path)
        .with_shutdown_timeout(Duration::from_secs(2));

    wait_until_ready(&client);

    let stopped = client.shutdown().unwrap();
    assert!(stopped);

    let daemon_result = handle.join().unwrap();
    assert!(daemon_result.is_ok(), "{daemon_result:?}");
    assert!(!socket_path.exists());
    assert!(!pid_path.exists());
}

#[test]
fn shutdown_does_not_auto_start_when_nothing_is_running() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("no-daemon.sock");
    let pid_path = temp.path().join("no-daemon.pid");
    let bin = env!("CARGO_BIN_EXE_sockd_test_daemon");

    let client = Client::new(&socket_path)
        .with_pid_file(&pid_path)
        .with_auto_start_config(AutoStart::new(bin).args([
            "--socket",
            socket_path.to_str().unwrap(),
            "--pid",
            pid_path.to_str().unwrap(),
        ]));

    let stopped = client.shutdown().unwrap();
    assert!(!stopped);
    assert!(!socket_path.exists());
    assert!(!pid_path.exists());
}

fn wait_until_ready(client: &Client) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline {
        if client.ping().is_ok() {
            return;
        }
        thread::sleep(Duration::from_millis(10));
    }

    panic!(
        "daemon did not become ready in time: socket_exists={}",
        client.socket_path().exists()
    );
}
