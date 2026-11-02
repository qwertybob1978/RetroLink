using ClassicMacIde.Core;

namespace ClassicMacIde.Core.Tests;

public class RetroLinkProtocolTests
{
    [Fact]
    public void EncodeAndDecodePreserveHeaderFieldsAndPayload()
    {
        var frame = CreateFrame(
            (byte)RetroLinkMessageType.ListRequest,
            requestId: 0x01020304,
            payload: [0x10, 0x20, 0x30]);

        var encoded = RetroLinkProtocol.Encode(frame);
        var decoded = Assert.Single(new RetroLinkFrameDecoder().Append(encoded));

        Assert.Equal(RetroLinkProtocol.MaximumFrameSize - 1021, encoded.Length);
        Assert.Equal("RLNK", System.Text.Encoding.ASCII.GetString(encoded, 0, 4));
        Assert.Equal(RetroLinkProtocol.Version, encoded[4]);
        Assert.Equal(0x01020304u, decoded.RequestId);
        Assert.Equal(frame.SessionId.ToArray(), decoded.SessionId.ToArray());
        Assert.Equal(frame.Payload.ToArray(), decoded.Payload.ToArray());
    }

    [Fact]
    public void DecoderWaitsForACompleteFrameAcrossAppends()
    {
        var encoded = RetroLinkProtocol.Encode(CreateFrame());
        var decoder = new RetroLinkFrameDecoder();

        Assert.Empty(decoder.Append(encoded.AsSpan(0, 13)));
        var decoded = Assert.Single(decoder.Append(encoded.AsSpan(13)));

        Assert.Equal((byte)RetroLinkMessageType.HelloAck, decoded.MessageType);
        Assert.Equal(CreateFrame().RequestId, decoded.RequestId);
        Assert.Equal(CreateFrame().Payload.ToArray(), decoded.Payload.ToArray());
    }

    [Fact]
    public void DecoderResynchronizesAfterNoiseAndBadCrc()
    {
        var invalid = RetroLinkProtocol.Encode(CreateFrame());
        invalid[^1] ^= 0xFF;
        var valid = RetroLinkProtocol.Encode(CreateFrame(
            (byte)RetroLinkMessageType.RoleState,
            requestId: 42));
        var decoder = new RetroLinkFrameDecoder();

        var frames = decoder.Append(
            new byte[] { 0x00, 0xFF, (byte)'R', (byte)'L' }
                .Concat(invalid)
                .Concat(valid)
                .ToArray());

        var decoded = Assert.Single(frames);
        Assert.Equal((byte)RetroLinkMessageType.RoleState, decoded.MessageType);
        Assert.Equal(42u, decoded.RequestId);
    }

    [Fact]
    public void DecoderRejectsInvalidHeaderAndContinuesToTheNextFrame()
    {
        var invalidVersion = RetroLinkProtocol.Encode(CreateFrame());
        invalidVersion[4] = 2;
        var invalidFlags = RetroLinkProtocol.Encode(CreateFrame());
        invalidFlags[7] = 1;
        var valid = RetroLinkProtocol.Encode(CreateFrame());
        var decoder = new RetroLinkFrameDecoder();

        var decoded = decoder.Append(
            invalidVersion.Concat(invalidFlags).Concat(valid).ToArray());

        Assert.Equal((byte)RetroLinkMessageType.HelloAck, Assert.Single(decoded).MessageType);
    }

    [Fact]
    public void EncodeRejectsInvalidSessionIdsAndOversizedPayloads()
    {
        var invalidSession = new RetroLinkFrame(
            1,
            new byte[15],
            0,
            ReadOnlyMemory<byte>.Empty);
        var oversizedPayload = new RetroLinkFrame(
            1,
            new byte[RetroLinkProtocol.SessionIdLength],
            0,
            new byte[RetroLinkProtocol.MaximumPayloadSize + 1]);

        Assert.Throws<ArgumentException>(() => RetroLinkProtocol.Encode(invalidSession));
        Assert.Throws<ArgumentOutOfRangeException>(
            () => RetroLinkProtocol.Encode(oversizedPayload));
    }

    [Fact]
    public void DecoderAcceptsMaximumPayloadFrameAndUnknownMessageTypes()
    {
        var frame = new RetroLinkFrame(
            0xF0,
            new byte[RetroLinkProtocol.SessionIdLength],
            uint.MaxValue,
            new byte[RetroLinkProtocol.MaximumPayloadSize]);

        var encoded = RetroLinkProtocol.Encode(frame);
        var decoded = Assert.Single(new RetroLinkFrameDecoder().Append(encoded));

        Assert.Equal(RetroLinkProtocol.MaximumFrameSize, encoded.Length);
        Assert.Equal(frame.MessageType, decoded.MessageType);
        Assert.Equal(frame.RequestId, decoded.RequestId);
        Assert.Equal(RetroLinkProtocol.MaximumPayloadSize, decoded.Payload.Length);
    }

    private static RetroLinkFrame CreateFrame(
        byte messageType = (byte)RetroLinkMessageType.HelloAck,
        uint requestId = 1,
        byte[]? payload = null) =>
        new(
            messageType,
            Enumerable.Range(0, RetroLinkProtocol.SessionIdLength)
                .Select(value => (byte)value)
                .ToArray(),
            requestId,
            payload ?? [0x01, 0x02, 0x03]);
}
