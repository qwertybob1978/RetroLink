namespace ClassicMacIde.Core;

public static class Crc32IsoHdlc
{
    public static uint Compute(Stream stream)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanRead)
        {
            throw new ArgumentException("The stream must be readable.", nameof(stream));
        }

        var buffer = new byte[8192];
        var crc = uint.MaxValue;
        int read;
        while ((read = stream.Read(buffer, 0, buffer.Length)) != 0)
        {
            for (var index = 0; index < read; index++)
            {
                crc ^= buffer[index];
                for (var bit = 0; bit < 8; bit++)
                {
                    crc = (crc & 1) != 0
                        ? (crc >> 1) ^ 0xEDB88320
                        : crc >> 1;
                }
            }
        }

        return ~crc;
    }

    public static uint Compute(ReadOnlySpan<byte> data)
    {
        var crc = uint.MaxValue;
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

        return ~crc;
    }
}
