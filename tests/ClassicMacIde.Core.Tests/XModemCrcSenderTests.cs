using System.Buffers.Binary;
using System.IO;
using System.Text;
using System.Threading.Channels;
using ClassicMacIde.Core;

namespace ClassicMacIde.Core.Tests;

public class XModemCrcSenderTests
{
    [Fact]
    public async Task SendUsesCrc128ByteBlocksPaddingAndRetriesNak()
    {
        var package = Enumerable.Range(0, 129).Select(value => (byte)value).ToArray();
        using var packageStream = new MemoryStream(package, writable: false);
        var serial = new TestXModemStream(naksBeforeAck: 1);
        var sender = new XModemCrcSender(responseTimeout: TimeSpan.FromMilliseconds(100));

        await sender.SendAsync(packageStream, serial, package.Length);

        Assert.Equal(3, serial.Packets.Count);
        var firstAttempt = serial.Packets[0];
        var firstRetry = serial.Packets[1];
        var lastBlock = serial.Packets[2];
        Assert.Equal(firstAttempt, firstRetry);
        AssertPacket(firstAttempt, 1, package.AsSpan(0, 128));
        AssertPacket(lastBlock, 2, package.AsSpan(128, 1));
        Assert.All(
            lastBlock.AsSpan(4, 127).ToArray(),
            value => Assert.Equal((byte)0x1A, value));
        Assert.Equal(1, serial.EndOfTransmissionCount);
    }

    [Fact]
    public async Task SendReportsProgressAfterAcknowledgedBlocks()
    {
        var package = new byte[129];
        using var packageStream = new MemoryStream(package, writable: false);
        var serial = new TestXModemStream();
        var progress = new ProgressCollector();
        var sender = new XModemCrcSender(
            responseTimeout: TimeSpan.FromMilliseconds(100));

        await sender.SendAsync(
            packageStream,
            serial,
            package.Length,
            progress: progress);

        Assert.Equal(new long[] { 0, 128, 129 }, progress.Values);
    }

    [Fact]
    public async Task SendTimesOutAfterMaximumBlockAttempts()
    {
        using var packageStream = new MemoryStream([0x42], writable: false);
        var serial = new TestXModemStream(acknowledgePackets: false);
        var sender = new XModemCrcSender(
            maximumAttempts: 2,
            responseTimeout: TimeSpan.FromMilliseconds(10));

        var error = await Assert.ThrowsAsync<TimeoutException>(
            () => sender.SendAsync(packageStream, serial, 1));

        Assert.Contains("block 1", error.Message);
        Assert.Equal(2, serial.Packets.Count);
        Assert.Equal(0, serial.EndOfTransmissionCount);
    }

    [Fact]
    public async Task SendHonorsCancellationWhileWaitingForCrcRequest()
    {
        using var packageStream = new MemoryStream([0x42], writable: false);
        var serial = new TestXModemStream(sendCrcRequest: false);
        var sender = new XModemCrcSender(
            responseTimeout: TimeSpan.FromSeconds(5));
        using var cancellation = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(20));

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => sender.SendAsync(
                packageStream,
                serial,
                1,
                cancellation.Token));

        Assert.Empty(serial.Packets);
        Assert.Equal(1, serial.CancelSignalCount);
    }

    [Fact]
    public async Task SendAbortsWhenReceiverCancelsBeforeTransfer()
    {
        using var packageStream = new MemoryStream([0x42], writable: false);
        var serial = new TestXModemStream(initialResponse: 0x18);
        var sender = new XModemCrcSender(
            responseTimeout: TimeSpan.FromMilliseconds(100));

        await Assert.ThrowsAsync<XModemTransferException>(
            () => sender.SendAsync(packageStream, serial, 1));

        Assert.Empty(serial.Packets);
    }

    [Fact]
    public async Task SendRejectsDeclaredLengthThatDiffersFromPackageStream()
    {
        using var packageStream = new MemoryStream([0x42], writable: false);
        var serial = new TestXModemStream();
        var sender = new XModemCrcSender();

        await Assert.ThrowsAsync<InvalidDataException>(
            () => sender.SendAsync(packageStream, serial, 2));

        Assert.Empty(serial.Packets);
    }

    [Fact]
    public async Task SendWrapsBlockNumbersAtEightBits()
    {
        var package = new byte[128 * 256 + 1];
        using var packageStream = new MemoryStream(package, writable: false);
        var serial = new TestXModemStream();
        var sender = new XModemCrcSender(responseTimeout: TimeSpan.FromMilliseconds(100));

        await sender.SendAsync(packageStream, serial, package.Length);

        Assert.Equal(257, serial.Packets.Count);
        Assert.Equal((byte)255, serial.Packets[254][1]);
        Assert.Equal((byte)0, serial.Packets[255][1]);
        Assert.Equal((byte)1, serial.Packets[256][1]);
    }

    [Fact]
    public void CrcMatchesTheXmodemKnownVector()
    {
        Assert.Equal(
            (ushort)0x31C3,
            CalculateCrc16Xmodem(Encoding.ASCII.GetBytes("123456789")));
    }

    private static void AssertPacket(
        byte[] packet,
        byte expectedBlockNumber,
        ReadOnlySpan<byte> expectedData)
    {
        Assert.Equal(133, packet.Length);
        Assert.Equal((byte)0x01, packet[0]);
        Assert.Equal(expectedBlockNumber, packet[1]);
        Assert.Equal(unchecked((byte)~expectedBlockNumber), packet[2]);
        Assert.Equal(expectedData.ToArray(), packet.AsSpan(3, expectedData.Length).ToArray());
        Assert.Equal(
            CalculateCrc16Xmodem(packet.AsSpan(3, 128)),
            BinaryPrimitives.ReadUInt16BigEndian(packet.AsSpan(131, 2)));
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

    private sealed class ProgressCollector : IProgress<long>
    {
        public List<long> Values { get; } = [];

        public void Report(long value) => Values.Add(value);
    }

    private sealed class TestXModemStream : Stream
    {
        private readonly Channel<byte> _responses = Channel.CreateUnbounded<byte>();
        private readonly bool _acknowledgePackets;
        private int _naksRemaining;

        public TestXModemStream(
            int naksBeforeAck = 0,
            bool acknowledgePackets = true,
            bool sendCrcRequest = true,
            byte initialResponse = 0x43)
        {
            _naksRemaining = naksBeforeAck;
            _acknowledgePackets = acknowledgePackets;
            if (sendCrcRequest)
            {
                _responses.Writer.TryWrite(initialResponse);
            }
        }

        public List<byte[]> Packets { get; } = [];

        public int EndOfTransmissionCount { get; private set; }

        public int CancelSignalCount { get; private set; }

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
            if (buffer.Length == 0)
            {
                return 0;
            }

            var response = await _responses.Reader.ReadAsync(cancellationToken);
            buffer.Span[0] = response;
            return 1;
        }

        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();

        public override void SetLength(long value) =>
            throw new NotSupportedException();

        public override void Write(byte[] buffer, int offset, int count) =>
            throw new NotSupportedException();

        public override ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (buffer.Length == 1 && buffer.Span[0] == 0x04)
            {
                EndOfTransmissionCount++;
                _responses.Writer.TryWrite(0x06);
                return ValueTask.CompletedTask;
            }

            if (buffer.Span.SequenceEqual(new byte[] { 0x18, 0x18 }))
            {
                CancelSignalCount++;
                return ValueTask.CompletedTask;
            }

            var packet = buffer.ToArray();
            Packets.Add(packet);
            if (_naksRemaining > 0)
            {
                _naksRemaining--;
                _responses.Writer.TryWrite(0x15);
            }
            else if (_acknowledgePackets)
            {
                _responses.Writer.TryWrite(0x06);
            }

            return ValueTask.CompletedTask;
        }
    }
}
