using System.Buffers.Binary;
using System.IO;
using ClassicMacIde.Core;

namespace ClassicMacIde.Core.Tests;

public class RetroLinkSessionTests
{
    [Fact]
    public async Task ConnectNegotiatesVersionCapabilitiesAndRoots()
    {
        var hostEndpointId = Enumerable.Range(0, RetroLinkProtocol.SessionIdLength)
            .Select(value => (byte)value)
            .ToArray();
        var stream = new HandshakeStream();
        var session = new RetroLinkSession(
            stream,
            hostEndpointId,
            responseTimeout: TimeSpan.FromSeconds(1));

        var peer = await session.ConnectAsync(9600);

        Assert.Equal(RetroLinkConnectionState.Connected, session.State);
        Assert.Equal(RetroLinkProtocol.Version, peer.ProtocolVersion);
        Assert.Equal((byte)7, peer.SystemMajor);
        Assert.Equal((ushort)0x004C, peer.MachineType);
        Assert.Equal((uint)512 * 1024, peer.MaximumTransferSize);
        Assert.Equal(
            new RetroLinkRootDescriptor(42, "Mac HD", IsReadOnly: false),
            Assert.Single(peer.Roots));
        Assert.Equal(
            RetroLinkCapabilities.List | RetroLinkCapabilities.RoleHandover,
            peer.Capabilities);

        var hello = Assert.Single(stream.ReceivedFrames);
        Assert.Equal((byte)RetroLinkMessageType.Hello, hello.MessageType);
        Assert.Equal(0u, hello.RequestId);
        Assert.Equal(29, hello.Payload.Length);
        Assert.Equal(RetroLinkProtocol.Version, hello.Payload.Span[0]);
        Assert.Equal(RetroLinkProtocol.Version, hello.Payload.Span[1]);
        Assert.Equal(hostEndpointId, hello.Payload.Span.Slice(5, 16).ToArray());
        Assert.Equal(9600u, BinaryPrimitives.ReadUInt32BigEndian(hello.Payload.Span[25..29]));
        Assert.Equal(1u, session.NextRequestId());
        Assert.Equal(2u, session.NextRequestId());
    }

    [Fact]
    public async Task ConnectIgnoresAHelloAckFromAnotherSession()
    {
        var stream = new HandshakeStream(sendStaleSessionFirst: true);
        var session = new RetroLinkSession(
            stream,
            new byte[RetroLinkProtocol.SessionIdLength],
            responseTimeout: TimeSpan.FromSeconds(1));

        var peer = await session.ConnectAsync(9600);

        Assert.Equal(RetroLinkProtocol.Version, peer.ProtocolVersion);
        Assert.Equal(RetroLinkConnectionState.Connected, session.State);
    }

    [Fact]
    public async Task ConnectTimesOutWhenStreamIgnoresReadCancellation()
    {
        var stream = new CancellationIgnoringStream();
        var session = new RetroLinkSession(
            stream,
            new byte[RetroLinkProtocol.SessionIdLength],
            maximumAttempts: 1,
            responseTimeout: TimeSpan.FromMilliseconds(50));
        var connectTask = session.ConnectAsync(9600);

        var timedOutPromptly = await Task.WhenAny(
            connectTask,
            Task.Delay(TimeSpan.FromSeconds(1))) == connectTask;
        stream.CompleteRead();

        Assert.True(timedOutPromptly);
        await Assert.ThrowsAsync<TimeoutException>(() => connectTask);
        Assert.Equal(RetroLinkConnectionState.Disconnected, session.State);
    }

    [Fact]
    public async Task SendingFramesUsesSessionIdAndRequiresConnection()
    {
        var stream = new HandshakeStream();
        var session = new RetroLinkSession(
            stream,
            new byte[RetroLinkProtocol.SessionIdLength],
            responseTimeout: TimeSpan.FromSeconds(1));

        await Assert.ThrowsAsync<InvalidOperationException>(
            () => session.SendAsync(
                (byte)RetroLinkMessageType.Heartbeat,
                1,
                ReadOnlyMemory<byte>.Empty));
        await session.ConnectAsync(9600);
        var requestId = session.NextRequestId();

        await session.SendAsync(
            (byte)RetroLinkMessageType.RoleRequest,
            requestId,
            new byte[] { 0xAA });

        var sentFrame = Assert.Single(stream.ReceivedFrames.Skip(1));
        Assert.Equal((byte)RetroLinkMessageType.RoleRequest, sentFrame.MessageType);
        Assert.Equal(requestId, sentFrame.RequestId);
        Assert.Equal(session.SessionId.ToArray(), sentFrame.SessionId.ToArray());
    }

    private sealed class HandshakeStream(bool sendStaleSessionFirst = false) : Stream
    {
        private readonly Queue<byte[]> _readChunks = new();
        private readonly RetroLinkFrameDecoder _decoder = new();
        private readonly List<RetroLinkFrame> _receivedFrames = [];
        private byte[]? _currentReadChunk;
        private int _currentReadOffset;

        public IReadOnlyList<RetroLinkFrame> ReceivedFrames => _receivedFrames;

        public override bool CanRead => true;

        public override bool CanSeek => false;

        public override bool CanWrite => true;

        public override long Length => throw new NotSupportedException();

        public override long Position
        {
            get => throw new NotSupportedException();
            set => throw new NotSupportedException();
        }

        public override void Flush()
        {
        }

        public override int Read(byte[] buffer, int offset, int count) =>
            ReadAsync(buffer.AsMemory(offset, count)).GetAwaiter().GetResult();

        public override ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_currentReadChunk is null)
            {
                if (_readChunks.Count == 0)
                {
                    throw new InvalidOperationException(
                        "The scripted peer has no pending response.");
                }

                _currentReadChunk = _readChunks.Dequeue();
                _currentReadOffset = 0;
            }

            var bytesToCopy = Math.Min(
                Math.Min(buffer.Length, _currentReadChunk.Length - _currentReadOffset),
                11);
            _currentReadChunk.AsSpan(_currentReadOffset, bytesToCopy).CopyTo(buffer.Span);
            _currentReadOffset += bytesToCopy;
            if (_currentReadOffset == _currentReadChunk.Length)
            {
                _currentReadChunk = null;
                _currentReadOffset = 0;
            }

            return ValueTask.FromResult(bytesToCopy);
        }

        public override void Write(byte[] buffer, int offset, int count) =>
            WriteAsync(buffer.AsMemory(offset, count)).GetAwaiter().GetResult();

        public override ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            foreach (var frame in _decoder.Append(buffer.Span))
            {
                _receivedFrames.Add(frame);
                if (frame.MessageType == (byte)RetroLinkMessageType.Hello)
                {
                    if (sendStaleSessionFirst)
                    {
                        QueueResponse(
                            RetroLinkProtocol.Encode(CreateHelloAck(
                                Enumerable.Repeat(
                                        (byte)0xFF,
                                        RetroLinkProtocol.SessionIdLength)
                                    .ToArray(),
                                frame.SessionId.ToArray())));
                    }

                    QueueResponse(RetroLinkProtocol.Encode(
                        CreateHelloAck(frame.SessionId, frame.SessionId.ToArray())));
                }
            }

            return ValueTask.CompletedTask;
        }

        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();

        public override void SetLength(long value) => throw new NotSupportedException();

        private static RetroLinkFrame CreateHelloAck(
            ReadOnlyMemory<byte> sessionId,
            byte[] endpointId)
        {
            const string rootName = "Mac HD";
            var payload = new byte[49];
            payload[0] = RetroLinkProtocol.Version;
            payload[1] = 2;
            payload[2] = 1;
            payload[3] = 0;
            payload[4] = 7;
            payload[5] = 5;
            BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(6, 2), 0x004C);
            endpointId.CopyTo(payload, 8);
            BinaryPrimitives.WriteUInt32BigEndian(
                payload.AsSpan(24, 4),
                (uint)(RetroLinkCapabilities.List | RetroLinkCapabilities.RoleHandover));
            BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(28, 4), 9600);
            BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(32, 4), 512 * 1024);
            payload[36] = 1;
            BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(37, 4), 42);
            payload[41] = (byte)rootName.Length;
            System.Text.Encoding.UTF8.GetBytes(rootName).CopyTo(payload, 42);
            payload[48] = 0;
            return new RetroLinkFrame(
                (byte)RetroLinkMessageType.HelloAck,
                sessionId,
                0,
                payload);
        }

        private void QueueResponse(byte[] response) => _readChunks.Enqueue(response);
    }

    private sealed class CancellationIgnoringStream : Stream
    {
        private readonly TaskCompletionSource<int> _readCompletion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override bool CanRead => true;

        public override bool CanSeek => false;

        public override bool CanWrite => true;

        public override long Length => throw new NotSupportedException();

        public override long Position
        {
            get => throw new NotSupportedException();
            set => throw new NotSupportedException();
        }

        public override void Flush()
        {
        }

        public override int Read(byte[] buffer, int offset, int count) =>
            throw new NotSupportedException();

        public override ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken = default) =>
            new(_readCompletion.Task);

        public override void Write(byte[] buffer, int offset, int count) =>
            throw new NotSupportedException();

        public override ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;

        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();

        public override void SetLength(long value) =>
            throw new NotSupportedException();

        public void CompleteRead() => _readCompletion.TrySetResult(0);
    }
}
