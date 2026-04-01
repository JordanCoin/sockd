# Wire Protocol

sockd uses a simple length-prefixed binary framing over Unix domain sockets.
Each connection carries exactly one request/response exchange, then closes.

## Frame Format

```
Offset  Size  Field          Description
─────────────────────────────────────────────────
0       4     magic          0x534F434B ("SOCK", big-endian)
4       1     version        Protocol version (currently 1)
5       1     kind           Frame kind (see below)
6       2     flags          Reserved, must be 0
8       4     payload_len    Payload length in bytes (big-endian uint32)
12      N     payload        Raw bytes (your data)
```

**Total header: 12 bytes.** Maximum payload: configurable, default 16 MB.

## Frame Kinds

| Value | Name         | Direction        | Payload              |
|-------|-------------|------------------|----------------------|
| 1     | Request     | client → daemon  | Your request bytes   |
| 2     | Response    | daemon → client  | Your response bytes  |
| 3     | Ping        | client → daemon  | Empty                |
| 4     | Pong        | daemon → client  | Empty                |
| 5     | Error       | daemon → client  | UTF-8 error message  |
| 6     | Shutdown    | client → daemon  | Empty                |
| 7     | ShutdownAck | daemon → client  | Empty                |

## Connection Flow

### Request/Response

```
Client                          Daemon
  │                                │
  ├─── connect ──────────────────► │
  ├─── Request(payload) ────────►  │
  │                                ├── on_request(state, payload)
  │  ◄── Response(result) ─────── │
  ├─── close ──────────────────►   │
```

### Health Check

```
Client                          Daemon
  │                                │
  ├─── connect ──────────────────► │
  ├─── Ping() ──────────────────►  │
  │  ◄── Pong() ────────────────── │
  ├─── close ──────────────────►   │
```

### Graceful Shutdown

```
Client                          Daemon
  │                                │
  ├─── connect ──────────────────► │
  ├─── Shutdown() ──────────────►  │
  │  ◄── ShutdownAck() ─────────  │
  ├─── close                       ├── on_stop(state)
  │                                ├── remove socket
  │                                ├── remove PID file
  │                                └── exit
```

### Error Response

If `on_request` returns an error, or the client sends an unexpected frame:

```
Client                          Daemon
  │                                │
  ├─── Request(payload) ────────►  │
  │                                ├── on_request fails
  │  ◄── Error("message") ──────  │
  │                                │ (daemon continues running)
```

## Implementing a Client

To write a sockd client in any language:

1. **Connect** to the Unix domain socket
2. **Write** a 12-byte header + payload:
   - Bytes 0-3: `0x534F434B` (big-endian)
   - Byte 4: `0x01` (version)
   - Byte 5: `0x01` (Request kind)
   - Bytes 6-7: `0x0000` (flags)
   - Bytes 8-11: payload length (big-endian uint32)
   - Bytes 12+: your payload
3. **Read** 12 bytes (response header)
4. **Verify** magic and version
5. **Read** `payload_len` more bytes (the response)
6. **Close** the connection

### Python (raw socket, no FFI)

```python
import socket, struct

MAGIC = 0x534F434B
VERSION = 1
REQUEST = 1

def sockd_request(socket_path, payload):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(socket_path)

    # Write request frame
    header = struct.pack('>IBBHI', MAGIC, VERSION, REQUEST, 0, len(payload))
    sock.sendall(header + payload)

    # Read response header
    resp_header = sock.recv(12)
    magic, ver, kind, flags, resp_len = struct.unpack('>IBBHI', resp_header)
    assert magic == MAGIC

    # Read response payload
    data = b''
    while len(data) < resp_len:
        data += sock.recv(resp_len - len(data))

    sock.close()
    return data
```

### Go (raw socket)

```go
func sockdRequest(socketPath string, payload []byte) ([]byte, error) {
    conn, err := net.Dial("unix", socketPath)
    if err != nil { return nil, err }
    defer conn.Close()

    // Write header
    header := make([]byte, 12)
    binary.BigEndian.PutUint32(header[0:4], 0x534F434B) // magic
    header[4] = 1 // version
    header[5] = 1 // Request kind
    binary.BigEndian.PutUint32(header[8:12], uint32(len(payload)))
    conn.Write(header)
    conn.Write(payload)

    // Read response header
    resp := make([]byte, 12)
    io.ReadFull(conn, resp)
    respLen := binary.BigEndian.Uint32(resp[8:12])

    // Read response payload
    body := make([]byte, respLen)
    io.ReadFull(conn, body)
    return body, nil
}
```

## Versioning

The protocol version is embedded in every frame header (byte 4). If the
daemon receives a frame with an unsupported version, it returns an `Error`
frame. This allows future protocol changes without breaking existing clients.

The flags field (bytes 6-7) is reserved for future use. Clients must set it
to 0. Daemons must ignore unknown flags.
