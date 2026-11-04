using System.Buffers.Binary;
using System.IO;

namespace ClassicMacIde.Core;

public sealed record XModemCrcReceiveResult(long Length, uint Crc32);

public sealed class XModemCrcReceiver
{
    private const byte StartOfHeader = 0x01;
    private const byte EndOfTransmission = 0x04;
    private const byte Acknowledge = 0x06;
    private const byte NegativeAcknowledge = 0x15;
    private const byte Cancel = 0x18;
    private const byte CrcRequest = (byte)'C';
    private const byte Substitute = 0x1A;
    private const int BlockSize = 128;
    private const int PacketDataLength = 2 + BlockSize + 2;

    private readonly int _maximumAttempts;
    private readonly TimeSpan _responseTimeout;

    public XModemCrcReceiver(
        int maximumAttempts = 10,
        TimeSpan? responseTimeout = null)
    {
        if (maximumAttempts <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumAttempts));
        }

        var timeout = responseTimeout ?? TimeSpan.FromSeconds(3);
        if (timeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(responseTimeout));
        }

        _maximumAttempts = maximumAttempts;
        _responseTimeout = timeout;
    }

    public async Task<XModemCrcReceiveResult> ReceiveAsync(
        Stream serialStream,
        Stream destinationStream,
        long expectedLength,
        CancellationToken cancellationToken = default,
        IProgress<long>? progress = null)
    {
        ArgumentNullException.ThrowIfNull(serialStream);
        ArgumentNullException.ThrowIfNull(destinationStream);
        if (!serialStream.CanRead || !serialStream.CanWrite)
        {
            throw new ArgumentException(
                "The XMODEM stream must be readable and writable.",
                nameof(serialStream));
        }

        if (!destinationStream.CanWrite)
        {
            throw new ArgumentException(
                "The destination stream must be writable.",
                nameof(destinationStream));
        }

        if (expectedLength < 0 || expectedLength > uint.MaxValue)
        {
            throw new ArgumentOutOfRangeException(nameof(expectedLength));
        }

        cancellationToken.ThrowIfCancellationRequested();
        progress?.Report(0);
        if (expectedLength == 0)
        {
            return new XModemCrcReceiveResult(0, Crc32IsoHdlc.Compute(ReadOnlySpan<byte>.Empty));
        }

        var controlBuffer = new byte[1];
        var packet = new byte[PacketDataLength];
        var receivedLength = 0L;
        var crc32 = uint.MaxValue;
        byte expectedBlockNumber = 1;
        var failedAttempts = 0;

        try
        {
            var started = false;
            for (var attempt = 0; attempt < _maximumAttempts; attempt++)
            {
                await WriteAsync(serialStream, new byte[] { CrcRequest }, cancellationToken);
                var response = await ReadByteAsync(
                    serialStream,
                    controlBuffer,
                    cancellationToken);
                if (response is null)
                {
                    continue;
                }

                if (response == Cancel)
                {
                    throw new XModemTransferException(
                        "The sender cancelled before starting the XMODEM transfer.");
                }

                if (response == StartOfHeader)
                {
                    started = true;
                    break;
                }
            }

            if (!started)
            {
                throw new TimeoutException(
                    $"The sender did not start XMODEM-CRC after {_maximumAttempts} requests.");
            }

            var startHeaderPending = true;
            while (true)
            {
                byte? control;
                if (startHeaderPending)
                {
                    control = StartOfHeader;
                    startHeaderPending = false;
                }
                else
                {
                    try
                    {
                        control = await ReadByteAsync(
                            serialStream,
                            controlBuffer,
                            cancellationToken);
                    }
                    catch (TimeoutException)
                    {
                        await RejectAttemptAsync(
                            serialStream,
                            ++failedAttempts,
                            "The sender timed out while waiting for an XMODEM block.");
                        continue;
                    }
                }

                if (control is null)
                {
                    await RejectAttemptAsync(
                        serialStream,
                        ++failedAttempts,
                        "The sender timed out while waiting for an XMODEM block.");
                    continue;
                }

                if (control == Cancel)
                {
                    throw new XModemTransferException(
                        "The sender cancelled the XMODEM transfer.");
                }

                if (control == EndOfTransmission)
                {
                    if (receivedLength != expectedLength)
                    {
                        await RejectAttemptAsync(
                            serialStream,
                            ++failedAttempts,
                            $"The sender ended after {receivedLength} of {expectedLength} bytes.");
                        continue;
                    }

                    await WriteAsync(serialStream, new byte[] { Acknowledge }, cancellationToken);
                    return new XModemCrcReceiveResult(receivedLength, ~crc32);
                }

                if (control != StartOfHeader)
                {
                    await RejectAttemptAsync(
                        serialStream,
                        ++failedAttempts,
                        "The sender sent an invalid XMODEM control byte.");
                    continue;
                }

                try
                {
                    await ReadExactlyAsync(
                        serialStream,
                        packet,
                        cancellationToken);
                }
                catch (TimeoutException)
                {
                    await RejectAttemptAsync(
                        serialStream,
                        ++failedAttempts,
                        "The sender timed out while sending an XMODEM block.");
                    continue;
                }

                var blockNumber = packet[0];
                var blockComplement = packet[1];
                var receivedCrc = BinaryPrimitives.ReadUInt16BigEndian(
                    packet.AsSpan(2 + BlockSize, 2));
                if (unchecked((byte)~blockNumber) != blockComplement ||
                    CalculateCrc16Xmodem(packet.AsSpan(2, BlockSize)) != receivedCrc)
                {
                    await RejectAttemptAsync(
                        serialStream,
                        ++failedAttempts,
                        "The sender sent an XMODEM block with invalid CRC or sequence data.");
                    continue;
                }

                var previousBlockNumber = unchecked((byte)(expectedBlockNumber - 1));
                if (blockNumber == previousBlockNumber)
                {
                    await WriteAsync(serialStream, new byte[] { Acknowledge }, cancellationToken);
                    failedAttempts = 0;
                    continue;
                }

                if (blockNumber != expectedBlockNumber)
                {
                    await RejectAttemptAsync(
                        serialStream,
                        ++failedAttempts,
                        $"Expected XMODEM block {expectedBlockNumber}, received {blockNumber}.");
                    continue;
                }

                var bytesRemaining = expectedLength - receivedLength;
                if (bytesRemaining <= 0)
                {
                    await RejectAttemptAsync(
                        serialStream,
                        ++failedAttempts,
                        "The sender sent data beyond the declared transfer length.");
                    continue;
                }

                var dataLength = (int)Math.Min(BlockSize, bytesRemaining);
                if (packet.AsSpan(2 + dataLength, BlockSize - dataLength)
                    .IndexOfAnyExcept(Substitute) >= 0)
                {
                    await RejectAttemptAsync(
                        serialStream,
                        ++failedAttempts,
                        "The final XMODEM block has invalid padding.");
                    continue;
                }

                var acceptedData = packet.AsMemory(2, dataLength).ToArray();
                await destinationStream.WriteAsync(
                    acceptedData,
                    cancellationToken);
                UpdateCrc32(ref crc32, acceptedData);
                receivedLength += dataLength;
                expectedBlockNumber = unchecked((byte)(expectedBlockNumber + 1));
                failedAttempts = 0;
                progress?.Report(receivedLength);
                await WriteAsync(serialStream, new byte[] { Acknowledge }, cancellationToken);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            using var notificationTimeout =
                new CancellationTokenSource(_responseTimeout);
            try
            {
                await WriteAsync(
                    serialStream,
                    new byte[] { Cancel, Cancel },
                    notificationTimeout.Token);
            }
            catch (Exception notificationError) when (
                notificationError is IOException or
                    ObjectDisposedException or
                    TimeoutException or
                    OperationCanceledException)
            {
                throw new XModemTransferException(
                    "The transfer was cancelled, but the sender could not be notified.",
                    new OperationCanceledException(
                        "The receiver was cancelled.",
                        notificationError,
                        cancellationToken));
            }

            throw;
        }
        catch (Exception error) when (
            (error is IOException or TimeoutException or ObjectDisposedException) &&
            error is not XModemTransferException)
        {
            using var notificationTimeout =
                new CancellationTokenSource(_responseTimeout);
            try
            {
                await WriteAsync(
                    serialStream,
                    new byte[] { Cancel, Cancel },
                    notificationTimeout.Token);
            }
            catch (Exception notificationError) when (
                notificationError is IOException or
                    ObjectDisposedException or
                    TimeoutException or
                    OperationCanceledException)
            {
            }

            throw;
        }
    }

    private async Task RejectAttemptAsync(
        Stream serialStream,
        int failedAttempts,
        string reason)
    {
        if (failedAttempts >= _maximumAttempts)
        {
            await WriteAsync(
                serialStream,
                new byte[] { Cancel, Cancel },
                CancellationToken.None);
            throw new XModemTransferException(reason);
        }

        await WriteAsync(
            serialStream,
            new byte[] { NegativeAcknowledge },
            CancellationToken.None);
    }

    private async Task<byte?> ReadByteAsync(
        Stream serialStream,
        byte[] buffer,
        CancellationToken cancellationToken)
    {
        using var timeout = new CancellationTokenSource(_responseTimeout);
        using var linkedCancellation =
            CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                timeout.Token);
        while (true)
        {
            int read;
            try
            {
                read = await serialStream.ReadAsync(
                    buffer.AsMemory(),
                    linkedCancellation.Token);
            }
            catch (OperationCanceledException) when (
                !cancellationToken.IsCancellationRequested)
            {
                return null;
            }

            if (read == 0)
            {
                throw new EndOfStreamException(
                    "The serial stream closed during XMODEM transfer.");
            }

            if (buffer[0] is StartOfHeader or EndOfTransmission or Cancel)
            {
                return buffer[0];
            }
        }
    }

    private async Task ReadExactlyAsync(
        Stream serialStream,
        Memory<byte> buffer,
        CancellationToken cancellationToken)
    {
        using var timeout = new CancellationTokenSource(_responseTimeout);
        using var linkedCancellation =
            CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                timeout.Token);
        try
        {
            await serialStream.ReadExactlyAsync(buffer, linkedCancellation.Token);
        }
        catch (OperationCanceledException) when (
            !cancellationToken.IsCancellationRequested)
        {
            throw new TimeoutException("Timed out while reading an XMODEM block.");
        }
    }

    private static async Task WriteAsync(
        Stream serialStream,
        ReadOnlyMemory<byte> data,
        CancellationToken cancellationToken)
    {
        await serialStream.WriteAsync(data, cancellationToken);
        await serialStream.FlushAsync(cancellationToken);
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

    private static void UpdateCrc32(ref uint crc, ReadOnlySpan<byte> data)
    {
        foreach (var value in data)
        {
            crc ^= value;
            for (var bit = 0; bit < 8; bit++)
            {
                crc = (crc & 1) != 0
                    ? (crc >> 1) ^ 0xEDB88320
                    : crc >> 1;
            }
        }
    }
}
