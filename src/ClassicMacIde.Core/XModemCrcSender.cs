using System.Buffers.Binary;
using System.IO;

namespace ClassicMacIde.Core;

public sealed class XModemTransferException : IOException
{
    public XModemTransferException(string message)
        : base(message)
    {
    }

    public XModemTransferException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}

public sealed class XModemCrcSender
{
    private const byte StartOfHeader = 0x01;
    private const byte EndOfTransmission = 0x04;
    private const byte Acknowledge = 0x06;
    private const byte NegativeAcknowledge = 0x15;
    private const byte Cancel = 0x18;
    private const byte CrcRequest = (byte)'C';
    private const byte Substitute = 0x1A;
    private const int BlockSize = 128;
    private const int PacketSize = 3 + BlockSize + 2;

    private readonly int _maximumAttempts;
    private readonly TimeSpan _responseTimeout;

    public XModemCrcSender(
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

    public async Task SendAsync(
        Stream packageStream,
        Stream serialStream,
        long packageLength,
        CancellationToken cancellationToken = default,
        IProgress<long>? progress = null)
    {
        ArgumentNullException.ThrowIfNull(packageStream);
        ArgumentNullException.ThrowIfNull(serialStream);
        if (!packageStream.CanRead || !packageStream.CanSeek)
        {
            throw new ArgumentException(
                "The package stream must be readable and seekable.",
                nameof(packageStream));
        }

        if (!serialStream.CanRead || !serialStream.CanWrite)
        {
            throw new ArgumentException(
                "The XMODEM stream must be readable and writable.",
                nameof(serialStream));
        }

        if (packageLength <= 0 || packageLength > uint.MaxValue)
        {
            throw new ArgumentOutOfRangeException(
                nameof(packageLength),
                $"Package length must be between 1 and {uint.MaxValue} bytes.");
        }

        if (packageStream.Length - packageStream.Position != packageLength)
        {
            throw new InvalidDataException(
                "The declared package length does not match the readable stream length.");
        }

        cancellationToken.ThrowIfCancellationRequested();
        var controlBuffer = new byte[1];
        try
        {
            progress?.Report(0);
            await WaitForCrcRequestAsync(
                serialStream,
                controlBuffer,
                cancellationToken);

            var packet = new byte[PacketSize];
            var block = new byte[BlockSize];
            long bytesRemaining = packageLength;
            byte blockNumber = 1;
            while (bytesRemaining > 0)
            {
                var dataLength = (int)Math.Min(BlockSize, bytesRemaining);
                await packageStream.ReadExactlyAsync(
                    block.AsMemory(0, dataLength),
                    cancellationToken);
                Array.Fill(block, Substitute, dataLength, BlockSize - dataLength);

                packet[0] = StartOfHeader;
                packet[1] = blockNumber;
                packet[2] = unchecked((byte)~blockNumber);
                block.CopyTo(packet, 3);
                BinaryPrimitives.WriteUInt16BigEndian(
                    packet.AsSpan(3 + BlockSize, 2),
                    CalculateCrc16Xmodem(block));

                var acknowledged = false;
                for (var attempt = 1; attempt <= _maximumAttempts; attempt++)
                {
                    await WriteAsync(serialStream, packet, cancellationToken);
                    var response = await ReadControlResponseAsync(
                        serialStream,
                        controlBuffer,
                        cancellationToken);
                    if (response == Acknowledge)
                    {
                        acknowledged = true;
                        break;
                    }

                    if (response == Cancel)
                    {
                        throw new XModemTransferException(
                            "The receiver cancelled the XMODEM transfer.");
                    }
                }

                if (!acknowledged)
                {
                    throw new TimeoutException(
                        $"XMODEM block {blockNumber} was not acknowledged after {_maximumAttempts} attempts.");
                }

                bytesRemaining -= dataLength;
                progress?.Report(packageLength - bytesRemaining);
                blockNumber = unchecked((byte)(blockNumber + 1));
            }

            if (await packageStream.ReadAsync(
                    controlBuffer.AsMemory(),
                    cancellationToken) != 0)
            {
                throw new InvalidDataException(
                    "The package stream contains data beyond its declared length.");
            }

            for (var attempt = 1; attempt <= _maximumAttempts; attempt++)
            {
                await WriteAsync(
                    serialStream,
                    new[] { EndOfTransmission },
                    cancellationToken);
                var response = await ReadControlResponseAsync(
                    serialStream,
                    controlBuffer,
                    cancellationToken);
                if (response == Acknowledge)
                {
                    return;
                }

                if (response == Cancel)
                {
                    throw new XModemTransferException(
                        "The receiver cancelled the XMODEM transfer at end of transmission.");
                }
            }

            throw new TimeoutException(
                $"The receiver did not acknowledge XMODEM end of transmission after {_maximumAttempts} attempts.");
        }
        catch (OperationCanceledException cancellationError) when (
            cancellationToken.IsCancellationRequested)
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
                    "The transfer was cancelled, but the receiver could not be notified.",
                    new AggregateException(cancellationError, notificationError));
            }

            throw;
        }
    }

    private async Task WaitForCrcRequestAsync(
        Stream serialStream,
        byte[] controlBuffer,
        CancellationToken cancellationToken)
    {
        for (var attempt = 1; attempt <= _maximumAttempts; attempt++)
        {
            var response = await ReadControlResponseAsync(
                serialStream,
                controlBuffer,
                cancellationToken);
            if (response == CrcRequest)
            {
                return;
            }

            if (response == Cancel)
            {
                throw new XModemTransferException(
                    "The receiver cancelled the XMODEM transfer before sending data.");
            }
        }

        throw new TimeoutException(
            $"The receiver did not request XMODEM-CRC after {_maximumAttempts} attempts.");
    }

    private async Task<byte?> ReadControlResponseAsync(
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

            var value = buffer[0];
            if (value is Acknowledge or NegativeAcknowledge or CrcRequest or Cancel)
            {
                return value;
            }
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
}
