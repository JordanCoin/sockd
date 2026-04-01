import Foundation
import CSockd

let defaultSocket = "/tmp/sockd-example.sock"
let defaultPid = "/tmp/sockd-example.pid"

func stringAndFree(_ ptr: UnsafeMutablePointer<CChar>?) -> String {
    guard let ptr else { return "sockd operation failed" }
    let value = String(cString: ptr)
    sockd_string_free(ptr)
    return value
}

func check(_ rc: Int32, _ error: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>) throws {
    if rc == 0 { return }
    throw NSError(
        domain: "sockd",
        code: Int(rc),
        userInfo: [NSLocalizedDescriptionKey: stringAndFree(error.pointee)]
    )
}

let args = Array(CommandLine.arguments.dropFirst())
var socketPath = defaultSocket
var pidPath = defaultPid
var autoStart: String?
var shutdown = false
var messageParts: [String] = []

var index = 0
while index < args.count {
    let arg = args[index]
    switch arg {
    case "--socket":
        index += 1
        socketPath = args[index]
    case "--pid":
        index += 1
        pidPath = args[index]
    case "--auto-start":
        index += 1
        autoStart = args[index]
    case "--shutdown":
        shutdown = true
    default:
        messageParts.append(arg)
    }
    index += 1
}

var errorPtr: UnsafeMutablePointer<CChar>? = nil
let config = sockd_client_config_new(socketPath)
guard let config else {
    fputs("failed to allocate sockd client config\n", stderr)
    exit(1)
}
defer { sockd_client_config_free(config) }

do {
    try check(sockd_client_config_set_pid_file(config, pidPath, &errorPtr), &errorPtr)

    if let autoStart {
        try check(
            sockd_client_config_set_auto_start(config, autoStart, 0, nil, &errorPtr),
            &errorPtr
        )
    }

    if shutdown {
        var stopped = false
        try check(sockd_client_shutdown(config, &stopped, &errorPtr), &errorPtr)
        print(stopped ? "true" : "false")
        exit(0)
    }

    let message = messageParts.isEmpty ? "hello from swift" : messageParts.joined(separator: " ")
    let bytes = Array(message.utf8)
    var responsePtr: UnsafeMutablePointer<UInt8>? = nil
    var responseLen: Int = 0

    try bytes.withUnsafeBufferPointer { buffer in
        try check(
            sockd_client_request(
                config,
                buffer.baseAddress,
                buffer.count,
                &responsePtr,
                &responseLen,
                &errorPtr
            ),
            &errorPtr
        )
    }

    if let responsePtr {
        let data = Data(bytes: responsePtr, count: responseLen)
        if let text = String(data: data, encoding: .utf8) {
            print(text)
        }
        sockd_buffer_free(responsePtr, responseLen)
    }
} catch {
    fputs("\(error)\n", stderr)
    exit(1)
}
