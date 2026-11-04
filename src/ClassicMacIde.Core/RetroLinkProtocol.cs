using System.Buffers.Binary;

namespace ClassicMacIde.Core;

public enum RetroLinkMessageType : byte
{
    Hello = 0x01,
    HelloAck = 0x02,
    RoleRequest = 0x03,
    RoleState = 0x04,
    Heartbeat = 0x05,
    ListRequest = 0x10,
    ListPage = 0x11,
    MakeDirectory = 0x12,
    Rename = 0x13,
    Delete = 0x14,
    OperationResult = 0x15,
    CopyPrepare = 0x20,
    TransferReady = 0x21,
    TransferComplete = 0x22,
    TransferCancel = 0x23,
    CopyRequest = 0x24,
    CopyRequestResult = 0x25,
    Error = 0x7F,
}

public readonly record struct RetroLinkFrame(
    byte MessageType,
    ReadOnlyMemory<byte> SessionId,
    uint RequestId,
    ReadOnlyMemory<byte> Payload);

public static class RetroLinkProtocol
{
    public const byte Version = 1;
    public const int SessionIdLength = 16;
    public const int HeaderSize = 30;
    public const int CrcSize = 2;
    public const int MaximumPayloadSize = 1024;
    public const int MaximumFrameSize = HeaderSize + MaximumPayloadSize + CrcSize;

    internal static ReadOnlySpan<byte> Magic => "RLNK"u8;

    public static byte[] Encode(RetroLinkFrame frame)
    {
        if (frame.SessionId.Length != SessionIdLength)
        {
            throw new ArgumentException(
                $"A session ID must contain {SessionIdLength} bytes.",
                nameof(frame));
        }

        if (frame.Payload.Length > MaximumPayloadSize)
        {
            throw new ArgumentOutOfRangeException(
                nameof(frame),
                $"Payloads cannot exceed {MaximumPayloadSize} bytes.");
        }

        var frameLength = HeaderSize + frame.Payload.Length + CrcSize;
        var bytes = new byte[frameLength];
        var header = bytes.AsSpan(0, HeaderSize);
        Magic.CopyTo(header);
        header[4] = Version;
        header[5] = frame.MessageType;
        header[6] = 0;
        header[7] = 0;
        frame.SessionId.Span.CopyTo(header[8..24]);
        BinaryPrimitives.WriteUInt32BigEndian(header[24..28], frame.RequestId);
        BinaryPrimitives.WriteUInt16BigEndian(
            header[28..30],
            checked((ushort)frame.Payload.Length));
        frame.Payload.Span.CopyTo(bytes.AsSpan(HeaderSize, frame.Payload.Length));

        var crc = CalculateCrc16(bytes.AsSpan(0, frameLength - CrcSize));
        BinaryPrimitives.WriteUInt16BigEndian(bytes.AsSpan(frameLength - CrcSize), crc);
        return bytes;
    }

    internal static ushort CalculateCrc16(ReadOnlySpan<byte> data)
    {
        ushort crc = 0xFFFF;
        foreach (var value in data)
        {
            crc ^= (ushort)(value << 8);
            for (var bit = 0; bit < 8; bit++)
            {
                crc = (crc & 0x8000) != 0
                    ? (ushort)((crc << 1) ^ 0x1021)
                    : (ushort)(crc << 1);
            }
        }

        return crc;
    }
}

public sealed class RetroLinkFrameDecoder
{
    private readonly byte[] _buffer = new byte[RetroLinkProtocol.MaximumFrameSize];
    private int _length;

    public IReadOnlyList<RetroLinkFrame> Append(ReadOnlySpan<byte> data)
    {
        var frames = new List<RetroLinkFrame>();
        foreach (var value in data)
        {
            if (_length == _buffer.Length)
            {
                DropPrefix(1);
            }

            _buffer[_length++] = value;
            DecodeAvailable(frames);
        }

        return frames;
    }

    public void Reset() => _length = 0;

    private void DecodeAvailable(List<RetroLinkFrame> frames)
    {
        while (_length >= RetroLinkProtocol.Magic.Length)
        {
            var magicOffset = FindMagic();
            if (magicOffset < 0)
            {
                DropPrefix(Math.Max(0, _length - (RetroLinkProtocol.Magic.Length - 1)));
                return;
            }

            if (magicOffset > 0)
            {
                DropPrefix(magicOffset);
            }

            if (_length < RetroLinkProtocol.HeaderSize)
            {
                return;
            }

            var header = _buffer.AsSpan(0, RetroLinkProtocol.HeaderSize);
            var payloadLength = BinaryPrimitives.ReadUInt16BigEndian(header[28..30]);
            if (header[4] != RetroLinkProtocol.Version ||
                header[6] != 0 ||
                header[7] != 0 ||
                payloadLength > RetroLinkProtocol.MaximumPayloadSize)
            {
                DropPrefix(1);
                continue;
            }

            var frameLength =
                RetroLinkProtocol.HeaderSize + payloadLength + RetroLinkProtocol.CrcSize;
            if (_length < frameLength)
            {
                return;
            }

            var expectedCrc = BinaryPrimitives.ReadUInt16BigEndian(
                _buffer.AsSpan(frameLength - RetroLinkProtocol.CrcSize));
            var actualCrc = RetroLinkProtocol.CalculateCrc16(
                _buffer.AsSpan(0, frameLength - RetroLinkProtocol.CrcSize));
            if (expectedCrc != actualCrc)
            {
                DropPrefix(1);
                continue;
            }

            var sessionId = _buffer.AsSpan(8, RetroLinkProtocol.SessionIdLength).ToArray();
            var requestId = BinaryPrimitives.ReadUInt32BigEndian(header[24..28]);
            var payload = _buffer.AsSpan(
                RetroLinkProtocol.HeaderSize,
                payloadLength).ToArray();
            frames.Add(new RetroLinkFrame(header[5], sessionId, requestId, payload));
            DropPrefix(frameLength);
        }
    }

    private int FindMagic()
    {
        for (var index = 0; index <= _length - RetroLinkProtocol.Magic.Length; index++)
        {
            if (_buffer.AsSpan(index, RetroLinkProtocol.Magic.Length)
                .SequenceEqual(RetroLinkProtocol.Magic))
            {
                return index;
            }
        }

        return -1;
    }

    private void DropPrefix(int count)
    {
        if (count == 0)
        {
            return;
        }

        _length -= count;
        if (_length > 0)
        {
            Array.Copy(_buffer, count, _buffer, 0, _length);
        }
    }
}
