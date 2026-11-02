using System.Text;
using ClassicMacIde.Core;

namespace ClassicMacIde.Core.Tests;

public class RetroLinkDirectoryProtocolTests
{
    [Fact]
    public void ParentPathRemovesOneComponentAndPreservesProvider()
    {
        var path = new RetroLinkPath(12, ["Projects", "Mac App"]);

        var parent = path.GetParentPath();

        Assert.NotNull(parent);
        Assert.Equal(12u, parent.ProviderId);
        Assert.Equal(new[] { "Projects" }, parent.Components);
    }

    [Fact]
    public void ParentPathAtVolumeRootIsNull()
    {
        var path = new RetroLinkPath(12, []);

        Assert.Null(path.GetParentPath());
    }

    [Fact]
    public void ListRequestRoundTripsWithPathAndPagination()
    {
        var request = new RetroLinkListRequest(
            7,
            Enumerable.Range(0, RetroLinkProtocol.SessionIdLength)
                .Select(value => (byte)value)
                .ToArray(),
            new RetroLinkPath(1, ["Projects", "Mac App"]),
            32,
            16);

        var decoded = RetroLinkDirectoryProtocol.DecodeListRequest(
            RetroLinkDirectoryProtocol.EncodeListRequest(request));

        Assert.Equal(request.RoleEpoch, decoded.RoleEpoch);
        Assert.Equal(request.ActiveEndpointId.ToArray(), decoded.ActiveEndpointId.ToArray());
        Assert.Equal(request.Path.ProviderId, decoded.Path.ProviderId);
        Assert.Equal(request.Path.Components, decoded.Path.Components);
        Assert.Equal(request.Cursor, decoded.Cursor);
        Assert.Equal(request.EntryLimit, decoded.EntryLimit);
    }

    [Fact]
    public void ListPageRoundTripsUtf8NamesAndMetadata()
    {
        var page = new RetroLinkListPage(
            0,
            4,
            RetroLinkDirectoryProtocol.CompleteCursor,
            [
                new RetroLinkDirectoryEntry(
                    RetroLinkDirectoryProtocol.DirectoryKind,
                    0,
                    "Caf\u00e9",
                    0,
                    0,
                    123),
                new RetroLinkDirectoryEntry(
                    RetroLinkDirectoryProtocol.FileKind,
                    1,
                    "readme.txt",
                    4096,
                    0,
                    456),
            ]);

        var decoded = RetroLinkDirectoryProtocol.DecodeListPage(
            RetroLinkDirectoryProtocol.EncodeListPage(page));

        Assert.Equal(page.ResultCode, decoded.ResultCode);
        Assert.Equal(page.Cursor, decoded.Cursor);
        Assert.Equal(page.NextCursor, decoded.NextCursor);
        Assert.Equal(page.Entries, decoded.Entries);
    }

    [Theory]
    [InlineData("")]
    [InlineData(".")]
    [InlineData("..")]
    [InlineData("a/b")]
    [InlineData("a\\b")]
    [InlineData("a:b")]
    public void ListRequestRejectsUnsafePathComponents(string component)
    {
        var request = new RetroLinkListRequest(
            1,
            new byte[RetroLinkProtocol.SessionIdLength],
            new RetroLinkPath(1, [component]),
            0,
            1);

        Assert.Throws<ArgumentException>(
            () => RetroLinkDirectoryProtocol.EncodeListRequest(request));
    }

    [Fact]
    public void ListRequestRejectsMalformedUtf8AndTrailingBytes()
    {
        var payload = RetroLinkDirectoryProtocol.EncodeListRequest(
            new RetroLinkListRequest(
                1,
                new byte[RetroLinkProtocol.SessionIdLength],
                new RetroLinkPath(1, ["folder"]),
                0,
                1));
        var componentLengthOffset = 4 + RetroLinkProtocol.SessionIdLength + 5;
        var componentOffset = componentLengthOffset + 1;
        payload[componentOffset] = 0xFF;

        Assert.Throws<InvalidDataException>(
            () => RetroLinkDirectoryProtocol.DecodeListRequest(payload));

        var validPayload = RetroLinkDirectoryProtocol.EncodeListRequest(
            new RetroLinkListRequest(
                1,
                new byte[RetroLinkProtocol.SessionIdLength],
                new RetroLinkPath(1, []),
                0,
                1));
        Array.Resize(ref validPayload, validPayload.Length + 1);
        validPayload[^1] = 0;
        Assert.Throws<InvalidDataException>(
            () => RetroLinkDirectoryProtocol.DecodeListRequest(validPayload));
    }

    [Fact]
    public void ListPageRejectsErrorEntriesAndOversizedPayloads()
    {
        Assert.Throws<ArgumentException>(
            () => RetroLinkDirectoryProtocol.EncodeListPage(
                new RetroLinkListPage(
                    1,
                    0,
                    RetroLinkDirectoryProtocol.CompleteCursor,
                    [new RetroLinkDirectoryEntry(0, 0, "x", 0, 0, 0)])));

        var entries = Enumerable.Range(0, 4)
            .Select(index => new RetroLinkDirectoryEntry(
                RetroLinkDirectoryProtocol.FileKind,
                0,
                new string((char)('a' + index), 255),
                0,
                0,
                0))
            .ToArray();
        Assert.Throws<ArgumentOutOfRangeException>(
            () => RetroLinkDirectoryProtocol.EncodeListPage(
                new RetroLinkListPage(
                    0,
                    0,
                    RetroLinkDirectoryProtocol.CompleteCursor,
                    entries)));
    }

    [Fact]
    public void ListPageRejectsUnsafeNamesFromPeer()
    {
        var payload = new byte[11 + 20 + 3];
        payload[10] = 1;
        payload[11] = RetroLinkDirectoryProtocol.FileKind;
        payload[14] = 3;
        Encoding.UTF8.GetBytes("a/b").CopyTo(payload.AsSpan(15));

        Assert.Throws<InvalidDataException>(
            () => RetroLinkDirectoryProtocol.DecodeListPage(payload));
    }

    [Fact]
    public void MakeDirectoryRequestRoundTripsWithParentPathAndName()
    {
        var request = new RetroLinkMakeDirectoryRequest(
            7,
            Enumerable.Range(0, RetroLinkProtocol.SessionIdLength)
                .Select(value => (byte)value)
                .ToArray(),
            new RetroLinkPath(12, ["Projects"]),
            "New Folder");

        var decoded = RetroLinkDirectoryProtocol.DecodeMakeDirectoryRequest(
            RetroLinkDirectoryProtocol.EncodeMakeDirectoryRequest(request));

        Assert.Equal(request.RoleEpoch, decoded.RoleEpoch);
        Assert.Equal(request.ActiveEndpointId.ToArray(), decoded.ActiveEndpointId.ToArray());
        Assert.Equal(request.ParentPath.ProviderId, decoded.ParentPath.ProviderId);
        Assert.Equal(request.ParentPath.Components, decoded.ParentPath.Components);
        Assert.Equal(request.Name, decoded.Name);
    }

    [Fact]
    public void RenameRequestRoundTripsAndRejectsCrossProviderMoves()
    {
        var request = new RetroLinkRenameRequest(
            4,
            new byte[RetroLinkProtocol.SessionIdLength],
            new RetroLinkPath(12, ["Projects", "Old Name"]),
            new RetroLinkPath(12, ["Projects"]),
            "New Name");

        var decoded = RetroLinkDirectoryProtocol.DecodeRenameRequest(
            RetroLinkDirectoryProtocol.EncodeRenameRequest(request));

        Assert.Equal(request.SourcePath.ProviderId, decoded.SourcePath.ProviderId);
        Assert.Equal(request.SourcePath.Components, decoded.SourcePath.Components);
        Assert.Equal(
            request.DestinationParentPath.Components,
            decoded.DestinationParentPath.Components);
        Assert.Equal(request.NewName, decoded.NewName);
        Assert.Throws<ArgumentException>(
            () => RetroLinkDirectoryProtocol.EncodeRenameRequest(
                request with
                {
                    DestinationParentPath = new RetroLinkPath(13, ["Projects"]),
                }));
    }

    [Fact]
    public void DeleteRequestRoundTripsAndRejectsReservedFlags()
    {
        var request = new RetroLinkDeleteRequest(
            9,
            new byte[RetroLinkProtocol.SessionIdLength],
            new RetroLinkPath(1, ["Old File"]));
        var payload = RetroLinkDirectoryProtocol.EncodeDeleteRequest(request);
        var decoded = RetroLinkDirectoryProtocol.DecodeDeleteRequest(payload);

        Assert.Equal(request.RoleEpoch, decoded.RoleEpoch);
        Assert.Equal(request.Path.ProviderId, decoded.Path.ProviderId);
        Assert.Equal(request.Path.Components, decoded.Path.Components);
        Assert.Throws<ArgumentOutOfRangeException>(
            () => RetroLinkDirectoryProtocol.EncodeDeleteRequest(request with { Flags = 1 }));

        payload[^1] = 1;
        Assert.Throws<InvalidDataException>(
            () => RetroLinkDirectoryProtocol.DecodeDeleteRequest(payload));
    }

    [Fact]
    public void DeleteRequestCanRoundTripMacNamesContainingSlashWhenEnabled()
    {
        var request = new RetroLinkDeleteRequest(
            12,
            Enumerable.Range(0, RetroLinkProtocol.SessionIdLength)
                .Select(value => (byte)value)
                .ToArray(),
            new RetroLinkPath(7, ["folder/name"]));

        var payload = RetroLinkDirectoryProtocol.EncodeDeleteRequest(
            request,
            allowSlashInComponents: true);
        var decoded = RetroLinkDirectoryProtocol.DecodeDeleteRequest(
            payload,
            allowSlashInComponents: true);

        Assert.Equal(request.Path.ProviderId, decoded.Path.ProviderId);
        Assert.Equal(request.Path.Components, decoded.Path.Components);
    }

    [Fact]
    public void OperationResultRoundTripsOptionalCanonicalName()
    {
        var result = new RetroLinkOperationResult(
            0,
            RetroLinkMessageType.Rename,
            "Canonical Name");

        Assert.Equal(
            result,
            RetroLinkDirectoryProtocol.DecodeOperationResult(
                RetroLinkDirectoryProtocol.EncodeOperationResult(result)));
        Assert.Equal(
            new RetroLinkOperationResult(5, RetroLinkMessageType.Delete),
            RetroLinkDirectoryProtocol.DecodeOperationResult(
                RetroLinkDirectoryProtocol.EncodeOperationResult(
                    new RetroLinkOperationResult(5, RetroLinkMessageType.Delete))));
    }

    [Fact]
    public void MacDirectoryNamesCanContainSlashWhenExplicitlyEnabled()
    {
        var request = new RetroLinkListRequest(
            1,
            new byte[RetroLinkProtocol.SessionIdLength],
            new RetroLinkPath(1, ["Folder/Name"]),
            0,
            1);
        Assert.Throws<ArgumentException>(
            () => RetroLinkDirectoryProtocol.EncodeListRequest(request));

        var encodedRequest = RetroLinkDirectoryProtocol.EncodeListRequest(
            request,
            allowSlashInComponents: true);
        var decodedRequest = RetroLinkDirectoryProtocol.DecodeListRequest(
            encodedRequest,
            allowSlashInComponents: true);
        Assert.Equal("Folder/Name", decodedRequest.Path.Components[0]);

        var page = new RetroLinkListPage(
            0,
            0,
            RetroLinkDirectoryProtocol.CompleteCursor,
            [new RetroLinkDirectoryEntry(0, 0, "Folder/Name", 1, 0, 0)]);
        var encodedPage = RetroLinkDirectoryProtocol.EncodeListPage(
            page,
            allowSlashInNames: true);
        var decodedPage = RetroLinkDirectoryProtocol.DecodeListPage(
            encodedPage,
            allowSlashInNames: true);
        Assert.Equal("Folder/Name", decodedPage.Entries[0].Name);
        Assert.Throws<InvalidDataException>(
            () => RetroLinkDirectoryProtocol.DecodeListPage(encodedPage));
    }
}
