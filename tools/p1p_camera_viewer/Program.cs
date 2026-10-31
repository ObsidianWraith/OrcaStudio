// Camera viewer revision 003. Independent LAN-only P1P/P1S JPEG client.
using System.Buffers.Binary;
using System.Net.Security;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

record CameraConfig(string Host, string AccessCode);
static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        CameraConfig? config = null;
        // Read the anonymous pipe only when launched by the slicer; desktop launch must not block.
        if (args.Contains("--stdin")) {
            try {
                var line = Console.In.ReadLine();
                if (line != null) config = JsonSerializer.Deserialize<CameraConfig>(line);
            } catch { }
        }
        if (!Valid(config)) {
            using var dialog = new ConnectDialog();
            if (dialog.ShowDialog() != DialogResult.OK) return;
            config = dialog.Config;
        }
        var key = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(
            Encoding.UTF8.GetBytes(config!.Host.Trim().ToLowerInvariant())));
        using var instance = new Mutex(true, @"Local\OrcaCameraViewer-" + key, out bool first);
        if (!first) {
            MessageBox.Show("A viewer for this printer is already open. Switch to it using Alt+Tab.", "Orca Camera Viewer");
            return;
        }
        Application.Run(new Viewer(config!));
    }
    public static bool Valid(CameraConfig? c) => c != null && !string.IsNullOrWhiteSpace(c.Host)
        && Uri.CheckHostName(c.Host.Trim()) != UriHostNameType.Unknown
        && c.AccessCode != null && c.AccessCode.Length is >= 1 and <= 32
        && c.AccessCode.All(ch => ch is >= ' ' and <= '~');
}
sealed class ViewerSettings
{
    public string Host { get; set; } = "";
    public int X { get; set; }
    public int Y { get; set; }
    public int Width { get; set; } = 960;
    public int Height { get; set; } = 600;
    public bool Positioned { get; set; }
    public bool Topmost { get; set; }
    static string PathName => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OrcaCameraViewer", "settings.json");
    public static ViewerSettings Load() {
        try { return JsonSerializer.Deserialize<ViewerSettings>(File.ReadAllText(PathName)) ?? new(); }
        catch { return new(); }
    }
    public void Save() {
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(PathName)!);
            File.WriteAllText(PathName + ".tmp", JsonSerializer.Serialize(this));
            File.Move(PathName + ".tmp", PathName, true);
        } catch { /* Preferences must never prevent camera use. No access code is stored. */ }
    }
}
sealed class ConnectDialog : Form
{
    readonly TextBox host = new() { Width = 270, Text = ViewerSettings.Load().Host };
    readonly TextBox code = new() { Width = 270, UseSystemPasswordChar = true, MaxLength = 32 };
    public CameraConfig Config => new(host.Text.Trim(), code.Text.Trim());
    public ConnectDialog() {
        Text = "Connect to P1P / P1S camera"; AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink; StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
        var panel = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, Padding = new Padding(16) };
        panel.Controls.Add(new Label { Text = "Printer IP address or hostname", AutoSize = true }); panel.Controls.Add(host);
        panel.Controls.Add(new Label { Text = "LAN access code (not saved)", AutoSize = true }); panel.Controls.Add(code);
        var connect = new Button { Text = "Connect", AutoSize = true };
        connect.Click += (_, _) => {
            if (!Program.Valid(Config)) { MessageBox.Show("Enter a valid host and LAN access code."); return; }
            DialogResult = DialogResult.OK;
        };
        panel.Controls.Add(connect); Controls.Add(panel); AcceptButton = connect;
    }
}
sealed class Viewer : Form
{
    readonly PictureBox picture = new() { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.Black };
    readonly Label status = new() { Dock = DockStyle.Bottom, Height = 30, ForeColor = Color.White, BackColor = Color.Black, TextAlign = ContentAlignment.MiddleCenter };
    readonly CancellationTokenSource lifetime = new();
    readonly CameraConfig config;
    readonly ViewerSettings settings = ViewerSettings.Load();
    readonly System.Windows.Forms.Timer clock = new() { Interval = 1000 };
    CancellationTokenSource? connectionAttempt;
    DateTime? lastFrame;
    string connectionStatus = "Connecting";
    bool fullscreen;
    Rectangle normalBounds;
    public Viewer(CameraConfig config)
    {
        this.config = config;
        Text = "Orca Camera Viewer — P1P";
        Size = new Size(960, 600); StartPosition = FormStartPosition.CenterScreen;
        if (settings.Positioned) {
            var saved = new Rectangle(settings.X, settings.Y, Math.Max(400, settings.Width), Math.Max(300, settings.Height));
            if (Screen.AllScreens.Any(s => s.WorkingArea.IntersectsWith(saved))) {
                StartPosition = FormStartPosition.Manual; Bounds = saved;
            }
        }
        TopMost = settings.Topmost;
        Controls.Add(picture); Controls.Add(status); KeyPreview = true;
        picture.DoubleClick += (_, _) => SetFullscreen(!fullscreen);
        KeyDown += (_, e) => {
            if (e.KeyCode == Keys.Escape) SetFullscreen(false);
            if (e.KeyCode == Keys.F11) SetFullscreen(!fullscreen);
        };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Toggle full screen (F11)", null, (_, _) => SetFullscreen(!fullscreen));
        var topmost = new ToolStripMenuItem("Always on top") { CheckOnClick = true, Checked = TopMost };
        topmost.CheckedChanged += (_, _) => TopMost = topmost.Checked;
        menu.Items.Add(topmost);
        menu.Items.Add("Reconnect", null, (_, _) => connectionAttempt?.Cancel());
        menu.Items.Add("Save snapshot…", null, (_, _) => SaveSnapshot());
        var monitors = new ToolStripMenuItem("Move to monitor");
        foreach (var screen in Screen.AllScreens) {
            var target = screen;
            monitors.DropDownItems.Add(screen.DeviceName, null, (_, _) => {
                SetFullscreen(false); Bounds = new Rectangle(target.WorkingArea.Location, new Size(960, 600)); SetFullscreen(true);
            });
        }
        menu.Items.Add(monitors);
        menu.Items.Add("Close viewer", null, (_, _) => Close());
        picture.ContextMenuStrip = menu;
        clock.Tick += (_, _) => {
            var age = lastFrame.HasValue ? (DateTime.UtcNow - lastFrame.Value).TotalSeconds : -1;
            status.Text = connectionStatus + (age >= 0 ? $" · Last frame {age:0}s ago" : " · No frames received")
                + " · Esc: windowed · F11: fullscreen · Right-click: options";
            status.ForeColor = age > 10 ? Color.Orange : Color.White;
        };
        clock.Start();
        Shown += async (_, _) => { SetFullscreen(true); await StreamCamera(); };
        FormClosed += (_, _) => {
            clock.Stop(); clock.Dispose(); lifetime.Cancel(); connectionAttempt?.Cancel();
            var bounds = fullscreen ? normalBounds : (WindowState == FormWindowState.Normal ? Bounds : RestoreBounds);
            settings.Host = config.Host; settings.X = bounds.X; settings.Y = bounds.Y;
            settings.Width = bounds.Width; settings.Height = bounds.Height;
            settings.Positioned = true; settings.Topmost = TopMost; settings.Save();
            picture.Image?.Dispose(); picture.Image = null;
        };
    }
    void SaveSnapshot()
    {
        if (picture.Image == null) { MessageBox.Show("No camera frame is available yet."); return; }
        using var dialog = new SaveFileDialog { Filter = "PNG image|*.png", FileName = $"P1P-{DateTime.Now:yyyyMMdd-HHmmss}.png" };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        try { picture.Image.Save(dialog.FileName, System.Drawing.Imaging.ImageFormat.Png); }
        catch (Exception) { MessageBox.Show("Unable to save the snapshot. Choose a writable folder."); }
    }
    void SetFullscreen(bool value)
    {
        if (fullscreen == value) return;
        fullscreen = value;
        if (value) {
            normalBounds = WindowState == FormWindowState.Normal ? Bounds : RestoreBounds;
            var screen = Screen.FromControl(this);
            WindowState = FormWindowState.Normal;
            FormBorderStyle = FormBorderStyle.None; Bounds = screen.Bounds;
        } else {
            FormBorderStyle = FormBorderStyle.Sizable; Bounds = normalBounds;
        }
    }
    static async Task ReadExact(Stream stream, Memory<byte> bytes, CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(20));
        await stream.ReadExactlyAsync(bytes, timeout.Token);
    }
    async Task StreamCamera()
    {
        var token = lifetime.Token;
        while (!token.IsCancellationRequested) {
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(token);
            connectionAttempt = attempt;
            try {
                connectionStatus = "Connecting to P1P camera…";
                using var client = new TcpClient();
                using var connection = CancellationTokenSource.CreateLinkedTokenSource(attempt.Token);
                connection.CancelAfter(TimeSpan.FromSeconds(15));
                await client.ConnectAsync(config.Host, 6000, connection.Token);
                // Bambu LAN cameras use self-signed certificates; no public CA identity is available.
                using var tls = new SslStream(client.GetStream(), false, (_, _, _, _) => true);
                await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions { TargetHost = config.Host }, connection.Token);
                var auth = new byte[80];
                BinaryPrimitives.WriteUInt32LittleEndian(auth.AsSpan(0, 4), 0x40);
                BinaryPrimitives.WriteUInt32LittleEndian(auth.AsSpan(4, 4), 0x3000);
                Encoding.ASCII.GetBytes("bblp").CopyTo(auth, 16);
                Encoding.ASCII.GetBytes(config.AccessCode).CopyTo(auth, 48);
                await tls.WriteAsync(auth, attempt.Token);
                Array.Clear(auth);
                while (!token.IsCancellationRequested) {
                    var header = new byte[16];
                    await ReadExact(tls, header, attempt.Token);
                    uint length = BinaryPrimitives.ReadUInt32LittleEndian(header);
                    if (length < 4 || length > 16 * 1024 * 1024) throw new InvalidDataException();
                    var jpeg = new byte[(int)length];
                    await ReadExact(tls, jpeg, attempt.Token);
                    if (jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[^2] != 0xff || jpeg[^1] != 0xd9)
                        throw new InvalidDataException();
                    using var memory = new MemoryStream(jpeg);
                    using var decoded = Image.FromStream(memory);
                    var frame = new Bitmap(decoded);
                    var old = picture.Image; picture.Image = frame; old?.Dispose();
                    lastFrame = DateTime.UtcNow; connectionStatus = "Receiving camera frames";
                }
            } catch (Exception) when (!token.IsCancellationRequested) {
                picture.Image?.Dispose(); picture.Image = null;
                connectionStatus = "Camera unavailable. Check LAN connection/access code. Retrying…";
            } catch (Exception) when (token.IsCancellationRequested) { break; }
            connectionAttempt = null;
            try { await Task.Delay(3000, token); } catch (OperationCanceledException) { break; }
        }
    }
}
