using System.Text;
using ClassicMacIde.Core;

namespace ClassicMacIde.Core.Tests;

public class RetroLinkTransferProtocolTests
{
    [Fact]
    public void CopyPrepareRoundTripsTransferMetadataAndMacPathNames()
    {
        var request = new RetroLinkCopyPrepare(
            3,
            Enumerable.Range(0, 16).Select(value => (byte)value).ToArray(),
            Enumerable.Range(16, 16).Select(value => (byte)value).ToArray(),
            new RetroLinkPath(0x80000001, ["folder"]),
            Enumerable.Range(32, 16).Select(value => (byte)value).ToArray(),
            new RetroLinkPath(1, ["Text/Doc Readers"]),
            "café.txt",
            129,
            0x12345678,
            RetroLinkTransferProtocol.RawDataFormat,
            false,
            RetroLinkTransferProtocol.XModemCrc128Mode);

        var decoded = RetroLinkTransferProtocol.DecodeCopyPrepare(
            RetroLinkTransferProtocol.EncodeCopyPrepare(request));

        Assert.Equal(request.RoleEpoch, decoded.RoleEpoch);
        Assert.Equal(request.ActiveEndpointId.ToArray(), decoded.ActiveEndpointId.ToArray());
        Assert.Equal(request.SourceEndpointId.ToArray(), decoded.SourceEndpointId.ToArray());
        Assert.Equal(request.SourcePath.ProviderId, decoded.SourcePath.ProviderId);
        Assert.Equal(request.SourcePath.Components, decoded.SourcePath.Components);
        Assert.Equal(
            request.DestinationEndpointId.ToArray(),
            decoded.DestinationEndpointId.ToArray());
        Assert.Equal(
            request.DestinationPath.ProviderId,
            decoded.DestinationPath.ProviderId);
        Assert.Equal(
            request.DestinationPath.Components,
            decoded.DestinationPath.Components);
        Assert.Equal(request.DestinationName, decoded.DestinationName);
        Assert.Equal(request.StreamLength, decoded.StreamLength);
        Assert.Equal(request.StreamCrc32, decoded.StreamCrc32);
        Assert.Equal(request.TransferFormat, decoded.TransferFormat);
        Assert.Equal(request.Overwrite, decoded.Overwrite);
        Assert.Equal(request.TransferMode, decoded.TransferMode);
    }

    [Fact]
    public void TransferReadyAndCompleteHaveFixedValidatedPayloads()
    {
        var ready = new RetroLinkTransferReady(
            0,
            129,
            0x12345678,
            RetroLinkTransferProtocol.XModemCrc128Mode,
            RetroLinkTransferProtocol.XModemCrcBlockSize);
        Assert.Equal(
            ready,
            RetroLinkTransferProtocol.DecodeTransferReady(
                RetroLinkTransferProtocol.EncodeTransferReady(ready)));

        var complete = new RetroLinkTransferComplete(0, 129, 0x12345678);
        Assert.Equal(
            complete,
            RetroLinkTransferProtocol.DecodeTransferComplete(
                RetroLinkTransferProtocol.EncodeTransferComplete(complete)));
    }

    [Fact]
    public void CopyRequestRoundTripsDestinationAndOverwritePolicy()
    {
        var request = new RetroLinkCopyRequest(
                4,
                Enumerable.Range(0, 16).Select(value => (byte)value).ToArray(),
                Enumerable.Range(16, 16).Select(value => (byte)value).ToArray(),
                new RetroLinkPath(1, ["folder", "sample.bin"]),
                Enumerable.Range(32, 16).Select(value => (byte)value).ToArray(),
                new RetroLinkPath(0x80000001, ["incoming"]),
                "sample.bin",
                true);

        var decoded = RetroLinkTransferProtocol.DecodeCopyRequest(
                RetroLinkTransferProtocol.EncodeCopyRequest(request));

        Assert.Equal(request.RoleEpoch, decoded.RoleEpoch);
        Assert.Equal(request.ActiveEndpointId.ToArray(), decoded.ActiveEndpointId.ToArray());
        Assert.Equal(request.SourceEndpointId.ToArray(), decoded.SourceEndpointId.ToArray());
        Assert.Equal(request.SourcePath.ProviderId, decoded.SourcePath.ProviderId);
        Assert.Equal(request.SourcePath.Components, decoded.SourcePath.Components);
        Assert.Equal(
                request.DestinationEndpointId.ToArray(),
                decoded.DestinationEndpointId.ToArray());
        Assert.Equal(
                request.DestinationPath.ProviderId,
                decoded.DestinationPath.ProviderId);
        Assert.Equal(
                request.DestinationPath.Components,
                decoded.DestinationPath.Components);
        Assert.Equal(request.DestinationName, decoded.DestinationName);
        Assert.Equal(request.Overwrite, decoded.Overwrite);
    }

    [Fact]
    public void CopyRequestRejectsUnsafeDestinationAndMalformedResult()
    {
        var request = new RetroLinkCopyRequest(
                1,
                new byte[16],
                Enumerable.Repeat((byte)1, 16).ToArray(),
                new RetroLinkPath(1, ["sample.bin"]),
                Enumerable.Repeat((byte)2, 16).ToArray(),
                new RetroLinkPath(0x80000001, []),
                "folder\\sample.bin",
                false);

        Assert.Throws<ArgumentException>(
                () => RetroLinkTransferProtocol.EncodeCopyRequest(request));
        Assert.Equal(
                4,
                RetroLinkTransferProtocol.DecodeCopyRequestResult(
                    RetroLinkTransferProtocol.EncodeCopyRequestResult(4)));
        Assert.Throws<InvalidDataException>(
                () => RetroLinkTransferProtocol.DecodeCopyRequestResult([0]));
    }

    [Fact]
    public void CopyPrepareRejectsUnsafeFieldsAndMalformedNames()
    {
        var request = CreateRequest() with { DestinationName = ".." };
        Assert.Throws<ArgumentException>(
            () => RetroLinkTransferProtocol.EncodeCopyPrepare(request));

        var valid = RetroLinkTransferProtocol.EncodeCopyPrepare(CreateRequest());
        var nameOffset = FindDestinationNameOffset(valid);
        valid[nameOffset + 1] = 0xFF;
        Assert.Throws<InvalidDataException>(
            () => RetroLinkTransferProtocol.DecodeCopyPrepare(valid));

        Assert.Throws<ArgumentException>(
            () => RetroLinkTransferProtocol.EncodeCopyPrepare(
                CreateRequest() with { StreamLength = (ulong)int.MaxValue + 1 }));
    }

    [Fact]
    public void Crc32IsoHdlcMatchesStandardCheckValue()
    {
        Assert.Equal(0xCBF43926u, Crc32IsoHdlc.Compute(Encoding.ASCII.GetBytes("123456789")));
        Assert.Equal(0u, Crc32IsoHdlc.Compute(ReadOnlySpan<byte>.Empty));
    }

    private static RetroLinkCopyPrepare CreateRequest() =>
        new(
            7,
            new byte[16],
            Enumerable.Repeat((byte)1, 16).ToArray(),
            new RetroLinkPath(0x80000001, []),
            Enumerable.Repeat((byte)2, 16).ToArray(),
            new RetroLinkPath(1, []),
            "sample.bin",
            42,
            0xCAFEBABE,
            RetroLinkTransferProtocol.RawDataFormat,
            false,
            RetroLinkTransferProtocol.XModemCrc128Mode);

    private static int FindDestinationNameOffset(byte[] payload)
    {
        var offset = 36;
        offset = SkipPath(payload, offset);
        offset += 16;
        offset = SkipPath(payload, offset);
        return offset;
    }

    private static int SkipPath(byte[] payload, int offset)
    {
        var componentCount = payload[offset + 4];
        offset += 5;
        for (var index = 0; index < componentCount; index++)
        {
            offset += 1 + payload[offset];
        }

        return offset;
    }
}
