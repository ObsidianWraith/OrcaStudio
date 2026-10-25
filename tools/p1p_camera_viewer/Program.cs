// Camera viewer revision 002. Independent LAN-only P1P/P1S JPEG client.
using System.Buffers.Binary;
using System.Net.Security;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

record CameraConfig(string Host, string AccessCode);
static class Program
{
    [STAThread]
    static void Main()
    {
        ApplicationConfiguration.Initialize();
        CameraConfig? config = null;
        try {
            var line = Console.In.ReadLine();
            if (line != null) config = JsonSerializer.Deserialize<CameraConfig>(line);
        } catch { }
        if (config == null || string.IsNullOrWhiteSpace(config.Host) ||
            config.AccessCode.Length is < 1 or > 32 || config.AccessCode.Any(c => c > 127)) {
            MessageBox.Show("Open this viewer using Pop-out camera in OrcaStudio.", "Orca Camera Viewer");
            return;
        }
        Application.Run(new Viewer(config));
    }
}
sealed class Viewer : Form
{
    readonly PictureBox picture = new() { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.Black };
    readonly Label status = new() { Dock = DockStyle.Bottom, Height = 30, ForeColor = Color.White, BackColor = Color.Black, TextAlign = ContentAlignment.MiddleCenter };
    readonly CancellationTokenSource lifetime = new();
    readonly CameraConfig config;
    bool fullscreen;
    Rectangle normalBounds;
    public Viewer(CameraConfig config)
    {
        this.config = config;
        Text = "Orca Camera Viewer — P1P";
        Size = new Size(960, 600); StartPosition = FormStartPosition.CenterScreen;
        Controls.Add(picture); Controls.Add(status); KeyPreview = true;
        picture.DoubleClick += (_, _) => SetFullscreen(!fullscreen);
        KeyDown += (_, e) => {
            if (e.KeyCode == Keys.Escape) SetFullscreen(false);
            if (e.KeyCode == Keys.F11) SetFullscreen(!fullscreen);
        };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Toggle full screen (F11)", null, (_, _) => SetFullscreen(!fullscreen));
        menu.Items.Add("Close viewer", null, (_, _) => Close());
        picture.ContextMenuStrip = menu;
        Shown += async (_, _) => { SetFullscreen(true); await StreamCamera(); };
        FormClosed += (_, _) => { lifetime.Cancel(); picture.Image?.Dispose(); };
    }
    void SetFullscreen(bool value)
    {
        if (fullscreen == value) return;
        fullscreen = value;
        if (value) {
            normalBounds = Bounds;
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
            try {
                status.Text = "Connecting to P1P camera…";
                using var client = new TcpClient();
                using var connection = CancellationTokenSource.CreateLinkedTokenSource(token);
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
                await tls.WriteAsync(auth, token);
                Array.Clear(auth);
                while (!token.IsCancellationRequested) {
                    var header = new byte[16];
                    await ReadExact(tls, header, token);
                    uint length = BinaryPrimitives.ReadUInt32LittleEndian(header);
                    if (length < 4 || length > 16 * 1024 * 1024) throw new InvalidDataException();
                    var jpeg = new byte[(int)length];
                    await ReadExact(tls, jpeg, token);
                    if (jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[^2] != 0xff || jpeg[^1] != 0xd9)
                        throw new InvalidDataException();
                    using var memory = new MemoryStream(jpeg);
                    using var decoded = Image.FromStream(memory);
                    var frame = new Bitmap(decoded);
                    var old = picture.Image; picture.Image = frame; old?.Dispose();
                    status.Text = "Live · Esc: windowed · F11/double-click: fullscreen · Right-click: close";
                }
            } catch (Exception) when (!token.IsCancellationRequested) {
                picture.Image?.Dispose(); picture.Image = null;
                status.Text = "Camera unavailable. Check LAN connection/access code. Retrying…";
            } catch (OperationCanceledException) { break; }
            try { await Task.Delay(3000, token); } catch (OperationCanceledException) { break; }
        }
    }
}
