use crate::error::{Error, Result};
use std::io::{Read, Write};

pub const MAGIC: u32 = 0x534f434b;
pub const VERSION: u8 = 1;
pub const HEADER_LEN: usize = 12;
pub const DEFAULT_MAX_PAYLOAD_LEN: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum FrameKind {
    Request = 1,
    Response = 2,
    Ping = 3,
    Pong = 4,
    Error = 5,
    Shutdown = 6,
    ShutdownAck = 7,
}

impl FrameKind {
    pub(crate) fn label(self) -> &'static str {
        match self {
            Self::Request => "request",
            Self::Response => "response",
            Self::Ping => "ping",
            Self::Pong => "pong",
            Self::Error => "error",
            Self::Shutdown => "shutdown",
            Self::ShutdownAck => "shutdown-ack",
        }
    }
}

impl TryFrom<u8> for FrameKind {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(Self::Request),
            2 => Ok(Self::Response),
            3 => Ok(Self::Ping),
            4 => Ok(Self::Pong),
            5 => Ok(Self::Error),
            6 => Ok(Self::Shutdown),
            7 => Ok(Self::ShutdownAck),
            other => Err(Error::UnknownFrameKind(other)),
        }
    }
}

#[derive(Debug, Eq, PartialEq)]
pub(crate) struct Frame {
    pub(crate) kind: FrameKind,
    pub(crate) flags: u16,
    pub(crate) payload: Vec<u8>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Header {
    kind: FrameKind,
    flags: u16,
    payload_len: u32,
}

impl Header {
    fn encode(self) -> [u8; HEADER_LEN] {
        let mut buf = [0u8; HEADER_LEN];
        buf[..4].copy_from_slice(&MAGIC.to_be_bytes());
        buf[4] = VERSION;
        buf[5] = self.kind as u8;
        buf[6..8].copy_from_slice(&self.flags.to_be_bytes());
        buf[8..12].copy_from_slice(&self.payload_len.to_be_bytes());
        buf
    }

    fn decode(buf: [u8; HEADER_LEN]) -> Result<Self> {
        let magic = u32::from_be_bytes(buf[..4].try_into().expect("header magic width"));
        if magic != MAGIC {
            return Err(Error::InvalidFrameMagic(magic));
        }

        let version = buf[4];
        if version != VERSION {
            return Err(Error::UnsupportedFrameVersion(version));
        }

        Ok(Self {
            kind: FrameKind::try_from(buf[5])?,
            flags: u16::from_be_bytes(buf[6..8].try_into().expect("header flags width")),
            payload_len: u32::from_be_bytes(
                buf[8..12].try_into().expect("header payload length width"),
            ),
        })
    }
}

pub(crate) fn read_frame<R: Read>(reader: &mut R, max_payload_len: usize) -> Result<Frame> {
    let mut header_buf = [0u8; HEADER_LEN];
    reader.read_exact(&mut header_buf).map_err(Error::Io)?;
    let header = Header::decode(header_buf)?;

    let payload_len = header.payload_len as usize;
    if payload_len > max_payload_len {
        return Err(Error::PayloadTooLarge {
            len: payload_len,
            max: max_payload_len,
        });
    }

    let mut payload = vec![0u8; payload_len];
    reader.read_exact(&mut payload).map_err(Error::Io)?;

    Ok(Frame {
        kind: header.kind,
        flags: header.flags,
        payload,
    })
}

pub(crate) fn write_frame<W: Write>(writer: &mut W, kind: FrameKind, payload: &[u8]) -> Result<()> {
    write_frame_with_flags(writer, kind, 0, payload)
}

pub(crate) fn write_frame_with_flags<W: Write>(
    writer: &mut W,
    kind: FrameKind,
    flags: u16,
    payload: &[u8],
) -> Result<()> {
    if payload.len() > u32::MAX as usize {
        return Err(Error::PayloadTooLarge {
            len: payload.len(),
            max: u32::MAX as usize,
        });
    }

    let header = Header {
        kind,
        flags,
        payload_len: payload.len() as u32,
    };
    writer.write_all(&header.encode()).map_err(Error::Io)?;
    writer.write_all(payload).map_err(Error::Io)?;
    writer.flush().map_err(Error::Io)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cmp;
    use std::io;

    struct PartialReader {
        data: Vec<u8>,
        offset: usize,
        chunk: usize,
    }

    impl Read for PartialReader {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if self.offset >= self.data.len() {
                return Ok(0);
            }

            let end = cmp::min(self.data.len(), self.offset + self.chunk);
            let count = end - self.offset;
            buf[..count].copy_from_slice(&self.data[self.offset..end]);
            self.offset = end;
            Ok(count)
        }
    }

    struct PartialWriter {
        data: Vec<u8>,
        chunk: usize,
    }

    impl Write for PartialWriter {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            let count = cmp::min(buf.len(), self.chunk);
            self.data.extend_from_slice(&buf[..count]);
            Ok(count)
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn frame_round_trips_with_partial_io() {
        let mut writer = PartialWriter {
            data: Vec::new(),
            chunk: 3,
        };
        write_frame(&mut writer, FrameKind::Request, b"hello world").unwrap();

        let mut reader = PartialReader {
            data: writer.data,
            offset: 0,
            chunk: 2,
        };
        let frame = read_frame(&mut reader, DEFAULT_MAX_PAYLOAD_LEN).unwrap();

        assert_eq!(frame.kind, FrameKind::Request);
        assert_eq!(frame.flags, 0);
        assert_eq!(frame.payload, b"hello world");
    }

    #[test]
    fn rejects_invalid_magic() {
        let mut bytes = [0u8; HEADER_LEN];
        bytes[..4].copy_from_slice(&0x12345678u32.to_be_bytes());
        bytes[4] = VERSION;
        bytes[5] = FrameKind::Ping as u8;

        let err = Header::decode(bytes).unwrap_err();
        assert!(matches!(err, Error::InvalidFrameMagic(0x12345678)));
    }
}
