namespace RetroLink;

public sealed record RetroLinkFileEntry(
    string Name,
    string Type,
    string Size,
    string Modified,
    string? FullPath,
    bool IsDirectory,
    bool IsReparsePoint,
    uint? ProviderId = null,
    IReadOnlyList<string>? PathComponents = null,
    bool IsParent = false,
    uint? ContinuationCursor = null);
