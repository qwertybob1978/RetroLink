using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Buffers.Binary;
using System.IO;
using System.IO.Ports;
using System.Security.Cryptography;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;
using ClassicMacIde.Core;

namespace RetroLink;

public partial class MainWindow : Window
{
    private const uint LocalShareProviderId = 0x80000001;
    private static readonly TimeSpan PendingActionTimeout = TimeSpan.FromSeconds(10);
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private readonly byte[] _endpointId =
        RandomNumberGenerator.GetBytes(RetroLinkProtocol.SessionIdLength);
    private SerialPort? _serialPort;
    private RetroLinkSession? _session;
    private CancellationTokenSource? _receiveCancellation;
    private string? _localRootPath;
    private string? _localCurrentPath;
    private uint _roleEpoch;
    private ReadOnlyMemory<byte> _activeEndpointId;
    private RetroLinkPath? _remoteCurrentPath;
    private RetroLinkPath? _remoteRequestedPath;
    private uint? _remotePendingRequestId;
    private uint _remoteRequestedCursor;
    private uint _remoteRequestRoleEpoch;
    private CancellationTokenSource? _pendingActionTimeout;
    private PendingOutboundTransfer? _outboundTransfer;
    private PendingFileOperation? _pendingFileOperation;
    private bool _isConnecting;
    private bool _remotePaneActive;

    private sealed record PendingOutboundTransfer(
        uint RequestId,
        FileStream Stream,
        ulong Length,
        uint Crc32);

    private sealed record PendingFileOperation(
        uint RequestId,
        RetroLinkMessageType Kind,
        bool IsRemote,
        RetroLinkPath? RemoteRefreshPath);

    public MainWindow()
    {
        InitializeComponent();
        DataContext = this;
        RefreshComPorts();
        UpdateConnectionControls();
    }

    public ObservableCollection<RetroLinkFileEntry> LocalEntries { get; } = [];

    public ObservableCollection<RetroLinkFileEntry> RemoteEntries { get; } = [];

    private void RefreshPorts_Click(object sender, RoutedEventArgs e) => RefreshComPorts();

    private void RefreshLocal_Click(object sender, RoutedEventArgs e) =>
        RefreshLocalFiles();

    private async void ChooseLocalRoot_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog
        {
            Title = "Choose the local RetroLink root",
            InitialDirectory = _localRootPath ?? Environment.GetFolderPath(
                Environment.SpecialFolder.UserProfile),
        };
        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        try
        {
            var root = Path.GetFullPath(dialog.FolderName);
            if (!Directory.Exists(root))
            {
                throw new DirectoryNotFoundException(
                    $"The selected local root does not exist: {root}");
            }
            if ((File.GetAttributes(root) & FileAttributes.ReparsePoint) != 0)
            {
                throw new UnauthorizedAccessException(
                    "The selected local root cannot be a symbolic link or reparse point.");
            }

            _localRootPath = root;
            _localCurrentPath = root;
            LocalRootText.Text = root;
            LocalPathText.Text = root;
            await RefreshLocalFilesAsync();
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            SetStatus($"Could not open the local root: {error.Message}");
        }
    }

    private async void LocalUp_Click(object sender, RoutedEventArgs e)
    {
        if (_localRootPath is null || _localCurrentPath is null ||
            string.Equals(
                _localCurrentPath,
                _localRootPath,
                StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        var parent = Directory.GetParent(_localCurrentPath)?.FullName;
        if (parent is null || !IsWithinRoot(parent, _localRootPath))
        {
            SetStatus("The local parent directory is outside the selected root.");
            return;
        }

        _localCurrentPath = parent;
        LocalPathText.Text = parent;
        await RefreshLocalFilesAsync();
    }

    private async void RemoteFileListView_MouseDoubleClick(
        object sender,
        MouseButtonEventArgs e)
    {
        if (RemoteFileListView.SelectedItem is not RetroLinkFileEntry entry)
        {
            return;
        }

        if (_session is null)
        {
            SetStatus("Connect to a REMOTE RetroLink peer before browsing its volumes.");
            return;
        }

        if (!_activeEndpointId.Span.SequenceEqual(_endpointId))
        {
            SetStatus("Take Control before browsing remote volumes.");
            return;
        }

        if (entry.ContinuationCursor is uint cursor &&
            _remoteCurrentPath is not null)
        {
            await RequestRemoteListAsync(_remoteCurrentPath, cursor);
            return;
        }

        if (entry.IsParent && _remoteCurrentPath is not null)
        {
            await NavigateRemoteUpAsync();
            return;
        }

        if (!entry.IsDirectory || entry.ProviderId is not uint providerId)
        {
            SetStatus($"Selected REMOTE file '{entry.Name}'; file transfer is not available yet.");
            return;
        }

        await RequestRemoteListAsync(
            new RetroLinkPath(providerId, entry.PathComponents ?? []),
            0);
    }

    private async void RemoteUp_Click(object sender, RoutedEventArgs e)
    {
        await NavigateRemoteUpAsync();
    }

    private async Task NavigateRemoteUpAsync()
    {
        var path = _remoteCurrentPath;
        if (path is null)
        {
            return;
        }

        var parentPath = path.GetParentPath();
        if (parentPath is null)
        {
            ShowRemoteRoots();
            return;
        }

        await RequestRemoteListAsync(parentPath, 0);
    }

    private async void LocalFileListView_MouseDoubleClick(
        object sender,
        MouseButtonEventArgs e)
    {
        if (LocalFileListView.SelectedItem is not RetroLinkFileEntry
            {
                IsDirectory: true,
                IsReparsePoint: false,
                FullPath: not null,
            } entry ||
            _localRootPath is null ||
            !IsWithinRoot(entry.FullPath, _localRootPath))
        {
            return;
        }

        _localCurrentPath = entry.FullPath;
        LocalPathText.Text = entry.FullPath;
        await RefreshLocalFilesAsync();
    }

    private async void CopyToMac_Click(object sender, RoutedEventArgs e)
    {
        if (_session is null ||
            !_activeEndpointId.Span.SequenceEqual(_endpointId))
        {
            SetStatus("LOCAL must own control before transferring a file.");
            return;
        }

        if (_localRootPath is null ||
            _localRootPath.Length == 0 ||
            _localCurrentPath is null)
        {
            SetStatus("Choose a local root before transferring a file.");
            return;
        }

        if (_remoteCurrentPath is null)
        {
            SetStatus("Open a REMOTE volume and destination folder first.");
            return;
        }

        if (LocalFileListView.SelectedItem is not RetroLinkFileEntry
            {
                IsDirectory: false,
                IsReparsePoint: false,
                FullPath: not null,
            } entry ||
            !IsWithinRoot(entry.FullPath, _localRootPath))
        {
            SetStatus("Select a regular LOCAL file to copy to REMOTE.");
            return;
        }

        var session = _session;
        if (session.PeerInfo is not { } peer)
        {
            SetStatus("REMOTE peer information is not available.");
            return;
        }

        FileStream? stream = null;
        try
        {
            stream = new FileStream(
                entry.FullPath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                bufferSize: 8192,
                useAsync: true);
            if (stream.Length > RetroLinkTransferProtocol.MaximumTransferSize)
            {
                throw new InvalidDataException(
                    $"Files larger than {RetroLinkTransferProtocol.MaximumTransferSize:N0} bytes are not supported.");
            }

            var fileLength = checked((ulong)stream.Length);
            var crc32 = await Task.Run(() => Crc32IsoHdlc.Compute(stream));
            stream.Position = 0;
            var relativePath = Path.GetRelativePath(_localRootPath, entry.FullPath);
            var components = relativePath == "."
                ? Array.Empty<string>()
                : relativePath.Split(
                    [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                    StringSplitOptions.RemoveEmptyEntries);
            var requestId = session.NextRequestId();
            var request = new RetroLinkCopyPrepare(
                _roleEpoch,
                _endpointId,
                _endpointId,
                new RetroLinkPath(LocalShareProviderId, components),
                peer.EndpointId,
                _remoteCurrentPath,
                entry.Name,
                fileLength,
                crc32,
                RetroLinkTransferProtocol.RawDataFormat,
                Overwrite: false,
                TransferMode: RetroLinkTransferProtocol.XModemCrc128Mode);
            var payload = RetroLinkTransferProtocol.EncodeCopyPrepare(request);
            _outboundTransfer = new PendingOutboundTransfer(
                requestId,
                stream,
                fileLength,
                crc32);
            stream = null;
            TransferProgressBar.Value = 0;
            TransferProgressBar.Visibility = Visibility.Visible;
            CopyToMacButton.IsEnabled = false;
            await session.SendAsync(
                (byte)RetroLinkMessageType.CopyPrepare,
                requestId,
                payload);
            SetStatus(
                $"Preparing {entry.Name} for transfer to REMOTE ({fileLength:N0} bytes).");
            StartPendingActionTimeout(
                requestId,
                () => FailOutboundTransfer(
                    "Timeout: REMOTE did not prepare the destination within 10 seconds."));
        }
        catch (Exception error) when (
            error is IOException or
                UnauthorizedAccessException or
                ArgumentException or
                InvalidOperationException or
                OverflowException or
                ObjectDisposedException)
        {
            stream?.Dispose();
            FailOutboundTransfer($"Could not prepare the file transfer: {error.Message}");
        }
    }

    private void FileSelection_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (e.AddedItems.Count != 0)
        {
            _remotePaneActive = ReferenceEquals(sender, RemoteFileListView);
        }

        UpdateCopyButtonState();
    }

    private async void CreateDirectory_Click(object sender, RoutedEventArgs e)
    {
        var name = PromptForName("Create Folder", "New folder name:");
        if (name is null)
        {
            return;
        }

        if (!IsValidOperationName(name))
        {
            SetStatus("Enter a valid file name (1-255 UTF-8 bytes, without path separators).");
            return;
        }

        if (_remotePaneActive)
        {
            if (_remoteCurrentPath is null)
            {
                SetStatus("Open a REMOTE volume and destination folder first.");
                return;
            }

            await SendRemoteFileOperationAsync(
                RetroLinkMessageType.MakeDirectory,
                RetroLinkDirectoryProtocol.EncodeMakeDirectoryRequest(
                    new RetroLinkMakeDirectoryRequest(
                        _roleEpoch,
                        _endpointId,
                        _remoteCurrentPath,
                        name),
                    allowSlashInComponents: true),
                _remoteCurrentPath);
            return;
        }

        if (_localRootPath is null || _localCurrentPath is null)
        {
            SetStatus("Choose a local root before creating a folder.");
            return;
        }

        if (!CanMutateLocalFiles())
        {
            return;
        }

        try
        {
            var parent = Path.GetFullPath(_localCurrentPath);
            if (!IsWithinRoot(parent, _localRootPath) ||
                (File.GetAttributes(parent) & FileAttributes.ReparsePoint) != 0)
            {
                throw new UnauthorizedAccessException("The selected directory is outside the share root.");
            }

            var target = Path.Combine(parent, name);
            if (Directory.Exists(target) || File.Exists(target))
            {
                SetStatus($"An entry named '{name}' already exists.");
                return;
            }

            Directory.CreateDirectory(target);
            await RefreshLocalFilesAsync();
            SetStatus($"Created folder '{name}'.");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            SetStatus($"Could not create folder: {error.Message}");
        }
    }

    private async void RenameEntry_Click(object sender, RoutedEventArgs e)
    {
        if (_remotePaneActive)
        {
            if (!TryGetSelectedRemoteEntry(out var remoteEntry, out var remotePath))
            {
                SetStatus("Select a REMOTE file or folder to rename.");
                return;
            }

            var remoteName = PromptForName("Rename", "New name:", remoteEntry.Name);
            if (remoteName is null)
            {
                return;
            }

            if (!IsValidOperationName(remoteName))
            {
                SetStatus("Enter a valid file name (1-255 UTF-8 bytes, without path separators).");
                return;
            }

            var parent = _remoteCurrentPath!;
            await SendRemoteFileOperationAsync(
                RetroLinkMessageType.Rename,
                RetroLinkDirectoryProtocol.EncodeRenameRequest(
                    new RetroLinkRenameRequest(
                        _roleEpoch,
                        _endpointId,
                        remotePath,
                        parent,
                        remoteName),
                    allowSlashInComponents: true),
                parent);
            return;
        }

        if (!TryGetSelectedLocalPath(out var localEntry, out var localPath))
        {
            SetStatus("Select a regular local file or folder to rename.");
            return;
        }
        if (!CanMutateLocalFiles())
        {
            return;
        }

        var localName = PromptForName("Rename", "New name:", localEntry.Name);
        if (localName is null)
        {
            return;
        }

        if (!IsValidOperationName(localName))
        {
            SetStatus("Enter a valid file name (1-255 UTF-8 bytes, without path separators).");
            return;
        }

        try
        {
            var destination = Path.Combine(_localCurrentPath!, localName);
            if (File.Exists(destination) || Directory.Exists(destination))
            {
                SetStatus($"An entry named '{localName}' already exists.");
                return;
            }

            if (localEntry.IsDirectory)
            {
                Directory.Move(localPath, destination);
            }
            else
            {
                File.Move(localPath, destination);
            }

            await RefreshLocalFilesAsync();
            SetStatus($"Renamed '{localEntry.Name}' to '{localName}'.");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            SetStatus($"Could not rename entry: {error.Message}");
        }
    }

    private async void DeleteEntry_Click(object sender, RoutedEventArgs e)
    {
        if (_remotePaneActive)
        {
            if (!TryGetSelectedRemoteEntry(out var remoteEntry, out var remotePath))
            {
                SetStatus("Select a REMOTE file or folder to delete.");
                return;
            }

            if (MessageBox.Show(
                    this,
                    $"Delete '{remoteEntry.Name}'? Directories must be empty.",
                    "Confirm Delete",
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Warning) != MessageBoxResult.Yes)
            {
                return;
            }

            var remoteRefreshPath = _remoteCurrentPath;
            if (remoteRefreshPath is null)
            {
                SetStatus("Open a REMOTE volume and destination folder first.");
                return;
            }

            await SendRemoteFileOperationAsync(
                RetroLinkMessageType.Delete,
                RetroLinkDirectoryProtocol.EncodeDeleteRequest(
                    new RetroLinkDeleteRequest(_roleEpoch, _endpointId, remotePath),
                    allowSlashInComponents: true),
                remoteRefreshPath);
            return;
        }

        if (!TryGetSelectedLocalPath(out var localEntry, out var localPath))
        {
            SetStatus("Select a regular local file or folder to delete.");
            return;
        }
        if (!CanMutateLocalFiles())
        {
            return;
        }

        if (MessageBox.Show(
                this,
                $"Delete '{localEntry.Name}'? Directories must be empty.",
                "Confirm Delete",
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning) != MessageBoxResult.Yes)
        {
            return;
        }

        try
        {
            if (localEntry.IsDirectory)
            {
                Directory.Delete(localPath);
            }
            else
            {
                File.Delete(localPath);
            }

            await RefreshLocalFilesAsync();
            SetStatus($"Deleted '{localEntry.Name}'.");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            SetStatus($"Could not delete entry: {error.Message}");
        }
    }

    private static string? PromptForName(
        string title,
        string prompt,
        string defaultName = "")
    {
        var name = Microsoft.VisualBasic.Interaction.InputBox(prompt, title, defaultName);
        return name.Length == 0 ? null : name;
    }

    private bool CanMutateLocalFiles()
    {
        if (_session is not null &&
            !_activeEndpointId.Span.SequenceEqual(_endpointId))
        {
            SetStatus("LOCAL must own control before changing local files.");
            return false;
        }

        return true;
    }

    private static bool IsValidOperationName(string name)
    {
        if (name.Length == 0 ||
            name is "." or ".." ||
            name.EndsWith(' ') ||
            name.EndsWith('.') ||
            name.Any(character =>
                char.IsControl(character) || Path.GetInvalidFileNameChars().Contains(character)))
        {
            return false;
        }

        try
        {
            return StrictUtf8.GetByteCount(name) <=
                   RetroLinkDirectoryProtocol.MaximumNameBytes;
        }
        catch (EncoderFallbackException)
        {
            return false;
        }
    }

    private bool TryGetSelectedLocalPath(
        out RetroLinkFileEntry entry,
        out string fullPath)
    {
        entry = null!;
        fullPath = string.Empty;
        if (_localRootPath is null ||
            _localCurrentPath is null ||
            LocalFileListView.SelectedItem is not RetroLinkFileEntry selected ||
            selected.IsParent ||
            selected.ContinuationCursor is not null ||
            selected.IsReparsePoint ||
            selected.FullPath is null ||
            !IsWithinRoot(selected.FullPath, _localRootPath))
        {
            return false;
        }

        entry = selected;
        fullPath = selected.FullPath;
        return true;
    }

    private bool TryGetSelectedRemoteEntry(
        out RetroLinkFileEntry entry,
        out RetroLinkPath path)
    {
        entry = null!;
        path = null!;
        if (_remoteCurrentPath is null ||
            RemoteFileListView.SelectedItem is not RetroLinkFileEntry selected ||
            selected.IsParent ||
            selected.ContinuationCursor is not null ||
            selected.ProviderId != _remoteCurrentPath.ProviderId ||
            selected.PathComponents is null ||
            selected.PathComponents.Count == 0 ||
            _session?.PeerInfo?.Roots.FirstOrDefault(
                root => root.ProviderId == selected.ProviderId)?.IsReadOnly != false)
        {
            return false;
        }

        entry = selected;
        path = new RetroLinkPath(selected.ProviderId!.Value, selected.PathComponents);
        return true;
    }

    private async Task SendRemoteFileOperationAsync(
        RetroLinkMessageType kind,
        byte[] payload,
        RetroLinkPath refreshPath)
    {
        var session = _session;
        if (session is null ||
            !_activeEndpointId.Span.SequenceEqual(_endpointId))
        {
            SetStatus("LOCAL must own control before changing remote files.");
            return;
        }

        if (_pendingFileOperation is not null)
        {
            SetStatus("Wait for the pending file operation to finish.");
            return;
        }

        var requestId = session.NextRequestId();
        _pendingFileOperation = new PendingFileOperation(
            requestId,
            kind,
            true,
            refreshPath);
        try
        {
            await session.SendAsync((byte)kind, requestId, payload);
            SetStatus($"Sent {kind} request to REMOTE.");
            StartPendingActionTimeout(
                requestId,
                () =>
                {
                    _pendingFileOperation = null;
                    SetStatus($"Timeout: REMOTE did not complete {kind} within 10 seconds.");
                });
        }
        catch (Exception error) when (
            error is IOException or InvalidOperationException or ObjectDisposedException)
        {
            _pendingFileOperation = null;
            SetStatus($"Could not send {kind} request to REMOTE: {error.Message}");
        }
    }

    private async void Connect_Click(object sender, RoutedEventArgs e)
    {
        if (_isConnecting || _session is not null)
        {
            return;
        }

        var portName = ComPortComboBox.Text.Trim().ToUpperInvariant();
        if (string.IsNullOrWhiteSpace(portName))
        {
            SetStatus("Select the COM port paired with the REMOTE RetroLink peer.");
            return;
        }

        if (portName is "COM30" or "COM31")
        {
            SetStatus("COM30 and COM31 are reserved for the separate MacStudio Agent link.");
            return;
        }

        if (BaudRateComboBox.SelectedItem is not ComboBoxItem baudItem ||
            !uint.TryParse(baudItem.Tag?.ToString(), out var baudRate))
        {
            SetStatus("Select a supported baud rate.");
            return;
        }

        _isConnecting = true;
        UpdateConnectionControls();
        SerialPort? port = null;
        try
        {
            port = new SerialPort(
                portName,
                checked((int)baudRate),
                Parity.None,
                8,
                StopBits.One)
            {
                Handshake = Handshake.None,
                ReadTimeout = 1000,
                WriteTimeout = 1000,
            };
            port.Open();

            var session = new RetroLinkSession(
                port.BaseStream,
                _endpointId,
                maximumAttempts: 1,
                responseTimeout: PendingActionTimeout);
            var peer = await session.ConnectAsync(baudRate);
            _serialPort = port;
            _session = session;
            port = null;
            _roleEpoch = 1;
            _activeEndpointId = _endpointId;
            UpdatePeerInfo(peer);
            await SendRoleStateAsync();
            _receiveCancellation = new CancellationTokenSource();
            _ = ReceiveLoopAsync(session, _receiveCancellation.Token);
            SetStatus(
                $"Connected to REMOTE at {baudRate} baud. LOCAL has initial control.");
        }
        catch (Exception error) when (
            error is IOException or
                UnauthorizedAccessException or
                ArgumentException or
                InvalidOperationException or
                TimeoutException or
                InvalidDataException or
                System.ComponentModel.Win32Exception)
        {
            port?.Dispose();
            DisconnectSession();
            SetStatus(error is TimeoutException
                ? $"Timeout: no RetroLink HELLO response within {PendingActionTimeout.TotalSeconds:N0} seconds."
                : $"Could not connect: {error.Message}");
        }
        finally
        {
            _isConnecting = false;
            UpdateConnectionControls();
        }
    }

    private void Disconnect_Click(object sender, RoutedEventArgs e) =>
        DisconnectSession("Disconnected.");

    private async void TakeControl_Click(object sender, RoutedEventArgs e)
    {
        if (_session is null)
        {
            return;
        }

        if (_pendingFileOperation is not null)
        {
            SetStatus("Wait for the pending file operation before changing control.");
            return;
        }

        _roleEpoch = unchecked(_roleEpoch + 1);
        _activeEndpointId = _endpointId;
        try
        {
            await SendRoleStateAsync();
            UpdateRoleText();
            SetStatus("LOCAL is ACTIVE.");
        }
        catch (Exception error) when (
            error is IOException or InvalidOperationException or ObjectDisposedException)
        {
            DisconnectSession($"Could not change active endpoint: {error.Message}");
        }
    }

    private void Window_Closing(object? sender, CancelEventArgs e) =>
        DisconnectSession();

    private void RefreshComPorts()
    {
        var selectedPort = ComPortComboBox.Text;
        try
        {
            ComPortComboBox.Items.Clear();
            foreach (var portName in SerialPort.GetPortNames()
                         .Where(port => port is not "COM30" and not "COM31")
                         .OrderBy(port => port, StringComparer.OrdinalIgnoreCase))
            {
                ComPortComboBox.Items.Add(portName);
            }

            ComPortComboBox.Text = selectedPort;
            SetStatus(
                "COM30/COM31 are excluded because they belong to the separate MacStudio Agent link.");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            SetStatus($"Could not enumerate COM ports: {error.Message}");
        }
    }

    private async Task RefreshLocalFilesAsync()
    {
        if (_localRootPath is null || _localCurrentPath is null)
        {
            return;
        }

        try
        {
            var entries = await Task.Run(
                () => EnumerateLocalEntries(_localCurrentPath, _localRootPath));
            LocalEntries.Clear();
            foreach (var entry in entries)
            {
                LocalEntries.Add(entry);
            }

            LocalPathText.Text = _localCurrentPath;
            LocalRootText.Text = _localRootPath;
            SetStatus($"Loaded {entries.Count} local entries.");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            SetStatus($"Could not list the local directory: {error.Message}");
        }
    }

    private void RefreshLocalFiles()
    {
        _ = RefreshLocalFilesAsync();
    }

    private static IReadOnlyList<RetroLinkFileEntry> EnumerateLocalEntries(
        string currentPath,
        string rootPath)
    {
        if (!IsWithinRoot(currentPath, rootPath))
        {
            throw new UnauthorizedAccessException(
                "The requested directory is outside the selected local root.");
        }

        return Directory.EnumerateFileSystemEntries(currentPath)
            .Select(path =>
            {
                var fullPath = Path.GetFullPath(path);
                if (!IsWithinRoot(fullPath, rootPath))
                {
                    throw new UnauthorizedAccessException(
                        $"An entry resolved outside the selected root: {path}");
                }

                var attributes = File.GetAttributes(fullPath);
                var isDirectory = (attributes & FileAttributes.Directory) != 0;
                var isReparsePoint = (attributes & FileAttributes.ReparsePoint) != 0;
                var size = isDirectory
                    ? "<DIR>"
                    : $"{new FileInfo(fullPath).Length:N0} bytes";
                return new RetroLinkFileEntry(
                    Path.GetFileName(fullPath),
                    isDirectory ? "Folder" : "File",
                    size,
                    File.GetLastWriteTime(fullPath).ToString("g"),
                    fullPath,
                    isDirectory,
                    isReparsePoint);
            })
            .OrderByDescending(entry => entry.IsDirectory)
            .ThenBy(entry => entry.Name, StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static bool IsWithinRoot(string path, string rootPath)
    {
        var root = Path.GetFullPath(rootPath);
        var fullPath = Path.GetFullPath(path);
        var relativePath = Path.GetRelativePath(root, fullPath);
        return relativePath == "." ||
               (!Path.IsPathRooted(relativePath) &&
                relativePath != ".." &&
                !relativePath.StartsWith(
                    $"..{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal) &&
                !relativePath.StartsWith(
                    $"..{Path.AltDirectorySeparatorChar}",
                    StringComparison.Ordinal));
    }

    private async Task ReceiveLoopAsync(
        RetroLinkSession session,
        CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var frame = await session.ReadAsync(cancellationToken);
                await Dispatcher.InvokeAsync(
                    () => HandleIncomingFrameAsync(session, frame)).Task.Unwrap();
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error) when (
            error is IOException or
                InvalidDataException or
                InvalidOperationException or
                ObjectDisposedException)
        {
            await Dispatcher.InvokeAsync(
                () => DisconnectSession($"Serial session ended: {error.Message}"));
        }
    }

    private async Task HandleIncomingFrameAsync(
        RetroLinkSession session,
        RetroLinkFrame frame)
    {
        if (!ReferenceEquals(_session, session))
        {
            return;
        }

        switch ((RetroLinkMessageType)frame.MessageType)
        {
            case RetroLinkMessageType.RoleRequest:
                await HandleRoleRequestAsync(frame.Payload);
                break;
            case RetroLinkMessageType.Heartbeat:
                SetStatus("REMOTE peer heartbeat received.");
                break;
            case RetroLinkMessageType.ListRequest:
                await HandleListRequestAsync(frame);
                break;
            case RetroLinkMessageType.ListPage:
                HandleRemoteListPage(frame);
                break;
            case RetroLinkMessageType.MakeDirectory:
            case RetroLinkMessageType.Rename:
            case RetroLinkMessageType.Delete:
                await HandleFileOperationRequestAsync(
                    frame,
                    (RetroLinkMessageType)frame.MessageType);
                break;
            case RetroLinkMessageType.OperationResult:
                await HandleOperationResultAsync(frame);
                break;
            case RetroLinkMessageType.TransferReady:
                await HandleTransferReadyAsync(frame);
                break;
            case RetroLinkMessageType.TransferComplete:
                HandleTransferComplete(frame);
                break;
            case RetroLinkMessageType.Error:
                if (_outboundTransfer?.RequestId == frame.RequestId)
                {
                    FailOutboundTransfer(FormatPeerError(frame.Payload.ToArray()));
                    break;
                }
                SetStatus(FormatPeerError(frame.Payload.ToArray()));
                break;
            default:
                SetStatus($"Ignored unsupported REMOTE message type 0x{frame.MessageType:X2}.");
                break;
        }
    }

    private async Task HandleListRequestAsync(RetroLinkFrame frame)
    {
        RetroLinkListRequest request;
        try
        {
            request = RetroLinkDirectoryProtocol.DecodeListRequest(frame.Payload.Span);
        }
        catch (InvalidDataException error)
        {
            await SendProtocolErrorAsync(frame.RequestId, 1, error.Message);
            SetStatus($"Rejected malformed directory request: {error.Message}");
            return;
        }

        var peerEndpointId = _session?.PeerInfo?.EndpointId;
        if (peerEndpointId is null ||
            request.RoleEpoch != _roleEpoch ||
            !request.ActiveEndpointId.Span.SequenceEqual(peerEndpointId.Value.Span) ||
            !_activeEndpointId.Span.SequenceEqual(peerEndpointId.Value.Span))
        {
            await SendProtocolErrorAsync(
                frame.RequestId,
                2,
                "The requester does not hold the current active role.");
            SetStatus("Rejected a directory request from a non-active endpoint.");
            return;
        }

        if (request.Path.ProviderId != LocalShareProviderId)
        {
            await SendListPageAsync(
                frame.RequestId,
                new RetroLinkListPage(7, request.Cursor, RetroLinkDirectoryProtocol.CompleteCursor, []));
            SetStatus("REMOTE requested an unknown LOCAL share provider.");
            return;
        }

        if (_localRootPath is null)
        {
            await SendListPageAsync(
                frame.RequestId,
                new RetroLinkListPage(3, request.Cursor, RetroLinkDirectoryProtocol.CompleteCursor, []));
            SetStatus("REMOTE requested LOCAL files, but no local root is selected.");
            return;
        }

        try
        {
            var entries = await Task.Run(
                () => EnumerateShareEntries(_localRootPath, request.Path));
            var page = CreateListPage(entries, request.Cursor, request.EntryLimit);
            await SendListPageAsync(frame.RequestId, page);
            SetStatus(
                $"Sent {page.Entries.Count} LOCAL entries to REMOTE " +
                $"(cursor {page.Cursor}).");
        }
        catch (DirectoryNotFoundException error)
        {
            await SendListPageAsync(
                frame.RequestId,
                new RetroLinkListPage(1, request.Cursor, RetroLinkDirectoryProtocol.CompleteCursor, []));
            SetStatus($"REMOTE requested a missing LOCAL directory: {error.Message}");
        }
        catch (UnauthorizedAccessException error)
        {
            await SendListPageAsync(
                frame.RequestId,
                new RetroLinkListPage(3, request.Cursor, RetroLinkDirectoryProtocol.CompleteCursor, []));
            SetStatus($"REMOTE requested an inaccessible LOCAL directory: {error.Message}");
        }
        catch (ArgumentException error)
        {
            await SendListPageAsync(
                frame.RequestId,
                new RetroLinkListPage(2, request.Cursor, RetroLinkDirectoryProtocol.CompleteCursor, []));
            SetStatus($"REMOTE requested an invalid LOCAL path: {error.Message}");
        }
        catch (IOException error)
        {
            await SendListPageAsync(
                frame.RequestId,
                new RetroLinkListPage(8, request.Cursor, RetroLinkDirectoryProtocol.CompleteCursor, []));
            SetStatus($"Could not list LOCAL files: {error.Message}");
        }
    }

    private async Task HandleFileOperationRequestAsync(
        RetroLinkFrame frame,
        RetroLinkMessageType kind)
    {
        RetroLinkPath? parentPath = null;
        RetroLinkPath? sourcePath = null;
        string? name = null;
        try
        {
            switch (kind)
            {
                case RetroLinkMessageType.MakeDirectory:
                    var makeDirectory = RetroLinkDirectoryProtocol.DecodeMakeDirectoryRequest(
                        frame.Payload.Span);
                    parentPath = makeDirectory.ParentPath;
                    name = makeDirectory.Name;
                    ValidateOperationRequester(
                        makeDirectory.RoleEpoch,
                        makeDirectory.ActiveEndpointId);
                    break;
                case RetroLinkMessageType.Rename:
                    var rename = RetroLinkDirectoryProtocol.DecodeRenameRequest(
                        frame.Payload.Span);
                    sourcePath = rename.SourcePath;
                    parentPath = rename.DestinationParentPath;
                    name = rename.NewName;
                    ValidateOperationRequester(rename.RoleEpoch, rename.ActiveEndpointId);
                    break;
                case RetroLinkMessageType.Delete:
                    var delete = RetroLinkDirectoryProtocol.DecodeDeleteRequest(
                        frame.Payload.Span);
                    sourcePath = delete.Path;
                    ValidateOperationRequester(delete.RoleEpoch, delete.ActiveEndpointId);
                    break;
            }
        }
        catch (InvalidDataException error)
        {
            await SendOperationResultAsync(
                frame.RequestId,
                new RetroLinkOperationResult(2, kind));
            SetStatus($"Rejected malformed {kind} request: {error.Message}");
            return;
        }
        catch (UnauthorizedAccessException error)
        {
            await SendOperationResultAsync(
                frame.RequestId,
                new RetroLinkOperationResult(3, kind));
            SetStatus($"Rejected unauthorized {kind} request: {error.Message}");
            return;
        }

        if ((parentPath?.ProviderId ?? sourcePath?.ProviderId) != LocalShareProviderId)
        {
            await SendOperationResultAsync(
                frame.RequestId,
                new RetroLinkOperationResult(7, kind));
            SetStatus($"REMOTE requested an unsupported LOCAL provider for {kind}.");
            return;
        }

        if (_localRootPath is null)
        {
            await SendOperationResultAsync(
                frame.RequestId,
                new RetroLinkOperationResult(3, kind));
            SetStatus($"Rejected {kind}: no local share root is selected.");
            return;
        }

        var result = ApplyShareOperation(
            _localRootPath,
            kind,
            parentPath,
            sourcePath,
            name);
        await SendOperationResultAsync(frame.RequestId, result);
        if (result.ResultCode == 0)
        {
            SetStatus($"REMOTE completed {kind} on LOCAL files.");
            await RefreshLocalFilesAsync();
        }
        else
        {
            SetStatus($"REMOTE {kind} failed (result {result.ResultCode}).");
        }
    }

    private void ValidateOperationRequester(
        uint roleEpoch,
        ReadOnlyMemory<byte> activeEndpointId)
    {
        var peerEndpointId = _session?.PeerInfo?.EndpointId;
        if (peerEndpointId is null ||
            roleEpoch != _roleEpoch ||
            !activeEndpointId.Span.SequenceEqual(peerEndpointId.Value.Span) ||
            !_activeEndpointId.Span.SequenceEqual(peerEndpointId.Value.Span))
        {
            throw new UnauthorizedAccessException(
                "The requester does not hold the current active role.");
        }
    }

    private static RetroLinkOperationResult ApplyShareOperation(
        string rootPath,
        RetroLinkMessageType kind,
        RetroLinkPath? parentPath,
        RetroLinkPath? sourcePath,
        string? name)
    {
        try
        {
            switch (kind)
            {
                case RetroLinkMessageType.MakeDirectory:
                {
                    if (parentPath is null ||
                        name is null ||
                        !IsValidOperationName(name))
                    {
                        return new RetroLinkOperationResult(2, kind);
                    }

                    var parent = ResolveShareDirectory(rootPath, parentPath);
                    var target = Path.Combine(parent, name);
                    if (File.Exists(target) || Directory.Exists(target))
                    {
                        return new RetroLinkOperationResult(4, kind);
                    }

                    Directory.CreateDirectory(target);
                    return new RetroLinkOperationResult(0, kind, name);
                }
                case RetroLinkMessageType.Rename:
                {
                    if (sourcePath is null ||
                        parentPath is null ||
                        name is null ||
                        !IsValidOperationName(name) ||
                        sourcePath.ProviderId != parentPath.ProviderId)
                    {
                        return new RetroLinkOperationResult(2, kind);
                    }

                    var (source, attributes) = ResolveShareObject(rootPath, sourcePath);
                    var parent = ResolveShareDirectory(rootPath, parentPath);
                    var destination = Path.Combine(parent, name);
                    if (File.Exists(destination) || Directory.Exists(destination))
                    {
                        return new RetroLinkOperationResult(4, kind);
                    }

                    if ((attributes & FileAttributes.Directory) != 0)
                    {
                        Directory.Move(source, destination);
                    }
                    else
                    {
                        File.Move(source, destination);
                    }

                    return new RetroLinkOperationResult(0, kind, name);
                }
                case RetroLinkMessageType.Delete:
                {
                    if (sourcePath is null)
                    {
                        return new RetroLinkOperationResult(2, kind);
                    }

                    var (target, attributes) = ResolveShareObject(rootPath, sourcePath);
                    if ((attributes & FileAttributes.Directory) != 0)
                    {
                        if (Directory.EnumerateFileSystemEntries(target).Any())
                        {
                            return new RetroLinkOperationResult(5, kind);
                        }

                        Directory.Delete(target);
                    }
                    else
                    {
                        File.Delete(target);
                    }

                    return new RetroLinkOperationResult(0, kind);
                }
                default:
                    return new RetroLinkOperationResult(7, kind);
            }
        }
        catch (FileNotFoundException)
        {
            return new RetroLinkOperationResult(1, kind);
        }
        catch (DirectoryNotFoundException)
        {
            return new RetroLinkOperationResult(1, kind);
        }
        catch (UnauthorizedAccessException)
        {
            return new RetroLinkOperationResult(3, kind);
        }
        catch (ArgumentException)
        {
            return new RetroLinkOperationResult(2, kind);
        }
        catch (NotSupportedException)
        {
            return new RetroLinkOperationResult(2, kind);
        }
        catch (IOException)
        {
            return new RetroLinkOperationResult(8, kind);
        }
    }

    private static string ResolveShareDirectory(string rootPath, RetroLinkPath path)
    {
        if (path.ProviderId != LocalShareProviderId)
        {
            throw new ArgumentException("The PathRef does not identify the LOCAL share.");
        }

        var root = Path.GetFullPath(rootPath);
        if ((File.GetAttributes(root) & FileAttributes.ReparsePoint) != 0)
        {
            throw new UnauthorizedAccessException("The selected share root is a reparse point.");
        }

        var current = root;
        foreach (var component in path.Components)
        {
            current = Path.GetFullPath(Path.Combine(current, component));
            if (!IsWithinRoot(current, root))
            {
                throw new UnauthorizedAccessException("The operation path escaped the share root.");
            }

            var attributes = File.GetAttributes(current);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new UnauthorizedAccessException("The operation path crosses a reparse point.");
            }

            if ((attributes & FileAttributes.Directory) == 0)
            {
                throw new DirectoryNotFoundException("An operation path component is not a directory.");
            }
        }

        return current;
    }

    private static (string Path, FileAttributes Attributes) ResolveShareObject(
        string rootPath,
        RetroLinkPath path)
    {
        if (path.Components.Count == 0)
        {
            throw new ArgumentException("The share root cannot be renamed or deleted.");
        }

        var parentPath = new RetroLinkPath(
            path.ProviderId,
            path.Components.Take(path.Components.Count - 1).ToArray());
        var parent = ResolveShareDirectory(rootPath, parentPath);
        var target = Path.GetFullPath(Path.Combine(parent, path.Components[^1]));
        var root = Path.GetFullPath(rootPath);
        if (!IsWithinRoot(target, root))
        {
            throw new UnauthorizedAccessException("The operation target escaped the share root.");
        }

        var attributes = File.GetAttributes(target);
        if ((attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new UnauthorizedAccessException("The operation target is a reparse point.");
        }

        return (target, attributes);
    }

    private async Task SendOperationResultAsync(
        uint requestId,
        RetroLinkOperationResult result)
    {
        var session = _session;
        if (session is null)
        {
            return;
        }

        await session.SendAsync(
            (byte)RetroLinkMessageType.OperationResult,
            requestId,
            RetroLinkDirectoryProtocol.EncodeOperationResult(result));
    }

    private async Task HandleOperationResultAsync(RetroLinkFrame frame)
    {
        var pending = _pendingFileOperation;
        if (pending is null || pending.RequestId != frame.RequestId)
        {
            return;
        }

        ClearPendingActionTimeout();
        _pendingFileOperation = null;
        RetroLinkOperationResult result;
        try
        {
            result = RetroLinkDirectoryProtocol.DecodeOperationResult(frame.Payload.Span);
        }
        catch (InvalidDataException error)
        {
            SetStatus($"REMOTE returned an invalid operation result: {error.Message}");
            return;
        }

        if (result.OperationKind != pending.Kind)
        {
            SetStatus("The REMOTE operation response did not match the pending request.");
            return;
        }

        if (result.ResultCode != 0)
        {
            SetStatus($"{pending.Kind} failed: {FormatOperationResult(result.ResultCode)}.");
            return;
        }

        if (pending.IsRemote && pending.RemoteRefreshPath is not null)
        {
            await RequestRemoteListAsync(pending.RemoteRefreshPath, 0);
        }

        SetStatus(result.CanonicalName is null
            ? $"{pending.Kind} completed successfully."
            : $"{pending.Kind} completed: '{result.CanonicalName}'.");
    }

    private static string FormatOperationResult(ushort resultCode) => resultCode switch
    {
        1 => "entry not found",
        2 => "invalid path or name",
        3 => "permission denied",
        4 => "entry already exists",
        5 => "directory is not empty",
        6 => "volume is out of space",
        7 => "operation is unsupported",
        _ => "I/O error",
    };

    private async Task SendListPageAsync(uint requestId, RetroLinkListPage page)
    {
        var session = _session;
        if (session is null)
        {
            return;
        }

        await session.SendAsync(
            (byte)RetroLinkMessageType.ListPage,
            requestId,
            RetroLinkDirectoryProtocol.EncodeListPage(page));
    }

    private async Task HandleTransferReadyAsync(RetroLinkFrame frame)
    {
        var transfer = _outboundTransfer;
        if (transfer is null || transfer.RequestId != frame.RequestId)
        {
            return;
        }

        RetroLinkTransferReady ready;
        try
        {
            ready = RetroLinkTransferProtocol.DecodeTransferReady(frame.Payload.Span);
        }
        catch (InvalidDataException error)
        {
            FailOutboundTransfer($"REMOTE returned an invalid transfer response: {error.Message}");
            return;
        }

        ClearPendingActionTimeout();
        if (ready.ResultCode != 0)
        {
            FailOutboundTransfer(
                $"REMOTE rejected the destination (error {ready.ResultCode}).");
            return;
        }

        if (ready.StreamLength != transfer.Length ||
            ready.StreamCrc32 != transfer.Crc32 ||
            ready.TransferMode != RetroLinkTransferProtocol.XModemCrc128Mode ||
            ready.MaximumBlockSize != RetroLinkTransferProtocol.XModemCrcBlockSize)
        {
            FailOutboundTransfer("The REMOTE transfer response did not match the requested file.");
            return;
        }

        try
        {
            SetStatus($"Transferring {transfer.Length:N0} bytes to REMOTE using XMODEM-CRC...");
            TransferProgressBar.Value = 20;
            if (transfer.Length > 0)
            {
                if (_serialPort is null)
                {
                    throw new IOException("The serial port closed before transfer started.");
                }

                await new XModemCrcSender(
                    maximumAttempts: 3,
                    responseTimeout: PendingActionTimeout)
                    .SendAsync(
                        transfer.Stream,
                        _serialPort.BaseStream,
                        checked((long)transfer.Length),
                        progress: new Progress<long>(bytesSent =>
                        {
                            TransferProgressBar.Value = 20 +
                                (double)bytesSent / transfer.Length * 70;
                            SetStatus(
                                $"Transferring {bytesSent:N0} of {transfer.Length:N0} bytes to REMOTE using XMODEM-CRC...");
                        }));
            }

            TransferProgressBar.Value = 90;
            SetStatus("File data sent; waiting for REMOTE to verify and install it.");
            StartPendingActionTimeout(
                frame.RequestId,
                () => FailOutboundTransfer(
                    "Timeout: REMOTE did not confirm the completed transfer within 10 seconds."));
        }
        catch (Exception error) when (
            error is IOException or
                TimeoutException or
                InvalidDataException or
                ObjectDisposedException or
                XModemTransferException)
        {
            FailOutboundTransfer(
                error is TimeoutException
                    ? $"Timeout during XMODEM transfer: {error.Message}"
                    : $"The XMODEM transfer failed: {error.Message}");
        }
    }

    private void HandleTransferComplete(RetroLinkFrame frame)
    {
        var transfer = _outboundTransfer;
        if (transfer is null || transfer.RequestId != frame.RequestId)
        {
            return;
        }

        RetroLinkTransferComplete complete;
        try
        {
            complete = RetroLinkTransferProtocol.DecodeTransferComplete(
                frame.Payload.Span);
        }
        catch (InvalidDataException error)
        {
            FailOutboundTransfer($"REMOTE returned an invalid completion response: {error.Message}");
            return;
        }

        if (complete.ResultCode != 0 ||
            complete.StreamLength != transfer.Length ||
            complete.StreamCrc32 != transfer.Crc32)
        {
            FailOutboundTransfer(
                $"REMOTE could not verify the transferred file (result {complete.ResultCode}).");
            return;
        }

        ClearPendingActionTimeout();
        transfer.Stream.Dispose();
        _outboundTransfer = null;
        TransferProgressBar.Value = 100;
        TransferProgressBar.Visibility = Visibility.Collapsed;
        UpdateCopyButtonState();
        SetStatus(
            $"Transfer complete: {transfer.Length:N0} bytes verified on REMOTE.");
    }

    private void FailOutboundTransfer(string message)
    {
        ClearPendingActionTimeout();
        _outboundTransfer?.Stream.Dispose();
        _outboundTransfer = null;
        TransferProgressBar.Visibility = Visibility.Collapsed;
        UpdateCopyButtonState();
        SetStatus(message);
    }

    private void StartPendingActionTimeout(uint requestId, Action onTimeout)
    {
        ClearPendingActionTimeout();
        var timeoutSource = new CancellationTokenSource();
        _pendingActionTimeout = timeoutSource;
        _ = WaitForPendingActionTimeoutAsync(timeoutSource, requestId, onTimeout);
    }

    private async Task WaitForPendingActionTimeoutAsync(
        CancellationTokenSource timeoutSource,
        uint requestId,
        Action onTimeout)
    {
        try
        {
            await Task.Delay(PendingActionTimeout, timeoutSource.Token);
            if (!ReferenceEquals(_pendingActionTimeout, timeoutSource))
            {
                return;
            }

            _pendingActionTimeout = null;
            if (_outboundTransfer?.RequestId == requestId ||
                _remotePendingRequestId == requestId ||
                _pendingFileOperation?.RequestId == requestId)
            {
                onTimeout();
            }
        }
        catch (OperationCanceledException) when (timeoutSource.IsCancellationRequested)
        {
        }
        finally
        {
            timeoutSource.Dispose();
        }
    }

    private void ClearPendingActionTimeout()
    {
        var timeoutSource = _pendingActionTimeout;
        _pendingActionTimeout = null;
        if (timeoutSource is not null)
        {
            timeoutSource.Cancel();
        }
    }

    private async Task RequestRemoteListAsync(RetroLinkPath path, uint cursor)
    {
        if (_pendingFileOperation is not null)
        {
            SetStatus("Wait for the pending file operation before browsing.");
            return;
        }

        var session = _session;
        if (session is null ||
            !_activeEndpointId.Span.SequenceEqual(_endpointId))
        {
            SetStatus("LOCAL must own control to browse REMOTE volumes.");
            return;
        }

        try
        {
            var requestId = session.NextRequestId();
            var request = new RetroLinkListRequest(
                _roleEpoch,
                _endpointId,
                path,
                cursor,
                RetroLinkDirectoryProtocol.MaximumEntryLimit);
            var payload = RetroLinkDirectoryProtocol.EncodeListRequest(
                request,
                allowSlashInComponents: true);
            _remoteRequestedPath = path;
            _remoteRequestedCursor = cursor;
            _remoteRequestRoleEpoch = _roleEpoch;
            _remotePendingRequestId = requestId;
            await session.SendAsync(
                (byte)RetroLinkMessageType.ListRequest,
                requestId,
                payload);
            SetStatus($"Requested REMOTE directory page at cursor {cursor}.");
            StartPendingActionTimeout(
                requestId,
                () =>
                {
                    _remotePendingRequestId = null;
                    SetStatus("Timeout: REMOTE did not return the directory within 10 seconds.");
                });
        }
        catch (Exception error) when (
            error is IOException or
                InvalidDataException or
                InvalidOperationException or
                ObjectDisposedException or
                ArgumentException)
        {
            _remotePendingRequestId = null;
            SetStatus($"Could not request the REMOTE directory: {error.Message}");
        }
    }

    private void HandleRemoteListPage(RetroLinkFrame frame)
    {
        if (_remotePendingRequestId != frame.RequestId ||
            _remoteRequestedPath is null)
        {
            return;
        }

        ClearPendingActionTimeout();
        if (_remoteRequestRoleEpoch != _roleEpoch ||
            !_activeEndpointId.Span.SequenceEqual(_endpointId))
        {
            _remotePendingRequestId = null;
            SetStatus("Ignored a stale REMOTE directory response after control changed.");
            return;
        }

        RetroLinkListPage page;
        try
        {
            page = RetroLinkDirectoryProtocol.DecodeListPage(
                frame.Payload.Span,
                allowSlashInNames: true);
        }
        catch (InvalidDataException error)
        {
            _remotePendingRequestId = null;
            SetStatus($"REMOTE returned an invalid directory page: {error.Message}");
            return;
        }

        if (page.Cursor != _remoteRequestedCursor)
        {
            _remotePendingRequestId = null;
            SetStatus("The REMOTE directory response did not match the requested page.");
            return;
        }

        _remotePendingRequestId = null;
        if (page.ResultCode != 0)
        {
            SetStatus($"REMOTE could not list this directory (error {page.ResultCode}).");
            return;
        }

        var path = _remoteRequestedPath;
        if (page.Cursor == 0)
        {
            RemoteEntries.Clear();
            RemoteEntries.Add(new RetroLinkFileEntry(
                path.Components.Count == 0 ? "Volumes" : "..",
                "Parent",
                string.Empty,
                string.Empty,
                null,
                true,
                false,
                path.ProviderId,
                path.Components.Take(
                    Math.Max(0, path.Components.Count - 1)).ToArray(),
                IsParent: true));
        }
        else
        {
            var moreEntry = RemoteEntries
                .FirstOrDefault(entry => entry.ContinuationCursor is not null);
            if (moreEntry is not null)
            {
                RemoteEntries.Remove(moreEntry);
            }
        }

        foreach (var entry in page.Entries)
        {
            var isDirectory = entry.Kind == RetroLinkDirectoryProtocol.DirectoryKind;
            var modified = DateTimeOffset
                .FromUnixTimeSeconds(entry.ModifiedTime + 946684800L)
                .ToLocalTime()
                .ToString("g");
            var size = isDirectory
                ? "<DIR>"
                : entry.ResourceForkSize == 0
                    ? $"{entry.DataForkSize:N0} bytes"
                    : $"{entry.DataForkSize:N0} + {entry.ResourceForkSize:N0} resource bytes";
            RemoteEntries.Add(new RetroLinkFileEntry(
                entry.Name,
                isDirectory ? "Folder" : "File",
                size,
                modified,
                null,
                isDirectory,
                false,
                path.ProviderId,
                path.Components.Append(entry.Name).ToArray()));
        }

        if (page.NextCursor != RetroLinkDirectoryProtocol.CompleteCursor)
        {
            RemoteEntries.Add(new RetroLinkFileEntry(
                "More...",
                "More",
                string.Empty,
                string.Empty,
                null,
                false,
                false,
                path.ProviderId,
                path.Components,
                ContinuationCursor: page.NextCursor));
        }

        _remoteCurrentPath = path;
        RemoteRootText.Text = FormatRemotePath(path);
        RemoteUpButton.IsEnabled = RemoteEntries.Any(entry => entry.IsParent);
        SetStatus(
            $"Loaded {page.Entries.Count} REMOTE entries from cursor {page.Cursor}.");
    }

    private string FormatRemotePath(RetroLinkPath path)
    {
        var rootName = _session?.PeerInfo?.Roots
            .FirstOrDefault(root => root.ProviderId == path.ProviderId)?.Name ??
            $"REMOTE volume {path.ProviderId}";
        return path.Components.Count == 0
            ? rootName
            : $"{rootName}:{string.Join(':', path.Components)}";
    }

    private void ShowRemoteRoots()
    {
        var peer = _session?.PeerInfo;
        if (peer is null)
        {
            return;
        }

        ClearPendingActionTimeout();
        _remotePendingRequestId = null;
        _remoteCurrentPath = null;
        RemoteEntries.Clear();
        RemoteUpButton.IsEnabled = false;
        foreach (var root in peer.Roots)
        {
            RemoteEntries.Add(new RetroLinkFileEntry(
                root.Name,
                root.IsReadOnly ? "Read-only volume" : "Volume",
                string.Empty,
                string.Empty,
                null,
                true,
                false,
                root.ProviderId,
                []));
        }

        RemoteRootText.Text = peer.Roots.Count == 0
            ? "The peer did not advertise any mounted roots."
            : $"{peer.Roots.Count} remote volume(s) advertised. Double-click a volume to browse.";
        SetStatus("Showing mounted REMOTE volumes.");
    }

    private async Task SendProtocolErrorAsync(
        uint requestId,
        ushort errorCode,
        string reason)
    {
        var session = _session;
        if (session is null)
        {
            return;
        }

        var reasonBytes = Encoding.UTF8.GetBytes(reason);
        if (reasonBytes.Length > 128)
        {
            reasonBytes = reasonBytes[..128];
        }

        var payload = new byte[4 + reasonBytes.Length];
        BinaryPrimitives.WriteUInt16BigEndian(payload, errorCode);
        payload[2] = (byte)RetroLinkMessageType.ListRequest;
        payload[3] = checked((byte)reasonBytes.Length);
        reasonBytes.CopyTo(payload.AsSpan(4));
        await session.SendAsync(
            (byte)RetroLinkMessageType.Error,
            requestId,
            payload);
    }

    private static IReadOnlyList<RetroLinkDirectoryEntry> EnumerateShareEntries(
        string rootPath,
        RetroLinkPath path)
    {
        if (path.ProviderId != LocalShareProviderId)
        {
            throw new ArgumentException("The PathRef does not identify the LOCAL share.", nameof(path));
        }

        var root = Path.GetFullPath(rootPath);
        if ((File.GetAttributes(root) & FileAttributes.ReparsePoint) != 0)
        {
            throw new UnauthorizedAccessException("The selected share root is a reparse point.");
        }

        var directoryPath = root;
        foreach (var component in path.Components)
        {
            directoryPath = Path.GetFullPath(Path.Combine(directoryPath, component));
            if (!IsWithinRoot(directoryPath, root))
            {
                throw new UnauthorizedAccessException("The requested directory escaped the share root.");
            }

            var attributes = File.GetAttributes(directoryPath);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new UnauthorizedAccessException("The requested path crosses a reparse point.");
            }

            if ((attributes & FileAttributes.Directory) == 0)
            {
                throw new DirectoryNotFoundException("A requested path component is not a directory.");
            }
        }

        return Directory.EnumerateFileSystemEntries(directoryPath)
            .Select(entryPath =>
            {
                var fullPath = Path.GetFullPath(entryPath);
                if (!IsWithinRoot(fullPath, root))
                {
                    throw new UnauthorizedAccessException(
                        $"An entry resolved outside the selected root: {entryPath}");
                }

                var attributes = File.GetAttributes(fullPath);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    return null;
                }

                var isDirectory = (attributes & FileAttributes.Directory) != 0;
                var name = Path.GetFileName(fullPath);
                if (StrictUtf8.GetByteCount(name) is 0 or > RetroLinkDirectoryProtocol.MaximumNameBytes)
                {
                    return null;
                }

                var modified = new DateTimeOffset(File.GetLastWriteTimeUtc(fullPath));
                var seconds = Math.Clamp(
                    (modified - new DateTimeOffset(2000, 1, 1, 0, 0, 0, TimeSpan.Zero)).TotalSeconds,
                    0,
                    uint.MaxValue);
                return new RetroLinkDirectoryEntry(
                    isDirectory
                        ? RetroLinkDirectoryProtocol.DirectoryKind
                        : RetroLinkDirectoryProtocol.FileKind,
                    (attributes & FileAttributes.ReadOnly) != 0 ? (ushort)1 : (ushort)0,
                    name,
                    isDirectory ? 0 : checked((ulong)new FileInfo(fullPath).Length),
                    0,
                    checked((uint)seconds));
            })
            .Where(entry => entry is not null)
            .Select(entry => entry!)
            .OrderByDescending(entry => entry.Kind == RetroLinkDirectoryProtocol.DirectoryKind)
            .ThenBy(entry => entry.Name, StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static RetroLinkListPage CreateListPage(
        IReadOnlyList<RetroLinkDirectoryEntry> entries,
        uint cursor,
        ushort entryLimit)
    {
        if (cursor > entries.Count)
        {
            return new RetroLinkListPage(
                1,
                cursor,
                RetroLinkDirectoryProtocol.CompleteCursor,
                []);
        }

        var remaining = entries.Skip(checked((int)cursor)).Take(entryLimit).ToArray();
        var count = remaining.Length;
        while (count > 0)
        {
            var pageEntries = remaining.Take(count).ToArray();
            var nextCursor = cursor + (uint)count < entries.Count
                ? cursor + (uint)count
                : RetroLinkDirectoryProtocol.CompleteCursor;
            var page = new RetroLinkListPage(0, cursor, nextCursor, pageEntries);
            try
            {
                _ = RetroLinkDirectoryProtocol.EncodeListPage(page);
                return page;
            }
            catch (ArgumentOutOfRangeException) when (count > 1)
            {
                count--;
            }
        }

        return new RetroLinkListPage(
            0,
            cursor,
            cursor < entries.Count ? cursor + 1 : RetroLinkDirectoryProtocol.CompleteCursor,
            remaining.Take(1).ToArray());
    }

    private async Task HandleRoleRequestAsync(ReadOnlyMemory<byte> payloadMemory)
    {
        var payload = payloadMemory.ToArray();
        if (_session?.PeerInfo is not { } peer ||
            payload.Length != 21 ||
            !payload.AsSpan(0, 16).SequenceEqual(peer.EndpointId.Span))
        {
            SetStatus("Ignored a malformed or unrecognized role request.");
            return;
        }

        var expectedEpoch = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(
            payload.AsSpan(16, 4));
        var reason = payload[20];
        if (expectedEpoch != _roleEpoch || reason > 2)
        {
            SetStatus("Ignored a stale or invalid role request.");
            return;
        }

        if (_outboundTransfer is not null || _pendingFileOperation is not null)
        {
            await SendRoleStateAsync();
            SetStatus("Control cannot change while a file operation is active.");
            return;
        }

        _roleEpoch = unchecked(_roleEpoch + 1);
        _activeEndpointId = peer.EndpointId;
        await SendRoleStateAsync();
        UpdateRoleText();
        SetStatus("REMOTE requested control and is now ACTIVE.");
    }

    private async Task SendRoleStateAsync()
    {
        var session = _session;
        if (session is null)
        {
            return;
        }

        var payload = new byte[21];
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(
            payload.AsSpan(0, 4),
            _roleEpoch);
        _activeEndpointId.Span.CopyTo(payload.AsSpan(4, 16));
        payload[20] = 0;
        await session.SendAsync(
            (byte)RetroLinkMessageType.RoleState,
            0,
            payload);
    }

    private void UpdatePeerInfo(RetroLinkPeerInfo peer)
    {
        var endpointHex = Convert.ToHexString(peer.EndpointId.Span[..4]);
        RemoteDeviceText.Text =
            $"REMOTE peer v{peer.ApplicationMajor}.{peer.ApplicationMinor}.{peer.ApplicationPatch} " +
            $"(System {peer.SystemMajor}.{peer.SystemMinor}, ID {endpointHex}...)";
        RemoteRootText.Text = peer.Roots.Count == 0
            ? "The peer did not advertise any mounted roots."
            : $"{peer.Roots.Count} remote volume(s) advertised. Double-click a volume to browse.";
        RemoteEntries.Clear();
        foreach (var root in peer.Roots)
        {
            RemoteEntries.Add(new RetroLinkFileEntry(
                root.Name,
                root.IsReadOnly ? "Read-only volume" : "Volume",
                string.Empty,
                string.Empty,
                null,
                true,
                false,
                root.ProviderId,
                []));
        }

        TakeControlButton.IsEnabled = true;
        UpdateRoleText();
    }

    private void UpdateRoleText()
    {
        var peerEndpointId = _session?.PeerInfo?.EndpointId;
        RoleText.Text = peerEndpointId is null
            ? "Role: Disconnected"
            : _activeEndpointId.Span.SequenceEqual(_endpointId)
                ? "Role: LOCAL ACTIVE"
                : "Role: REMOTE ACTIVE";
    }

    private static string FormatPeerError(byte[] payload)
    {
        if (payload.Length < 4 || payload[3] > 128 || payload.Length != 4 + payload[3])
        {
            return "REMOTE peer returned a malformed protocol error.";
        }

        var text = Encoding.UTF8.GetString(payload.AsSpan(4));
        return $"REMOTE peer error {System.Buffers.Binary.BinaryPrimitives.ReadUInt16BigEndian(payload)} " +
               $"(message 0x{payload[2]:X2}): {text}";
    }

    private void DisconnectSession(string? status = null)
    {
        ClearPendingActionTimeout();
        _outboundTransfer?.Stream.Dispose();
        _outboundTransfer = null;
        _receiveCancellation?.Cancel();
        _receiveCancellation?.Dispose();
        _receiveCancellation = null;
        _session?.Disconnect();
        _session = null;
        _serialPort?.Dispose();
        _serialPort = null;
        _roleEpoch = 0;
        _activeEndpointId = ReadOnlyMemory<byte>.Empty;
        RemoteEntries.Clear();
        _remoteCurrentPath = null;
        _remoteRequestedPath = null;
        _remotePendingRequestId = null;
        _pendingFileOperation = null;
        RemoteUpButton.IsEnabled = false;
        TransferProgressBar.Visibility = Visibility.Collapsed;
        RemoteDeviceText.Text = "Not connected to a REMOTE RetroLink peer";
        RemoteRootText.Text = "Remote volumes will appear after the HELLO handshake.";
        TakeControlButton.IsEnabled = false;
        UpdateRoleText();
        UpdateConnectionControls();
        if (status is not null)
        {
            SetStatus(status);
        }
    }

    private void UpdateConnectionControls()
    {
        var connected = _session is not null;
        ConnectButton.IsEnabled = !_isConnecting && !connected;
        DisconnectButton.IsEnabled = connected;
        ComPortComboBox.IsEnabled = !_isConnecting && !connected;
        BaudRateComboBox.IsEnabled = !_isConnecting && !connected;
        UpdateCopyButtonState();
    }

    private void UpdateCopyButtonState()
    {
        CopyToMacButton.IsEnabled =
            _session is not null &&
            _activeEndpointId.Span.SequenceEqual(_endpointId) &&
            _pendingFileOperation is null &&
            _outboundTransfer is null &&
            _remoteCurrentPath is not null &&
            _localRootPath is not null &&
            LocalFileListView.SelectedItem is RetroLinkFileEntry
            {
                IsDirectory: false,
                IsReparsePoint: false,
                FullPath: not null,
            };
    }

    private void SetStatus(string message) => StatusText.Text = message;
}
