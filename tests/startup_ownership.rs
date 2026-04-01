use sockd::{Client, Daemon, Error};
use std::thread;
use std::time::{Duration, Instant};
use tempfile::tempdir;

#[test]
fn second_daemon_refuses_to_claim_existing_pid_and_socket() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("owned.sock");
    let pid_path = temp.path().join("owned.pid");

    let daemon = Daemon::builder()
        .socket(&socket_path)
        .pid_file(&pid_path)
        .idle_timeout(Duration::from_millis(250))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.to_vec()))
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

    let second = Daemon::builder()
        .socket(&socket_path)
        .pid_file(&pid_path)
        .idle_timeout(Duration::from_millis(250))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.to_vec()))
        .build()
        .unwrap();

    let err = second.run().unwrap_err();
    assert!(matches!(
        err,
        Error::DaemonAlreadyRunning { pid: Some(_), .. }
    ));

    let daemon_result = handle.join().unwrap();
    assert!(daemon_result.is_ok(), "{daemon_result:?}");
}
