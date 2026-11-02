using System.Buffers.Binary;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace ClassicMacIde.Core;

[Flags]
public enum RetroLinkCapabilities : uint
{
    None = 0,
    List = 0x00000001,
    MakeDirectory = 0x00000002,
    Rename = 0x00000004,
    Delete = 0x00000008,
    XModemCrc = 0x00000010,
    MacBinaryIi = 0x00000020,
    RoleHandover = 0x00000040,
}

public enum RetroLinkConnectionState
{
    Disconnected,
    Connecting,
    Connected,
}

public sealed record RetroLinkRootDescriptor(
    uint ProviderId,
    string Name,
    bool IsReadOnly);

public sealed record RetroLinkPeerInfo(
    byte ProtocolVersion,
    byte ApplicationMajor,
    byte ApplicationMinor,
    byte ApplicationPatch,
    byte SystemMajor,
    byte SystemMinor,
    ushort MachineType,
    ReadOnlyMemory<byte> EndpointId,
    RetroLinkCapabilities Capabilities,
    uint SupportedBaudRates,
    uint MaximumTransferSize,
    IReadOnlyList<RetroLinkRootDescriptor> Roots);

public sealed class RetroLinkProtocolException(
    ushort errorCode,
    byte rejectedMessageType,
    string reason) : IOException(
        $"The RetroLink peer rejected the request ({errorCode}): {reason}")
{
    public ushort ErrorCode { get; } = errorCode;

    public byte RejectedMessageType { get; } = rejectedMessageType;

    public string Reason { get; } = reason;
}

public sealed class RetroLinkSession
{
    private const byte HostMajorVersion = 1;
    private const int ReadBufferSize = 256;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    private readonly Stream _stream;
    private readonly int _maximumAttempts;
    private readonly TimeSpan _responseTimeout;
    private readonly SemaphoreSlim _connectGate = new(1, 1);
    private readonly SemaphoreSlim _readGate = new(1, 1);
    private readonly SemaphoreSlim _writeGate = new(1, 1);
    private readonly byte[] _readBuffer = new byte[ReadBufferSize];
    private readonly RetroLinkFrameDecoder _decoder = new();
    private readonly Queue<RetroLinkFrame> _pendingFrames = new();
    private byte[]? _sessionId;
    private int _lastRequestId;

    public RetroLinkSession(
        Stream stream,
        ReadOnlyMemory<byte> endpointId,
        int maximumAttempts = 3,
        TimeSpan? responseTimeout = null)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanRead || !stream.CanWrite)
        {
            throw new ArgumentException(
                "The RetroLink session requires a readable and writable stream.",
                nameof(stream));
        }

        if (endpointId.Length != RetroLinkProtocol.SessionIdLength)
        {
            throw new ArgumentException(
                $"An endpoint ID must contain {RetroLinkProtocol.SessionIdLength} bytes.",
                nameof(endpointId));
        }

        if (maximumAttempts <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumAttempts));
        }

        var timeout = responseTimeout ?? TimeSpan.FromSeconds(2);
        if (timeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(responseTimeout));
        }

        _stream = stream;
        EndpointId = endpointId.ToArray();
        _maximumAttempts = maximumAttempts;
        _responseTimeout = timeout;
    }

    public RetroLinkConnectionState State { get; private set; }

    public ReadOnlyMemory<byte> EndpointId { get; }

    public ReadOnlyMemory<byte> SessionId =>
        _sessionId is null ? ReadOnlyMemory<byte>.Empty : _sessionId;

    public RetroLinkPeerInfo? PeerInfo { get; private set; }

    public async Task<RetroLinkPeerInfo> ConnectAsync(
        uint requestedBaudRate,
        RetroLinkCapabilities capabilities =
            RetroLinkCapabilities.List |
            RetroLinkCapabilities.MakeDirectory |
            RetroLinkCapabilities.Rename |
            RetroLinkCapabilities.Delete |
            RetroLinkCapabilities.XModemCrc |
            RetroLinkCapabilities.MacBinaryIi |
            RetroLinkCapabilities.RoleHandover,
        CancellationToken cancellationToken = default)
    {
        await _connectGate.WaitAsync(cancellationToken);
        try
        {
            if (State != RetroLinkConnectionState.Disconnected)
            {
                throw new InvalidOperationException(
                    "The RetroLink session is already connecting or connected.");
            }

            State = RetroLinkConnectionState.Connecting;
            PeerInfo = null;
            _sessionId = RandomNumberGenerator.GetBytes(
                RetroLinkProtocol.SessionIdLength);
            _lastRequestId = 0;
            _decoder.Reset();
            _pendingFrames.Clear();
            var hello = new RetroLinkFrame(
                (byte)RetroLinkMessageType.Hello,
                _sessionId,
                0,
                CreateHelloPayload(requestedBaudRate, capabilities));
            var encodedHello = RetroLinkProtocol.Encode(hello);

            for (var attempt = 1; attempt <= _maximumAttempts; attempt++)
            {
                using var timeoutSource = new CancellationTokenSource(_responseTimeout);
                using var attemptSource = CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken,
                    timeoutSource.Token);

                try
                {
                    await WriteEncodedFrameAsync(encodedHello, attemptSource.Token);
                    while (true)
                    {
                        var response = await ReadFrameCoreAsync(attemptSource.Token);
                        if (response.MessageType == (byte)RetroLinkMessageType.Error)
                        {
                            throw ParsePeerError(response);
                        }

                        if (response.MessageType !=
                            (byte)RetroLinkMessageType.HelloAck)
                        {
                            continue;
                        }

                        PeerInfo = ParseHelloAck(response.Payload.Span);
                        State = RetroLinkConnectionState.Connected;
                        return PeerInfo;
                    }
                }
                catch (OperationCanceledException) when (
                    !cancellationToken.IsCancellationRequested)
                {
                    if (attempt == _maximumAttempts)
                    {
                        throw new TimeoutException(
                            $"The RetroLink peer did not answer HELLO after {_maximumAttempts} attempts.");
                    }
                }
            }

            throw new InvalidOperationException("The RetroLink handshake did not complete.");
        }
        catch
        {
            State = RetroLinkConnectionState.Disconnected;
            PeerInfo = null;
            _sessionId = null;
            throw;
        }
        finally
        {
            _connectGate.Release();
        }
    }

    public uint NextRequestId()
    {
        if (State != RetroLinkConnectionState.Connected)
        {
            throw new InvalidOperationException(
                "A request ID is available only for a connected session.");
        }

        var next = unchecked((uint)Interlocked.Increment(ref _lastRequestId));
        if (next == 0)
        {
            next = unchecked((uint)Interlocked.Increment(ref _lastRequestId));
        }

        return next;
    }

    public async Task SendAsync(
        byte messageType,
        uint requestId,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken = default)
    {
        if (State != RetroLinkConnectionState.Connected || _sessionId is null)
        {
            throw new InvalidOperationException(
                "A RetroLink frame can be sent only after a successful handshake.");
        }

        var encoded = RetroLinkProtocol.Encode(
            new RetroLinkFrame(messageType, _sessionId, requestId, payload));
        await WriteEncodedFrameAsync(encoded, cancellationToken);
    }

    public async Task<RetroLinkFrame> ReadAsync(
        CancellationToken cancellationToken = default)
    {
        if (State != RetroLinkConnectionState.Connected)
        {
            throw new InvalidOperationException(
                "A RetroLink frame can be read only after a successful handshake.");
        }

        return await ReadFrameCoreAsync(cancellationToken);
    }

    public void Disconnect()
    {
        State = RetroLinkConnectionState.Disconnected;
        PeerInfo = null;
        _sessionId = null;
        _decoder.Reset();
        _pendingFrames.Clear();
    }

    private byte[] CreateHelloPayload(
        uint requestedBaudRate,
        RetroLinkCapabilities capabilities)
    {
        var payload = new byte[29];
        payload[0] = RetroLinkProtocol.Version;
        payload[1] = RetroLinkProtocol.Version;
        payload[2] = HostMajorVersion;
        payload[3] = 0;
        payload[4] = 0;
        EndpointId.Span.CopyTo(payload.AsSpan(5, RetroLinkProtocol.SessionIdLength));
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(21, 4), (uint)capabilities);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(25, 4), requestedBaudRate);
        return payload;
    }

    private async Task WriteEncodedFrameAsync(
        ReadOnlyMemory<byte> encoded,
        CancellationToken cancellationToken)
    {
        await _writeGate.WaitAsync(cancellationToken);
        try
        {
            await _stream.WriteAsync(encoded, cancellationToken);
            await _stream.FlushAsync(cancellationToken);
        }
        finally
        {
            _writeGate.Release();
        }
    }

    private async Task<RetroLinkFrame> ReadFrameCoreAsync(
        CancellationToken cancellationToken)
    {
        await _readGate.WaitAsync(cancellationToken);
        try
        {
            while (true)
            {
                while (_pendingFrames.Count > 0)
                {
                    var pending = _pendingFrames.Dequeue();
                    if (_sessionId is not null &&
                        pending.SessionId.Span.SequenceEqual(_sessionId))
                    {
                        return pending;
                    }
                }

                var readOperation = _stream.ReadAsync(_readBuffer, cancellationToken);
                var read = await readOperation.AsTask().WaitAsync(cancellationToken);
                if (read == 0)
                {
                    throw new EndOfStreamException("The RetroLink serial stream was closed.");
                }

                foreach (var frame in _decoder.Append(_readBuffer.AsSpan(0, read)))
                {
                    if (_sessionId is not null &&
                        frame.SessionId.Span.SequenceEqual(_sessionId))
                    {
                        _pendingFrames.Enqueue(frame);
                    }
                }
            }
        }
        finally
        {
            _readGate.Release();
        }
    }

    private static RetroLinkPeerInfo ParseHelloAck(ReadOnlySpan<byte> payload)
    {
        const int fixedLength = 37;
        if (payload.Length < fixedLength)
        {
            throw new InvalidDataException("The HELLO_ACK payload is truncated.");
        }

        var version = payload[0];
        if (version != RetroLinkProtocol.Version)
        {
            throw new InvalidDataException(
                $"The RetroLink peer selected unsupported protocol version {version}.");
        }

        var rootCount = payload[36];
        if (rootCount > 8)
        {
            throw new InvalidDataException("The HELLO_ACK contains too many roots.");
        }

        var roots = new List<RetroLinkRootDescriptor>(rootCount);
        var offset = fixedLength;
        for (var index = 0; index < rootCount; index++)
        {
            if (payload.Length - offset < 5)
            {
                throw new InvalidDataException("A HELLO_ACK root descriptor is truncated.");
            }

            var providerId = BinaryPrimitives.ReadUInt32BigEndian(payload[offset..]);
            var nameLength = payload[offset + 4];
            offset += 5;
            if (nameLength > 64 || payload.Length - offset < nameLength + 1)
            {
                throw new InvalidDataException("A HELLO_ACK root name is invalid.");
            }

            string name;
            try
            {
                name = StrictUtf8.GetString(payload.Slice(offset, nameLength));
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException(
                    "A HELLO_ACK root name is not valid UTF-8.",
                    error);
            }

            offset += nameLength;
            var flags = payload[offset++];
            if ((flags & 0xFE) != 0)
            {
                throw new InvalidDataException("A HELLO_ACK root has reserved flags set.");
            }

            roots.Add(new RetroLinkRootDescriptor(providerId, name, (flags & 1) != 0));
        }

        if (offset != payload.Length)
        {
            throw new InvalidDataException("The HELLO_ACK contains trailing data.");
        }

        return new RetroLinkPeerInfo(
            version,
            payload[1],
            payload[2],
            payload[3],
            payload[4],
            payload[5],
            BinaryPrimitives.ReadUInt16BigEndian(payload[6..8]),
            payload.Slice(8, RetroLinkProtocol.SessionIdLength).ToArray(),
            (RetroLinkCapabilities)BinaryPrimitives.ReadUInt32BigEndian(payload[24..28]),
            BinaryPrimitives.ReadUInt32BigEndian(payload[28..32]),
            BinaryPrimitives.ReadUInt32BigEndian(payload[32..36]),
            roots.AsReadOnly());
    }

    private static RetroLinkProtocolException ParsePeerError(RetroLinkFrame frame)
    {
        var payload = frame.Payload.Span;
        if (payload.Length < 4 || payload[3] > 128 || payload.Length != 4 + payload[3])
        {
            throw new InvalidDataException("The RetroLink ERROR payload is malformed.");
        }

        string reason;
        try
        {
            reason = StrictUtf8.GetString(payload[4..]);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "The RetroLink ERROR text is not valid UTF-8.",
                error);
        }

        return new RetroLinkProtocolException(
            BinaryPrimitives.ReadUInt16BigEndian(payload[0..2]),
            payload[2],
            reason);
    }
}
