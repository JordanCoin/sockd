use sockd::{Client, Daemon};
use std::thread;
use std::time::{Duration, Instant};
use tempfile::tempdir;

#[test]
fn daemon_round_trips_and_cleans_up_after_idle_timeout() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("sockd.sock");
    let pid_path = temp.path().join("sockd.pid");

    let daemon = Daemon::builder()
        .socket(&socket_path)
        .pid_file(&pid_path)
        .idle_timeout(Duration::from_millis(200))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.iter().map(u8::to_ascii_uppercase).collect()))
        .build()
        .unwrap();

    let handle = thread::spawn(move || daemon.run());

    let client = Client::new(&socket_path).with_pid_file(&pid_path);
    let deadline = Instant::now() + Duration::from_secs(1);
    while Instant::now() < deadline {
        if client.ping().is_ok() {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }

    let first = client.request(b"hello daemon").unwrap();
    assert_eq!(first, b"HELLO DAEMON");

    let second = client.request(b"hello again").unwrap();
    assert_eq!(second, b"HELLO AGAIN");

    let daemon_result = handle.join().unwrap();
    assert!(daemon_result.is_ok(), "{daemon_result:?}");
    assert!(!socket_path.exists());
    assert!(!pid_path.exists());
}
