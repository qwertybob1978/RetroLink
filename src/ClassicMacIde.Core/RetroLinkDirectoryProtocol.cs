using System.Buffers.Binary;
using System.Text;

namespace ClassicMacIde.Core;

public sealed record RetroLinkPath(
    uint ProviderId,
    IReadOnlyList<string> Components)
{
    public RetroLinkPath? GetParentPath() =>
        Components.Count == 0
            ? null
            : new RetroLinkPath(
                ProviderId,
                Components.Take(Components.Count - 1).ToArray());
}

public sealed record RetroLinkListRequest(
    uint RoleEpoch,
    ReadOnlyMemory<byte> ActiveEndpointId,
    RetroLinkPath Path,
    uint Cursor,
    ushort EntryLimit);

public sealed record RetroLinkDirectoryEntry(
    byte Kind,
    ushort Attributes,
    string Name,
    ulong DataForkSize,
    uint ResourceForkSize,
    uint ModifiedTime);

public sealed record RetroLinkListPage(
    ushort ResultCode,
    uint Cursor,
    uint NextCursor,
    IReadOnlyList<RetroLinkDirectoryEntry> Entries);

public sealed record RetroLinkMakeDirectoryRequest(
    uint RoleEpoch,
    ReadOnlyMemory<byte> ActiveEndpointId,
    RetroLinkPath ParentPath,
    string Name);

public sealed record RetroLinkRenameRequest(
    uint RoleEpoch,
    ReadOnlyMemory<byte> ActiveEndpointId,
    RetroLinkPath SourcePath,
    RetroLinkPath DestinationParentPath,
    string NewName);

public sealed record RetroLinkDeleteRequest(
    uint RoleEpoch,
    ReadOnlyMemory<byte> ActiveEndpointId,
    RetroLinkPath Path,
    byte Flags = 0);

public sealed record RetroLinkOperationResult(
    ushort ResultCode,
    RetroLinkMessageType OperationKind,
    string? CanonicalName = null);

public static class RetroLinkDirectoryProtocol
{
    public const int MaximumPathComponents = 16;
    public const int MaximumPathBytes = 512;
    public const int MaximumNameBytes = 255;
    public const ushort MaximumEntryLimit = 32;
    public const uint CompleteCursor = uint.MaxValue;
    public const byte FileKind = 0;
    public const byte DirectoryKind = 1;
    private const int ListPageHeaderSize = 11;
    private const int ListEntryFixedSize = 20;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static byte[] EncodeListRequest(
        RetroLinkListRequest request,
        bool allowSlashInComponents = false)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.ActiveEndpointId.Length != RetroLinkProtocol.SessionIdLength)
        {
            throw new ArgumentException(
                $"An endpoint ID must contain {RetroLinkProtocol.SessionIdLength} bytes.",
                nameof(request));
        }

        if (request.EntryLimit is 0 or > MaximumEntryLimit)
        {
            throw new ArgumentOutOfRangeException(
                nameof(request),
                $"An entry limit must be between 1 and {MaximumEntryLimit}.");
        }

        var path = EncodePath(request.Path, allowSlashInComponents);
        var payloadLength = 4 + RetroLinkProtocol.SessionIdLength + path.Length + 6;
        if (payloadLength > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new ArgumentOutOfRangeException(
                nameof(request),
                "The encoded LIST_REQUEST exceeds the frame payload limit.");
        }

        var payload = new byte[payloadLength];
        BinaryPrimitives.WriteUInt32BigEndian(payload, request.RoleEpoch);
        request.ActiveEndpointId.Span.CopyTo(
            payload.AsSpan(4, RetroLinkProtocol.SessionIdLength));
        path.CopyTo(payload.AsSpan(4 + RetroLinkProtocol.SessionIdLength));
        var offset = 4 + RetroLinkProtocol.SessionIdLength + path.Length;
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(offset), request.Cursor);
        BinaryPrimitives.WriteUInt16BigEndian(
            payload.AsSpan(offset + 4),
            request.EntryLimit);
        return payload;
    }

    public static RetroLinkListRequest DecodeListRequest(
        ReadOnlySpan<byte> payload,
        bool allowSlashInComponents = false)
    {
        const int requestPrefixSize = 4 + RetroLinkProtocol.SessionIdLength;
        if (payload.Length < requestPrefixSize + 5 + 6 ||
            payload.Length > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new InvalidDataException("The LIST_REQUEST payload has an invalid size.");
        }

        var roleEpoch = BinaryPrimitives.ReadUInt32BigEndian(payload);
        var endpointId = payload.Slice(4, RetroLinkProtocol.SessionIdLength).ToArray();
        var offset = requestPrefixSize;
        var path = DecodePath(payload, ref offset, allowSlashInComponents);
        if (payload.Length - offset != 6)
        {
            throw new InvalidDataException("The LIST_REQUEST contains trailing or missing data.");
        }

        var cursor = BinaryPrimitives.ReadUInt32BigEndian(payload[offset..]);
        var entryLimit = BinaryPrimitives.ReadUInt16BigEndian(payload[(offset + 4)..]);
        if (entryLimit is 0 or > MaximumEntryLimit)
        {
            throw new InvalidDataException("The LIST_REQUEST entry limit is invalid.");
        }

        return new RetroLinkListRequest(
            roleEpoch,
            endpointId,
            path,
            cursor,
            entryLimit);
    }

    public static byte[] EncodeListPage(
        RetroLinkListPage page,
        bool allowSlashInNames = false)
    {
        ArgumentNullException.ThrowIfNull(page);
        ArgumentNullException.ThrowIfNull(page.Entries);
        if (page.Entries.Count > MaximumEntryLimit ||
            (page.ResultCode != 0 && page.Entries.Count != 0))
        {
            throw new ArgumentException(
                "A LIST_PAGE may contain at most 32 entries and error pages must be empty.",
                nameof(page));
        }

        var encodedNames = page.Entries
            .Select(entry =>
            {
                ValidateComponent(entry.Name, nameof(page), allowSlashInNames);
                return (Entry: entry, Name: EncodeName(entry.Name));
            })
            .ToArray();
        var payloadLength = ListPageHeaderSize +
            encodedNames.Sum(item => ListEntryFixedSize + item.Name.Length);
        if (payloadLength > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new ArgumentOutOfRangeException(
                nameof(page),
                "The encoded LIST_PAGE exceeds the frame payload limit.");
        }

        var payload = new byte[payloadLength];
        BinaryPrimitives.WriteUInt16BigEndian(payload, page.ResultCode);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(2), page.Cursor);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(6), page.NextCursor);
        payload[10] = checked((byte)encodedNames.Length);
        var offset = ListPageHeaderSize;
        foreach (var (entry, name) in encodedNames)
        {
            if (entry.Kind is not FileKind and not DirectoryKind)
            {
                throw new ArgumentException("A LIST_PAGE entry has an invalid kind.", nameof(page));
            }

            payload[offset] = entry.Kind;
            BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(offset + 1), entry.Attributes);
            payload[offset + 3] = checked((byte)name.Length);
            name.CopyTo(payload.AsSpan(offset + 4));
            offset += 4 + name.Length;
            BinaryPrimitives.WriteUInt64BigEndian(payload.AsSpan(offset), entry.DataForkSize);
            BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(offset + 8), entry.ResourceForkSize);
            BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(offset + 12), entry.ModifiedTime);
            offset += 16;
        }

        return payload;
    }

    public static RetroLinkListPage DecodeListPage(
        ReadOnlySpan<byte> payload,
        bool allowSlashInNames = false)
    {
        if (payload.Length < ListPageHeaderSize ||
            payload.Length > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new InvalidDataException("The LIST_PAGE payload has an invalid size.");
        }

        var resultCode = BinaryPrimitives.ReadUInt16BigEndian(payload);
        var cursor = BinaryPrimitives.ReadUInt32BigEndian(payload[2..]);
        var nextCursor = BinaryPrimitives.ReadUInt32BigEndian(payload[6..]);
        var entryCount = payload[10];
        if (entryCount > MaximumEntryLimit ||
            (resultCode != 0 && entryCount != 0))
        {
            throw new InvalidDataException("The LIST_PAGE entry count is invalid.");
        }

        var entries = new List<RetroLinkDirectoryEntry>(entryCount);
        var offset = ListPageHeaderSize;
        for (var index = 0; index < entryCount; index++)
        {
            if (payload.Length - offset < 4)
            {
                throw new InvalidDataException("A LIST_PAGE entry header is truncated.");
            }

            var kind = payload[offset];
            var attributes = BinaryPrimitives.ReadUInt16BigEndian(payload[(offset + 1)..]);
            var nameLength = payload[offset + 3];
            offset += 4;
            if (kind is not FileKind and not DirectoryKind ||
                nameLength is 0 or > MaximumNameBytes ||
                payload.Length - offset < nameLength + 16)
            {
                throw new InvalidDataException("A LIST_PAGE entry is invalid or truncated.");
            }

            string name;
            try
            {
                name = StrictUtf8.GetString(payload.Slice(offset, nameLength));
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException(
                    "A LIST_PAGE entry name is not valid UTF-8.",
                    error);
            }

            ValidateDecodedComponent(
                name,
                "A LIST_PAGE entry name is invalid.",
                allowSlashInNames);
            offset += nameLength;
            var dataForkSize = BinaryPrimitives.ReadUInt64BigEndian(payload[offset..]);
            var resourceForkSize = BinaryPrimitives.ReadUInt32BigEndian(payload[(offset + 8)..]);
            var modifiedTime = BinaryPrimitives.ReadUInt32BigEndian(payload[(offset + 12)..]);
            offset += 16;
            entries.Add(new RetroLinkDirectoryEntry(
                kind,
                attributes,
                name,
                dataForkSize,
                resourceForkSize,
                modifiedTime));
        }

        if (offset != payload.Length)
        {
            throw new InvalidDataException("The LIST_PAGE contains trailing data.");
        }

        return new RetroLinkListPage(resultCode, cursor, nextCursor, entries.AsReadOnly());
    }

    public static byte[] EncodeMakeDirectoryRequest(
        RetroLinkMakeDirectoryRequest request,
        bool allowSlashInComponents = false)
    {
        ArgumentNullException.ThrowIfNull(request);
        var buffer = new MemoryStream();
        buffer.Write(EncodeOperationPrefix(
            request.RoleEpoch,
            request.ActiveEndpointId));
        buffer.Write(EncodePath(request.ParentPath, allowSlashInComponents));
        WriteName(buffer, request.Name, nameof(request));
        return ToOperationPayload(buffer, nameof(request));
    }

    public static RetroLinkMakeDirectoryRequest DecodeMakeDirectoryRequest(
        ReadOnlySpan<byte> payload,
        bool allowSlashInComponents = false)
    {
        var offset = ReadOperationPrefix(payload, out var roleEpoch, out var endpointId);
        var parentPath = DecodePath(payload, ref offset, allowSlashInComponents);
        var name = ReadName(payload, ref offset, allowSlash: false);
        EnsureNoTrailingData(payload, offset, "MKDIR");
        return new RetroLinkMakeDirectoryRequest(
            roleEpoch,
            endpointId,
            parentPath,
            name);
    }

    public static byte[] EncodeRenameRequest(
        RetroLinkRenameRequest request,
        bool allowSlashInComponents = false)
    {
        ArgumentNullException.ThrowIfNull(request);
        EnsureSameProvider(
            request.SourcePath,
            request.DestinationParentPath,
            nameof(request));
        var buffer = new MemoryStream();
        buffer.Write(EncodeOperationPrefix(
            request.RoleEpoch,
            request.ActiveEndpointId));
        buffer.Write(EncodePath(request.SourcePath, allowSlashInComponents));
        buffer.Write(EncodePath(request.DestinationParentPath, allowSlashInComponents));
        WriteName(buffer, request.NewName, nameof(request));
        return ToOperationPayload(buffer, nameof(request));
    }

    public static RetroLinkRenameRequest DecodeRenameRequest(
        ReadOnlySpan<byte> payload,
        bool allowSlashInComponents = false)
    {
        var offset = ReadOperationPrefix(payload, out var roleEpoch, out var endpointId);
        var sourcePath = DecodePath(payload, ref offset, allowSlashInComponents);
        var destinationParentPath = DecodePath(
            payload,
            ref offset,
            allowSlashInComponents);
        var newName = ReadName(payload, ref offset, allowSlash: false);
        EnsureNoTrailingData(payload, offset, "RENAME");
        if (sourcePath.ProviderId != destinationParentPath.ProviderId)
        {
            throw new InvalidDataException("RENAME paths must use the same provider.");
        }

        return new RetroLinkRenameRequest(
            roleEpoch,
            endpointId,
            sourcePath,
            destinationParentPath,
            newName);
    }

    public static byte[] EncodeDeleteRequest(
        RetroLinkDeleteRequest request,
        bool allowSlashInComponents = false)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.Flags != 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(request),
                "DELETE flags must be zero in protocol version 1.");
        }

        var buffer = new MemoryStream();
        buffer.Write(EncodeOperationPrefix(
            request.RoleEpoch,
            request.ActiveEndpointId));
        buffer.Write(EncodePath(request.Path, allowSlashInComponents));
        buffer.WriteByte(request.Flags);
        return ToOperationPayload(buffer, nameof(request));
    }

    public static RetroLinkDeleteRequest DecodeDeleteRequest(
        ReadOnlySpan<byte> payload,
        bool allowSlashInComponents = false)
    {
        var offset = ReadOperationPrefix(payload, out var roleEpoch, out var endpointId);
        var path = DecodePath(payload, ref offset, allowSlashInComponents);
        if (payload.Length - offset != 1 || payload[offset] != 0)
        {
            throw new InvalidDataException("The DELETE flags or payload size is invalid.");
        }

        return new RetroLinkDeleteRequest(roleEpoch, endpointId, path, payload[offset]);
    }

    public static byte[] EncodeOperationResult(RetroLinkOperationResult result)
    {
        ArgumentNullException.ThrowIfNull(result);
        ValidateOperationKind(result.OperationKind, nameof(result));
        var canonicalName = result.CanonicalName is null
            ? Array.Empty<byte>()
            : EncodeName(result.CanonicalName);
        if (result.CanonicalName is not null)
        {
            ValidateComponent(result.CanonicalName, nameof(result), allowSlash: false);
        }

        var payload = new byte[4 + canonicalName.Length];
        BinaryPrimitives.WriteUInt16BigEndian(payload, result.ResultCode);
        payload[2] = (byte)result.OperationKind;
        payload[3] = checked((byte)canonicalName.Length);
        canonicalName.CopyTo(payload.AsSpan(4));
        return payload;
    }

    public static RetroLinkOperationResult DecodeOperationResult(
        ReadOnlySpan<byte> payload)
    {
        if (payload.Length is < 4 or > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new InvalidDataException("The OP_RESULT payload is truncated.");
        }

        var operationKind = (RetroLinkMessageType)payload[2];
        if (operationKind is not RetroLinkMessageType.MakeDirectory and
            not RetroLinkMessageType.Rename and
            not RetroLinkMessageType.Delete)
        {
            throw new InvalidDataException("The OP_RESULT operation kind is invalid.");
        }
        var nameLength = payload[3];
        if (payload.Length != 4 + nameLength)
        {
            throw new InvalidDataException("The OP_RESULT name length is invalid.");
        }

        string? canonicalName = null;
        if (nameLength != 0)
        {
            try
            {
                canonicalName = StrictUtf8.GetString(payload.Slice(4, nameLength));
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException(
                    "The OP_RESULT canonical name is not valid UTF-8.",
                    error);
            }

            ValidateDecodedComponent(
                canonicalName,
                "The OP_RESULT canonical name is invalid.",
                allowSlash: false);
        }

        return new RetroLinkOperationResult(
            BinaryPrimitives.ReadUInt16BigEndian(payload),
            operationKind,
            canonicalName);
    }

    private static byte[] EncodeOperationPrefix(
        uint roleEpoch,
        ReadOnlyMemory<byte> activeEndpointId)
    {
        if (activeEndpointId.Length != RetroLinkProtocol.SessionIdLength)
        {
            throw new ArgumentException(
                $"An endpoint ID must contain {RetroLinkProtocol.SessionIdLength} bytes.",
                nameof(activeEndpointId));
        }

        var prefix = new byte[4 + RetroLinkProtocol.SessionIdLength];
        BinaryPrimitives.WriteUInt32BigEndian(prefix, roleEpoch);
        activeEndpointId.Span.CopyTo(prefix.AsSpan(4));
        return prefix;
    }

    private static int ReadOperationPrefix(
        ReadOnlySpan<byte> payload,
        out uint roleEpoch,
        out ReadOnlyMemory<byte> endpointId)
    {
        const int prefixSize = 4 + RetroLinkProtocol.SessionIdLength;
        if (payload.Length < prefixSize + 5 ||
            payload.Length > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new InvalidDataException("The filesystem operation payload is truncated.");
        }

        roleEpoch = BinaryPrimitives.ReadUInt32BigEndian(payload);
        endpointId = payload.Slice(4, RetroLinkProtocol.SessionIdLength).ToArray();
        return prefixSize;
    }

    private static void WriteName(
        Stream buffer,
        string name,
        string parameterName)
    {
        var encodedName = EncodeName(name);
        ValidateComponent(name, parameterName, allowSlash: false);
        buffer.WriteByte(checked((byte)encodedName.Length));
        buffer.Write(encodedName);
    }

    private static string ReadName(
        ReadOnlySpan<byte> payload,
        ref int offset,
        bool allowSlash)
    {
        if (offset >= payload.Length)
        {
            throw new InvalidDataException("The filesystem operation name is missing.");
        }

        var nameLength = payload[offset++];
        if (nameLength is 0 or > MaximumNameBytes ||
            payload.Length - offset < nameLength)
        {
            throw new InvalidDataException("The filesystem operation name is invalid or truncated.");
        }

        string name;
        try
        {
            name = StrictUtf8.GetString(payload.Slice(offset, nameLength));
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "The filesystem operation name is not valid UTF-8.",
                error);
        }

        ValidateDecodedComponent(
            name,
            "The filesystem operation name is invalid.",
            allowSlash);
        offset += nameLength;
        return name;
    }

    private static void EnsureNoTrailingData(
        ReadOnlySpan<byte> payload,
        int offset,
        string operation)
    {
        if (offset != payload.Length)
        {
            throw new InvalidDataException($"The {operation} payload contains trailing data.");
        }
    }

    private static void EnsureSameProvider(
        RetroLinkPath sourcePath,
        RetroLinkPath destinationParentPath,
        string parameterName)
    {
        ArgumentNullException.ThrowIfNull(sourcePath);
        ArgumentNullException.ThrowIfNull(destinationParentPath);
        if (sourcePath.ProviderId != destinationParentPath.ProviderId)
        {
            throw new ArgumentException(
                "A rename must stay within one provider and volume.",
                parameterName);
        }
    }

    private static byte[] ToOperationPayload(MemoryStream buffer, string parameterName)
    {
        if (buffer.Length > RetroLinkProtocol.MaximumPayloadSize)
        {
            throw new ArgumentOutOfRangeException(
                parameterName,
                "The filesystem operation exceeds the frame payload limit.");
        }

        return buffer.ToArray();
    }

    private static void ValidateOperationKind(
        RetroLinkMessageType operationKind,
        string parameterName)
    {
        if (operationKind is not RetroLinkMessageType.MakeDirectory and
            not RetroLinkMessageType.Rename and
            not RetroLinkMessageType.Delete)
        {
            throw new ArgumentException(
                "An OP_RESULT must identify a supported filesystem operation.",
                parameterName);
        }
    }

    private static byte[] EncodePath(
        RetroLinkPath path,
        bool allowSlashInComponents)
    {
        ArgumentNullException.ThrowIfNull(path);
        ArgumentNullException.ThrowIfNull(path.Components);
        if (path.ProviderId == 0 || path.Components.Count > MaximumPathComponents)
        {
            throw new ArgumentException("The PathRef provider or component count is invalid.", nameof(path));
        }

        using var buffer = new MemoryStream();
        Span<byte> fixedPart = stackalloc byte[5];
        BinaryPrimitives.WriteUInt32BigEndian(fixedPart, path.ProviderId);
        fixedPart[4] = checked((byte)path.Components.Count);
        buffer.Write(fixedPart);
        foreach (var component in path.Components)
        {
            var name = EncodeName(component);
            ValidateComponent(component, nameof(path), allowSlashInComponents);
            buffer.WriteByte(checked((byte)name.Length));
            buffer.Write(name);
        }

        if (buffer.Length > MaximumPathBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(path),
                $"A PathRef cannot exceed {MaximumPathBytes} bytes.");
        }

        return buffer.ToArray();
    }

    private static RetroLinkPath DecodePath(
        ReadOnlySpan<byte> payload,
        ref int offset,
        bool allowSlashInComponents)
    {
        var pathStart = offset;
        if (payload.Length - offset < 5)
        {
            throw new InvalidDataException("The PathRef header is truncated.");
        }

        var providerId = BinaryPrimitives.ReadUInt32BigEndian(payload[offset..]);
        var componentCount = payload[offset + 4];
        offset += 5;
        if (providerId == 0 || componentCount > MaximumPathComponents)
        {
            throw new InvalidDataException("The PathRef provider or component count is invalid.");
        }

        var components = new List<string>(componentCount);
        for (var index = 0; index < componentCount; index++)
        {
            if (payload.Length - offset < 1)
            {
                throw new InvalidDataException("A PathRef component length is missing.");
            }

            var length = payload[offset++];
            if (length is 0 or > MaximumNameBytes || payload.Length - offset < length)
            {
                throw new InvalidDataException("A PathRef component is truncated or invalid.");
            }

            string component;
            try
            {
                component = StrictUtf8.GetString(payload.Slice(offset, length));
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException(
                    "A PathRef component is not valid UTF-8.",
                    error);
            }

            ValidateDecodedComponent(
                component,
                "A PathRef component is invalid.",
                allowSlashInComponents);
            components.Add(component);
            offset += length;
        }

        if (offset - pathStart > MaximumPathBytes)
        {
            throw new InvalidDataException(
                $"A PathRef cannot exceed {MaximumPathBytes} bytes.");
        }

        return new RetroLinkPath(providerId, components.AsReadOnly());
    }

    private static byte[] EncodeName(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        var bytes = StrictUtf8.GetBytes(name);
        if (bytes.Length is 0 or > MaximumNameBytes)
        {
            throw new ArgumentException(
                $"A path component must use 1 to {MaximumNameBytes} UTF-8 bytes.",
                nameof(name));
        }

        return bytes;
    }

    private static void ValidateComponent(
        string component,
        string parameterName,
        bool allowSlash)
    {
        if (component is "." or ".." ||
            (!allowSlash && component.Contains('/')) ||
            component.Contains('\\') ||
            component.Contains(':') ||
            component.IndexOf('\0') >= 0)
        {
            throw new ArgumentException(
                "A path component cannot be empty, relative, or contain path separators.",
                parameterName);
        }
    }

    private static void ValidateDecodedComponent(
        string component,
        string message,
        bool allowSlash)
    {
        try
        {
            ValidateComponent(component, nameof(component), allowSlash);
        }
        catch (ArgumentException error)
        {
            throw new InvalidDataException(message, error);
        }
    }
}
