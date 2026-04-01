use sockd::{Client, Daemon};
use std::fs;
use std::os::unix::net::UnixListener;
use std::process::Command;
use std::thread;
use std::time::{Duration, Instant};
use tempfile::tempdir;

#[test]
fn daemon_reclaims_dead_pid_file() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("dead-pid.sock");
    let pid_path = temp.path().join("dead.pid");

    let dead_pid = spawn_dead_pid();
    fs::write(&pid_path, format!("{dead_pid}\n")).unwrap();

    let daemon = Daemon::builder()
        .socket(&socket_path)
        .pid_file(&pid_path)
        .idle_timeout(Duration::from_millis(200))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.to_vec()))
        .build()
        .unwrap();

    let handle = thread::spawn(move || daemon.run());

    wait_until_ready(&socket_path, &pid_path);
    let response = Client::new(&socket_path)
        .with_pid_file(&pid_path)
        .request(b"dead pid recovered")
        .unwrap();
    assert_eq!(response, b"dead pid recovered");

    let daemon_result = handle.join().unwrap();
    assert!(daemon_result.is_ok(), "{daemon_result:?}");
}

#[test]
fn daemon_reclaims_malformed_pid_file() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("bad-pid.sock");
    let pid_path = temp.path().join("bad.pid");
    fs::write(&pid_path, "not-a-pid\n").unwrap();

    let daemon = Daemon::builder()
        .socket(&socket_path)
        .pid_file(&pid_path)
        .idle_timeout(Duration::from_millis(200))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.iter().map(u8::to_ascii_uppercase).collect()))
        .build()
        .unwrap();

    let handle = thread::spawn(move || daemon.run());

    wait_until_ready(&socket_path, &pid_path);
    let response = Client::new(&socket_path)
        .with_pid_file(&pid_path)
        .request(b"bad pid recovered")
        .unwrap();
    assert_eq!(response, b"BAD PID RECOVERED");

    let daemon_result = handle.join().unwrap();
    assert!(daemon_result.is_ok(), "{daemon_result:?}");
}

#[test]
fn daemon_reclaims_stale_socket_file() {
    let temp = tempdir().unwrap();
    let socket_path = temp.path().join("stale.sock");
    let pid_path = temp.path().join("stale.pid");

    {
        let listener = UnixListener::bind(&socket_path).unwrap();
        drop(listener);
    }
    assert!(socket_path.exists());

    let daemon = Daemon::builder()
        .socket(&socket_path)
        .pid_file(&pid_path)
        .idle_timeout(Duration::from_millis(200))
        .poll_interval(Duration::from_millis(20))
        .on_request(|_, req| Ok(req.to_vec()))
        .build()
        .unwrap();

    let handle = thread::spawn(move || daemon.run());

    wait_until_ready(&socket_path, &pid_path);
    let response = Client::new(&socket_path)
        .with_pid_file(&pid_path)
        .request(b"stale socket recovered")
        .unwrap();
    assert_eq!(response, b"stale socket recovered");

    let daemon_result = handle.join().unwrap();
    assert!(daemon_result.is_ok(), "{daemon_result:?}");
}

fn wait_until_ready(socket_path: &std::path::Path, pid_path: &std::path::Path) {
    let client = Client::new(socket_path).with_pid_file(pid_path);
    let deadline = Instant::now() + Duration::from_secs(1);
    while Instant::now() < deadline {
        if client.ping().is_ok() {
            return;
        }
        thread::sleep(Duration::from_millis(10));
    }

    panic!(
        "daemon did not become ready in time: socket_exists={}, pid_exists={}",
        socket_path.exists(),
        pid_path.exists()
    );
}

fn spawn_dead_pid() -> u32 {
    let child = Command::new("sh").arg("-c").arg("exit 0").spawn().unwrap();
    let pid = child.id();
    let output = child.wait_with_output().unwrap();
    assert!(output.status.success());
    pid
}
