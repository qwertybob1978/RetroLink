using System.Buffers.Binary;
using System.Text;

namespace ClassicMacIde.Core;

public sealed record RetroLinkCopyPrepare(
    uint RoleEpoch,
    ReadOnlyMemory<byte> ActiveEndpointId,
    ReadOnlyMemory<byte> SourceEndpointId,
    RetroLinkPath SourcePath,
    ReadOnlyMemory<byte> DestinationEndpointId,
    RetroLinkPath DestinationPath,
    string DestinationName,
    ulong StreamLength,
    uint StreamCrc32,
    byte TransferFormat,
    bool Overwrite,
    byte TransferMode);

public sealed record RetroLinkTransferReady(
    ushort ResultCode,
    ulong StreamLength,
    uint StreamCrc32,
    byte TransferMode,
    ushort MaximumBlockSize);

public sealed record RetroLinkTransferComplete(
    ushort ResultCode,
    ulong StreamLength,
    uint StreamCrc32);

public static class RetroLinkTransferProtocol
{
    public const byte RawDataFormat = 0;
    public const byte XModemCrc128Mode = 1;
    public const ushort XModemCrcBlockSize = 128;
    public const int MaximumTransferSize = int.MaxValue;
    private const int EndpointIdLength = RetroLinkProtocol.SessionIdLength;
    private const int CopyPrepareFixedLength = 4 + EndpointIdLength * 2 + EndpointIdLength + 8 + 4 + 3;
    private const int TransferReadyLength = 17;
    private const int TransferCompleteLength = 14;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static byte[] EncodeCopyPrepare(RetroLinkCopyPrepare request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateRequest(request);
        using var buffer = new MemoryStream();
        Span<byte> fixedPart = stackalloc byte[4 + EndpointIdLength * 2];
        BinaryPrimitives.WriteUInt32BigEndian(fixedPart, request.RoleEpoch);
        request.ActiveEndpointId.Span.CopyTo(fixedPart[4..]);
        request.SourceEndpointId.Span.CopyTo(fixedPart[(4 + EndpointIdLength)..]);
        buffer.Write(fixedPart);
        WritePath(buffer, request.SourcePath);
        buffer.Write(request.DestinationEndpointId.Span);
        WritePath(buffer, request.DestinationPath);
        var name = StrictUtf8.GetBytes(request.DestinationName);
        buffer.WriteByte(checked((byte)name.Length));
        buffer.Write(name);
        Span<byte> trailer = stackalloc byte[15];
        BinaryPrimitives.WriteUInt64BigEndian(trailer, request.StreamLength);
        BinaryPrimitives.WriteUInt32BigEndian(trailer[8..], request.StreamCrc32);
        trailer[12] = request.TransferFormat;
        trailer[13] = request.Overwrite ? (byte)1 : (byte)0;
        trailer[14] = request.TransferMode;
        buffer.Write(trailer);
        if (buffer.Length > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new ArgumentOutOfRangeException(
                nameof(request),
                "The COPY_PREPARE payload exceeds the frame limit.");
        }

        return buffer.ToArray();
    }

    public static RetroLinkCopyPrepare DecodeCopyPrepare(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < CopyPrepareFixedLength)
        {
            throw new InvalidDataException("The COPY_PREPARE payload is truncated.");
        }

        var roleEpoch = BinaryPrimitives.ReadUInt32BigEndian(payload);
        var activeEndpointId = payload.Slice(4, EndpointIdLength).ToArray();
        var sourceEndpointId = payload.Slice(4 + EndpointIdLength, EndpointIdLength).ToArray();
        var offset = 4 + EndpointIdLength * 2;
        var sourcePath = ReadPath(payload, ref offset);
        if (payload.Length - offset < EndpointIdLength)
        {
            throw new InvalidDataException("The COPY_PREPARE destination endpoint is truncated.");
        }

        var destinationEndpointId = payload.Slice(offset, EndpointIdLength).ToArray();
        offset += EndpointIdLength;
        var destinationPath = ReadPath(payload, ref offset);
        if (payload.Length - offset < 1)
        {
            throw new InvalidDataException("The COPY_PREPARE destination name is missing.");
        }

        var nameLength = payload[offset++];
        if (nameLength is 0 or > 255 || payload.Length - offset != nameLength + 15)
        {
            throw new InvalidDataException("The COPY_PREPARE name or trailer is invalid.");
        }

        string destinationName;
        try
        {
            destinationName = StrictUtf8.GetString(payload.Slice(offset, nameLength));
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException("The COPY_PREPARE name is not valid UTF-8.", error);
        }

        offset += nameLength;
        var request = new RetroLinkCopyPrepare(
            roleEpoch,
            activeEndpointId,
            sourceEndpointId,
            sourcePath,
            destinationEndpointId,
            destinationPath,
            destinationName,
            BinaryPrimitives.ReadUInt64BigEndian(payload[offset..]),
            BinaryPrimitives.ReadUInt32BigEndian(payload[(offset + 8)..]),
            payload[offset + 12],
            payload[offset + 13] switch
            {
                0 => false,
                1 => true,
                _ => throw new InvalidDataException("The COPY_PREPARE overwrite flag is invalid."),
            },
            payload[offset + 14]);
        try
        {
            ValidateRequest(request);
        }
        catch (ArgumentException error)
        {
            throw new InvalidDataException("The COPY_PREPARE fields are invalid.", error);
        }

        return request;
    }

    public static byte[] EncodeTransferReady(RetroLinkTransferReady response)
    {
        ValidateTransferMode(response.TransferMode, response.MaximumBlockSize);
        var payload = new byte[TransferReadyLength];
        BinaryPrimitives.WriteUInt16BigEndian(payload, response.ResultCode);
        BinaryPrimitives.WriteUInt64BigEndian(payload.AsSpan(2), response.StreamLength);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(10), response.StreamCrc32);
        payload[14] = response.TransferMode;
        BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(15), response.MaximumBlockSize);
        return payload;
    }

    public static RetroLinkTransferReady DecodeTransferReady(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != TransferReadyLength)
        {
            throw new InvalidDataException("The TRANSFER_READY payload has an invalid size.");
        }

        var result = new RetroLinkTransferReady(
            BinaryPrimitives.ReadUInt16BigEndian(payload),
            BinaryPrimitives.ReadUInt64BigEndian(payload[2..]),
            BinaryPrimitives.ReadUInt32BigEndian(payload[10..]),
            payload[14],
            BinaryPrimitives.ReadUInt16BigEndian(payload[15..]));
        if (result.ResultCode == 0)
        {
            try
            {
                ValidateTransferMode(result.TransferMode, result.MaximumBlockSize);
            }
            catch (ArgumentException error)
            {
                throw new InvalidDataException("The TRANSFER_READY mode is invalid.", error);
            }
        }

        return result;
    }

    public static byte[] EncodeTransferComplete(RetroLinkTransferComplete response)
    {
        var payload = new byte[TransferCompleteLength];
        BinaryPrimitives.WriteUInt16BigEndian(payload, response.ResultCode);
        BinaryPrimitives.WriteUInt64BigEndian(payload.AsSpan(2), response.StreamLength);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(10), response.StreamCrc32);
        return payload;
    }

    public static RetroLinkTransferComplete DecodeTransferComplete(
        ReadOnlySpan<byte> payload)
    {
        if (payload.Length != TransferCompleteLength)
        {
            throw new InvalidDataException(
                "The TRANSFER_COMPLETE payload has an invalid size.");
        }

        return new RetroLinkTransferComplete(
            BinaryPrimitives.ReadUInt16BigEndian(payload),
            BinaryPrimitives.ReadUInt64BigEndian(payload[2..]),
            BinaryPrimitives.ReadUInt32BigEndian(payload[10..]));
    }

    private static void ValidateRequest(RetroLinkCopyPrepare request)
    {
        ValidateEndpointId(request.ActiveEndpointId, nameof(request.ActiveEndpointId));
        ValidateEndpointId(request.SourceEndpointId, nameof(request.SourceEndpointId));
        ValidateEndpointId(
            request.DestinationEndpointId,
            nameof(request.DestinationEndpointId));
        ValidatePath(request.SourcePath);
        ValidatePath(request.DestinationPath);
        var name = StrictUtf8.GetByteCount(request.DestinationName);
        if (name is 0 or > 255 ||
            request.DestinationName is "." or ".." ||
            request.DestinationName.Contains('\\') ||
            request.DestinationName.Contains(':') ||
            request.DestinationName.IndexOf('\0') >= 0)
        {
            throw new ArgumentException(
                "The destination name is invalid or too long.",
                nameof(request));
        }

        if (request.StreamLength > MaximumTransferSize ||
            request.TransferFormat != RawDataFormat ||
            request.TransferMode != XModemCrc128Mode)
        {
            throw new ArgumentException(
                "The file size, transfer format, or transfer mode is unsupported.",
                nameof(request));
        }
    }

    private static void ValidateEndpointId(
        ReadOnlyMemory<byte> endpointId,
        string parameterName)
    {
        if (endpointId.Length != EndpointIdLength)
        {
            throw new ArgumentException(
                $"An endpoint ID must contain {EndpointIdLength} bytes.",
                parameterName);
        }
    }

    private static void ValidatePath(RetroLinkPath path)
    {
        ArgumentNullException.ThrowIfNull(path);
        ArgumentNullException.ThrowIfNull(path.Components);
        if (path.ProviderId == 0 ||
            path.Components.Count > RetroLinkDirectoryProtocol.MaximumPathComponents)
        {
            throw new ArgumentException("A transfer PathRef is invalid.", nameof(path));
        }

        foreach (var component in path.Components)
        {
            var bytes = StrictUtf8.GetByteCount(component);
            if (bytes is 0 or > RetroLinkDirectoryProtocol.MaximumNameBytes ||
                component is "." or ".." ||
                component.Contains('\\') ||
                component.Contains(':') ||
                component.IndexOf('\0') >= 0)
            {
                throw new ArgumentException("A transfer PathRef component is invalid.", nameof(path));
            }
        }
    }

    private static void ValidateTransferMode(byte mode, ushort blockSize)
    {
        if (mode != XModemCrc128Mode || blockSize != XModemCrcBlockSize)
        {
            throw new ArgumentException("Only 128-byte XMODEM-CRC is supported.");
        }
    }

    private static void WritePath(Stream stream, RetroLinkPath path)
    {
        using var encoded = new MemoryStream();
        Span<byte> header = stackalloc byte[5];
        BinaryPrimitives.WriteUInt32BigEndian(header, path.ProviderId);
        header[4] = checked((byte)path.Components.Count);
        encoded.Write(header);
        foreach (var component in path.Components)
        {
            var bytes = StrictUtf8.GetBytes(component);
            encoded.WriteByte(checked((byte)bytes.Length));
            encoded.Write(bytes);
        }

        if (encoded.Length > RetroLinkDirectoryProtocol.MaximumPathBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(path),
                "A transfer PathRef exceeds the maximum encoded size.");
        }

        encoded.Position = 0;
        encoded.CopyTo(stream);
    }

    private static RetroLinkPath ReadPath(ReadOnlySpan<byte> payload, ref int offset)
    {
        var start = offset;
        if (payload.Length - offset < 5)
        {
            throw new InvalidDataException("A transfer PathRef is truncated.");
        }

        var providerId = BinaryPrimitives.ReadUInt32BigEndian(payload[offset..]);
        var componentCount = payload[offset + 4];
        offset += 5;
        if (providerId == 0 ||
            componentCount > RetroLinkDirectoryProtocol.MaximumPathComponents)
        {
            throw new InvalidDataException("A transfer PathRef provider or depth is invalid.");
        }

        var components = new List<string>(componentCount);
        for (var index = 0; index < componentCount; index++)
        {
            if (payload.Length - offset < 1)
            {
                throw new InvalidDataException("A transfer PathRef component is truncated.");
            }

            var length = payload[offset++];
            if (length is 0 or > RetroLinkDirectoryProtocol.MaximumNameBytes ||
                payload.Length - offset < length)
            {
                throw new InvalidDataException("A transfer PathRef component is invalid.");
            }

            string component;
            try
            {
                component = StrictUtf8.GetString(payload.Slice(offset, length));
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException(
                    "A transfer PathRef component is not valid UTF-8.",
                    error);
            }

            components.Add(component);
            offset += length;
        }

        if (offset - start > RetroLinkDirectoryProtocol.MaximumPathBytes)
        {
            throw new InvalidDataException("A transfer PathRef is oversized.");
        }

        var path = new RetroLinkPath(providerId, components.AsReadOnly());
        try
        {
            ValidatePath(path);
        }
        catch (ArgumentException error)
        {
            throw new InvalidDataException("A transfer PathRef is invalid.", error);
        }

        return path;
    }
}
