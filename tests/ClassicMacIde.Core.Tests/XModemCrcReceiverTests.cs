using System.Buffers.Binary;
using System.IO;
using System.Threading.Channels;
using ClassicMacIde.Core;

namespace ClassicMacIde.Core.Tests;

public class XModemCrcReceiverTests
{
    [Fact]
    public async Task ReceiveNegotiatesWritesExactLengthAndAcknowledgesEot()
    {
        var data = Enumerable.Range(0, 129).Select(value => (byte)value).ToArray();
        var serial = new TestXModemStream(
        [
            .. CreatePacket(1, data.AsSpan(0, 128)),
            .. CreatePacket(2, data.AsSpan(128)),
            0x04,
        ]);
        using var destination = new MemoryStream();
        var receiver = new XModemCrcReceiver(
            responseTimeout: TimeSpan.FromMilliseconds(100));

        var result = await receiver.ReceiveAsync(serial, destination, data.Length);

        Assert.Equal(data.Length, result.Length);
        Assert.Equal(Crc32IsoHdlc.Compute(data), result.Crc32);
        Assert.Equal(data, destination.ToArray());
        Assert.Equal(new byte[] { 0x43, 0x06, 0x06, 0x06 }, serial.WrittenBytes);
    }

    [Fact]
    public async Task ReceiveNaksBadCrcThenAcceptsRetransmission()
    {
        var data = new byte[] { 0x41, 0x42, 0x43 };
        var badPacket = CreatePacket(1, data);
        badPacket[^1] ^= 0x01;
        var serial = new TestXModemStream(
        [
            .. badPacket,
            .. CreatePacket(1, data),
            0x04,
        ]);
        using var destination = new MemoryStream();

        var result = await new XModemCrcReceiver(
                responseTimeout: TimeSpan.FromMilliseconds(100))
            .ReceiveAsync(serial, destination, data.Length);

        Assert.Equal(data, destination.ToArray());
        Assert.Equal(Crc32IsoHdlc.Compute(data), result.Crc32);
        Assert.Equal(new byte[] { 0x43, 0x15, 0x06, 0x06 }, serial.WrittenBytes);
    }

    [Fact]
    public async Task ReceiveAcknowledgesDuplicateBlockWithoutWritingItTwice()
    {
        var data = Enumerable.Range(0, 129).Select(value => (byte)value).ToArray();
        var firstBlock = CreatePacket(1, data.AsSpan(0, 128));
        var serial = new TestXModemStream(
        [
            .. firstBlock,
            .. firstBlock,
            .. CreatePacket(2, data.AsSpan(128)),
            0x04,
        ]);
        using var destination = new MemoryStream();

        var result = await new XModemCrcReceiver(
                responseTimeout: TimeSpan.FromMilliseconds(100))
            .ReceiveAsync(serial, destination, data.Length);

        Assert.Equal(data.Length, result.Length);
        Assert.Equal(data, destination.ToArray());
        Assert.Equal(new byte[] { 0x43, 0x06, 0x06, 0x06, 0x06 }, serial.WrittenBytes);
    }

    [Fact]
    public async Task ReceiveRejectsPrematureEotAndDeclaredLengthMismatch()
    {
        var data = new byte[128];
        var serial = new TestXModemStream(
        [
            .. CreatePacket(1, data),
            0x04,
            0x04,
        ]);
        using var destination = new MemoryStream();
        var receiver = new XModemCrcReceiver(
            maximumAttempts: 2,
            responseTimeout: TimeSpan.FromMilliseconds(100));

        await Assert.ThrowsAsync<XModemTransferException>(
            () => receiver.ReceiveAsync(serial, destination, 129));

        Assert.Equal(data, destination.ToArray());
        Assert.Equal(
            new byte[] { 0x43, 0x06, 0x15, 0x18, 0x18 },
            serial.WrittenBytes);
    }

    [Fact]
    public async Task ReceiveHonorsCancellationAndNotifiesSender()
    {
        var serial = new TestXModemStream([]);
        using var destination = new MemoryStream();
        using var cancellation = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(20));

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => new XModemCrcReceiver(
                    responseTimeout: TimeSpan.FromSeconds(1))
                .ReceiveAsync(serial, destination, 1, cancellation.Token));

        Assert.Equal(new byte[] { 0x43, 0x18, 0x18 }, serial.WrittenBytes);
    }

    [Fact]
    public async Task ReceiveZeroLengthSkipsTheXmodemByteStream()
    {
        var serial = new TestXModemStream([]);
        using var destination = new MemoryStream();

        var result = await new XModemCrcReceiver().ReceiveAsync(
            serial,
            destination,
            expectedLength: 0);

        Assert.Equal(0, result.Length);
        Assert.Equal(Crc32IsoHdlc.Compute(ReadOnlySpan<byte>.Empty), result.Crc32);
        Assert.Empty(serial.WrittenBytes);
    }

    private static byte[] CreatePacket(byte blockNumber, ReadOnlySpan<byte> data)
    {
        var packet = new byte[133];
        packet[0] = 0x01;
        packet[1] = blockNumber;
        packet[2] = unchecked((byte)~blockNumber);
        data.CopyTo(packet.AsSpan(3));
        packet.AsSpan(3 + data.Length, 128 - data.Length).Fill(0x1A);
        BinaryPrimitives.WriteUInt16BigEndian(
            packet.AsSpan(131, 2),
            CalculateCrc16Xmodem(packet.AsSpan(3, 128)));
        return packet;
    }

    private static ushort CalculateCrc16Xmodem(ReadOnlySpan<byte> data)
    {
        ushort crc = 0;
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

    private sealed class TestXModemStream : Stream
    {
        private readonly Channel<byte> _input = Channel.CreateUnbounded<byte>();
        private readonly MemoryStream _output = new();

        public TestXModemStream(byte[] input)
        {
            foreach (var value in input)
            {
                _input.Writer.TryWrite(value);
            }
        }

        public byte[] WrittenBytes => _output.ToArray();

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

        public override async ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken = default)
        {
            if (buffer.IsEmpty)
            {
                return 0;
            }

            var value = await _input.Reader.ReadAsync(cancellationToken);
            buffer.Span[0] = value;
            return 1;
        }

        public override void Write(byte[] buffer, int offset, int count) =>
            _output.Write(buffer, offset, count);

        public override ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken = default) =>
            _output.WriteAsync(buffer, cancellationToken);

        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();

        public override void SetLength(long value) =>
            throw new NotSupportedException();
    }
}
