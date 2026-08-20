// VR180 Spectator Console - one window that owns the whole pipeline.
//
// Replaces the old shortcut that scattered several consoles across the desktop:
//   * starts MediaMTX, the web server, VR180Mirror and OBS itself
//   * merges every log into one view (plus OBS's log file and the viewer's
//     errors, which arrive from inside the headset over /clientlog)
//   * guarantees that closing this window kills everything it started
//
// The guarantee comes from a Windows Job Object with KILL_ON_JOB_CLOSE: every
// child is assigned to the job, so when this process's handle closes - normal
// exit, crash, or "End task" in Task Manager - the kernel terminates the whole
// job. A job kill is a TerminateProcess, so OBS never gets to ask whether we
// really want to stop streaming.
//
// Built by tools\build-console.ps1 with the in-box .NET Framework compiler.

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

public class ConsoleForm : Form
{
    // ---------------------------------------------------------------- job object
    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS { public ulong r, w, o, rt, wt, ot; }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObject(IntPtr a, string name);
    [DllImport("kernel32.dll")]
    static extern bool SetInformationJobObject(IntPtr job, int cls, IntPtr info, uint len);
    [DllImport("kernel32.dll")]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);

    const int ExtendedLimitInformation = 9;
    const uint KillOnJobClose = 0x2000;

    IntPtr job = IntPtr.Zero;

    void CreateKillOnCloseJob()
    {
        job = CreateJobObject(IntPtr.Zero, null);
        var ext = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        ext.BasicLimitInformation.LimitFlags = KillOnJobClose;
        int len = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
        IntPtr p = Marshal.AllocHGlobal(len);
        Marshal.StructureToPtr(ext, p, false);
        if (!SetInformationJobObject(job, ExtendedLimitInformation, p, (uint)len))
            Log("sys", "WARN: could not arm the job object; children may outlive this window", Warn);
        Marshal.FreeHGlobal(p);
    }

    // ---------------------------------------------------------------- appearance
    static readonly Color Bg = ColorTranslator.FromHtml("#0e141b");
    static readonly Color Ink = ColorTranslator.FromHtml("#c9d3dd");
    static readonly Color Dim = ColorTranslator.FromHtml("#7c8894");
    static readonly Color Accent = ColorTranslator.FromHtml("#3ec3dc");
    static readonly Color Good = ColorTranslator.FromHtml("#58c274");
    static readonly Color Warn = ColorTranslator.FromHtml("#dfa04b");
    static readonly Color Bad = ColorTranslator.FromHtml("#ff6b6b");

    RichTextBox logBox;
    Label statusLabel;
    Button stopBtn, copyBtn, saveBtn, startBtn, measureBtn, refreshDevBtn;
    CheckBox autoScroll;
    ComboBox resCombo, bitrateCombo, deviceCombo;
    Label measureLabel;
    NumericUpDown renderScaleNud;
    readonly List<Process> children = new List<Process>();
    readonly string root;
    System.Windows.Forms.Timer statusTimer;
    long obsLogPos = 0, clientLogPos = 0;
    string obsLogPath = null;
    bool pipelineStarted = false;

    // resolution choice -> (OBS-side codec, canvas) matching Start-Spectator.ps1's own mapping
    static readonly string[] ResChoices = { "6144x3264 (native, HEVC)", "4096x2048 (AV1)", "3840x1920 (H264)" };
    static readonly string[] ResCodecs  = { "hevc", "av1", "h264" };
    static readonly string[] BitrateChoices = { "50", "80", "100", "120", "150 (default)", "180", "200" };

    [STAThread]
    public static void Main(string[] args) { Application.EnableVisualStyles(); Application.Run(new ConsoleForm(args)); }

    public ConsoleForm(string[] args)
    {
        root = Path.GetDirectoryName(Path.GetDirectoryName(Application.ExecutablePath)); // ...\tools\.. => repo
        Text = "VR180 Spectator";
        // WinForms positions everything below in raw pixels tuned for 96 DPI.
        // Without Dpi autoscale, a scaled display (125%/150%, common on Windows
        // laptops) renders larger text in the same fixed-width boxes and clips
        // it - this makes the whole layout scale together instead.
        AutoScaleMode = AutoScaleMode.Dpi;
        AutoScaleDimensions = new SizeF(96F, 96F);
        ClientSize = new Size(1180, 720);
        MinimumSize = new Size(900, 500);
        BackColor = Bg;
        StartPosition = FormStartPosition.CenterScreen;

        // Everything below uses AutoSize controls inside FlowLayoutPanels instead
        // of fixed pixel Left/Top/Width. A fixed-pixel layout (this file's first
        // attempt) clipped text badly under Windows display scaling: WinForms'
        // own Dpi/Font auto-scale machinery does not reliably rescale absolute-
        // positioned children on an older-style per-monitor-aware manifest, so a
        // control sized for 96 DPI stayed that size while its text rendered at
        // the monitor's real (scaled) font size and overflowed the box. AutoSize
        // has no such gap: it measures the control's actual on-screen content at
        // whatever DPI/font is really in effect, so there is nothing to get out
        // of sync. FlowLayoutPanel with WrapContents also means the whole config
        // bar reflows instead of clipping if the window is narrower.
        var topFlow = new FlowLayoutPanel {
            Dock = DockStyle.Top, FlowDirection = FlowDirection.LeftToRight, WrapContents = true,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Bg,
            Padding = new Padding(8, 8, 8, 4),
        };
        startBtn = MakeButton("Start pipeline");
        startBtn.Click += (s, e) => { if (!pipelineStarted) StartPipeline(args); };
        stopBtn = MakeButton("Stop everything");
        stopBtn.Click += (s, e) => Close();
        copyBtn = MakeButton("Copy all logs");
        copyBtn.Click += (s, e) => {
            try { Clipboard.SetText(logBox.Text); Log("sys", "logs copied to the clipboard", Accent); } catch { }
        };
        saveBtn = MakeButton("Save logs...");
        saveBtn.Click += (s, e) => SaveLogs();
        autoScroll = new CheckBox {
            Text = "Follow", Checked = true, ForeColor = Dim, AutoSize = true, BackColor = Bg,
            Margin = new Padding(12, 10, 0, 0),
        };
        topFlow.Controls.AddRange(new Control[] { startBtn, stopBtn, copyBtn, saveBtn, autoScroll });

        // ---- config bar: resolution/bitrate/target headset, before starting -------
        var cfgFlow = new FlowLayoutPanel {
            Dock = DockStyle.Top, FlowDirection = FlowDirection.LeftToRight, WrapContents = true,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Bg,
            Padding = new Padding(8, 4, 8, 8),
        };

        resCombo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        resCombo.Items.AddRange(ResChoices); resCombo.SelectedIndex = 0;
        SizeCombo(resCombo);

        bitrateCombo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        bitrateCombo.Items.AddRange(BitrateChoices); bitrateCombo.SelectedIndex = 4;   // 150 (default)
        SizeCombo(bitrateCombo);

        deviceCombo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        deviceCombo.Items.Add("(auto - single device)");
        deviceCombo.SelectedIndex = 0;
        SizeCombo(deviceCombo);
        refreshDevBtn = MakeButton("Refresh devices");
        refreshDevBtn.Click += (s, e) => PopulateDevices();

        measureBtn = MakeButton("Measure USB speed");
        measureBtn.Click += (s, e) => MeasureUsbSpeed();
        measureLabel = new Label {
            Text = "not measured yet", ForeColor = Dim, AutoSize = true, MaximumSize = new Size(700, 0),
            BackColor = Bg, Margin = new Padding(4, 10, 0, 0),
        };

        // Render scale: mirrors the slider on the headset's own page (both read/write
        // the same /settings bridge), so the multiplier that lands on the goal
        // resolution (native canvas above, e.g. 6144x3264) can be found by trial and
        // error from here without putting the headset on each time.
        renderScaleNud = new NumericUpDown {
            Width = 70, Minimum = 1.00m, Maximum = 2.00m, Increment = 0.01m, DecimalPlaces = 2, Value = 1.50m,
        };
        renderScaleNud.ValueChanged += (s, e) => PushRenderScale();
        var rsHint = new Label {
            Text = "applies next time the headset presses Enter VR", ForeColor = Dim, AutoSize = true,
            BackColor = Bg, Margin = new Padding(6, 4, 0, 0),
        };

        cfgFlow.Controls.AddRange(new Control[] {
            MakeField("Resolution", resCombo),
            MakeField("Bitrate (Mbps)", bitrateCombo),
            MakeField("Target headset (USB)", MakeRow(deviceCombo, refreshDevBtn)),
            MakeRow(measureBtn, measureLabel),
            MakeField("Quest render scale", MakeRow(renderScaleNud, rsHint)),
        });

        statusLabel = new Label {
            Dock = DockStyle.Bottom, Height = 26, ForeColor = Dim, BackColor = Bg,
            Font = new Font("Consolas", 9.5f), TextAlign = ContentAlignment.MiddleLeft,
            Text = "not started - pick settings above, then Start pipeline"
        };

        logBox = new RichTextBox {
            Dock = DockStyle.Fill, BackColor = ColorTranslator.FromHtml("#0a0f14"), ForeColor = Ink,
            Font = new Font("Consolas", 9.5f), ReadOnly = true, BorderStyle = BorderStyle.None,
            WordWrap = false, ScrollBars = RichTextBoxScrollBars.Both, HideSelection = false
        };

        Controls.Add(logBox);
        Controls.Add(statusLabel);
        Controls.Add(cfgFlow);
        Controls.Add(topFlow);

        Shown += (s, e) => PopulateDevices();
        FormClosing += (s, e) => Shutdown();
    }

    static Button MakeButton(string text)
    {
        return new Button {
            Text = text, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Padding = new Padding(12, 4, 12, 4), Margin = new Padding(0, 0, 8, 0), MinimumSize = new Size(0, 28),
            FlatStyle = FlatStyle.Flat, ForeColor = Ink, BackColor = ColorTranslator.FromHtml("#1b2530"),
        };
    }

    // A label above a control, both AutoSize, stacked vertically - the "field"
    // unit the config bar is built from.
    static FlowLayoutPanel MakeField(string labelText, Control control)
    {
        var lbl = new Label { Text = labelText, ForeColor = Dim, AutoSize = true, BackColor = Bg, Margin = new Padding(0, 0, 0, 2) };
        var col = new FlowLayoutPanel {
            FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Bg, Margin = new Padding(0, 0, 28, 12),
        };
        col.Controls.Add(lbl);
        col.Controls.Add(control);
        return col;
    }

    // Controls placed side by side (e.g. a combo + its refresh button).
    static FlowLayoutPanel MakeRow(params Control[] controls)
    {
        var row = new FlowLayoutPanel {
            FlowDirection = FlowDirection.LeftToRight, WrapContents = false, AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Bg, Margin = new Padding(0),
        };
        foreach (var c in controls) { c.Margin = new Padding(0, 0, 8, 0); row.Controls.Add(c); }
        return row;
    }

    // ComboBox does not AutoSize to its content in WinForms, so measure the
    // widest item against the control's own (already correctly DPI-scaled)
    // Font at runtime, rather than guessing a fixed pixel width that only
    // happens to fit at 100% scaling.
    static void SizeCombo(ComboBox combo, int extraPx = 40)
    {
        int w = 60;
        using (var g = combo.CreateGraphics()) {
            foreach (var item in combo.Items) {
                int iw = (int)g.MeasureString(item.ToString(), combo.Font).Width;
                if (iw > w) w = iw;
            }
        }
        combo.Width = w + extraPx;
    }

    // ---------------------------------------------------------------- logging
    void Log(string src, string line, Color color)
    {
        if (line == null) return;
        if (logBox.InvokeRequired) { logBox.BeginInvoke((Action)(() => Log(src, line, color))); return; }
        if (logBox.Lines.Length > 6000) { logBox.Clear(); }   // keep the box responsive

        logBox.SelectionStart = logBox.TextLength;
        logBox.SelectionColor = Dim;
        logBox.AppendText(DateTime.Now.ToString("HH:mm:ss") + " " + src.PadRight(7));
        logBox.SelectionColor = color;
        logBox.AppendText(line + Environment.NewLine);
        if (autoScroll.Checked) { logBox.SelectionStart = logBox.TextLength; logBox.ScrollToCaret(); }
    }

    static Color ColorFor(string line)
    {
        if (Regex.IsMatch(line, @"\b(ERR|ERROR|error|failed|Failed|crash|FATAL)\b")) return Bad;
        if (Regex.IsMatch(line, @"\b(WAR|WARN|warning)\b")) return Warn;
        return Ink;
    }

    void SaveLogs()
    {
        try {
            string file = Path.Combine(root, "vr180-logs-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".txt");
            File.WriteAllText(file, logBox.Text);
            Log("sys", "saved " + file, Accent);
            Process.Start("explorer.exe", "/select,\"" + file + "\"");
        } catch (Exception ex) { Log("sys", "save failed: " + ex.Message, Bad); }
    }

    // ---------------------------------------------------------------- startup
    string SelectedCodec() { return ResCodecs[Math.Max(0, resCombo.SelectedIndex)]; }
    int SelectedBitrateKbps()
    {
        string t = BitrateChoices[Math.Max(0, bitrateCombo.SelectedIndex)];
        int mbps = int.Parse(Regex.Match(t, @"\d+").Value);
        return mbps * 1000;
    }
    string SelectedSerial()
    {
        string t = deviceCombo.SelectedItem as string;
        if (string.IsNullOrEmpty(t) || t.StartsWith("(")) return null;
        return t.Split(' ')[0];
    }

    void StartPipeline(string[] args)
    {
        pipelineStarted = true;
        startBtn.Enabled = false;
        resCombo.Enabled = false; bitrateCombo.Enabled = false; deviceCombo.Enabled = false; refreshDevBtn.Enabled = false;

        CreateKillOnCloseJob();
        Log("sys", "VR180 Spectator - closing this window stops every process it starts", Accent);

        KillLeftovers();

        string codec = SelectedCodec();
        int bitrateKbps = SelectedBitrateKbps();
        string serial = SelectedSerial();
        string extra = string.Join(" ", args)
            + " -Codec " + codec + " -Bitrate " + bitrateKbps
            + (serial != null ? " -Serial " + serial : "");

        Log("sys", "provisioning (OBS profile, USB tunnel): " + codec + " " + bitrateKbps + "kbps"
            + (serial != null ? ", device " + serial : ""), Accent);
        RunToCompletion("powershell", "-ExecutionPolicy Bypass -File \"" + Path.Combine(root, "Start-Spectator.ps1")
            + "\" -ProvisionOnly " + extra, root, "setup");

        Spawn("mediamtx", Path.Combine(root, @"tools\mediamtx\mediamtx.exe"),
              "\"" + Path.Combine(root, @"tools\mediamtx\mediamtx.yml") + "\"",
              Path.Combine(root, @"tools\mediamtx"));

        string webArgs = "\"" + Path.Combine(root, @"web\server.js") + "\"" + (serial != null ? " --serial " + serial : "");
        Spawn("web", "node", webArgs, Path.Combine(root, "web"));
        // pick up whatever render scale is already saved (e.g. from a previous
        // session or the headset's own slider) once the web server is up
        var rsLoadTimer = new System.Windows.Forms.Timer { Interval = 2000 };
        rsLoadTimer.Tick += (s, e) => { rsLoadTimer.Stop(); LoadRenderScale(); };
        rsLoadTimer.Start();

        // The mirror's preview window is what OBS captures, so it stays visible.
        Spawn("mirror", Path.Combine(root, @"bin\VR180Mirror.exe"),
              MirrorArgs(extra, codec), Path.Combine(root, "bin"));

        Spawn("obs", @"C:\Program Files\obs-studio\bin\64bit\obs64.exe",
              "--multi --only-bundled-plugins --disable-shutdown-check --disable-updater "
              + "--profile VR180Mirror --collection VR180Mirror --startstreaming --minimize-to-tray",
              @"C:\Program Files\obs-studio\bin\64bit");

        obsLogPath = NewestObsLog();
        // start tailing from the current end of file, not byte 0 - otherwise
        // every stale line from a previous run (OBS log, viewer error log)
        // gets dumped into the log view all at once on the first poll.
        obsLogPos = FileLength(obsLogPath);
        clientLogPos = FileLength(Path.Combine(root, @"web\client.log"));
        statusTimer = new System.Windows.Forms.Timer { Interval = 1000 };
        statusTimer.Tick += (s, e) => Poll();
        statusTimer.Start();
    }

    static string MirrorArgs(string extra, string codec)
    {
        // canvas must match Start-Spectator.ps1's own codec -> canvas mapping
        string size = codec == "hevc" ? "6144x3264" : codec == "av1" ? "4096x2048" : "3840x1920";
        string a = "--size " + size + " --fps 72 --preview 1280";
        if (extra.IndexOf("-TestGrid", StringComparison.OrdinalIgnoreCase) >= 0) a += " --test-grid";
        if (extra.IndexOf("-VR180", StringComparison.OrdinalIgnoreCase) >= 0) a += " --vr180";
        return a;
    }

    // ---------------------------------------------------------------- adb helpers
    static string AdbPath()
    {
        string[] cands = {
            Environment.ExpandEnvironmentVariables(@"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"),
        };
        foreach (var c in cands) { if (File.Exists(c)) return c; }
        try {
            string wingetRoot = Environment.ExpandEnvironmentVariables(@"%LOCALAPPDATA%\Microsoft\WinGet\Packages");
            if (Directory.Exists(wingetRoot))
                foreach (var d in Directory.GetDirectories(wingetRoot, "Google.PlatformTools*")) {
                    string p = Path.Combine(d, @"platform-tools\adb.exe");
                    if (File.Exists(p)) return p;
                }
        } catch { }
        return "adb";   // fall back to PATH
    }

    static string RunAdb(string args, int timeoutMs)
    {
        try {
            var psi = new ProcessStartInfo(AdbPath(), args) {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            var p = Process.Start(psi);
            string outp = p.StandardOutput.ReadToEnd();
            p.WaitForExit(timeoutMs);
            return outp;
        } catch { return null; }
    }

    // /settings is served by web/server.js on the PC itself, so it's reachable at
    // localhost regardless of the adb-reverse USB tunnel (that tunnel only exists
    // to let the *headset's browser* reach this same port).
    void PushRenderScale()
    {
        string json = "{\"renderScale\":" + renderScaleNud.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) + "}";
        System.Threading.Tasks.Task.Run(() => HttpPostJson("http://127.0.0.1:9080/settings", json, 1500));
    }

    void LoadRenderScale()
    {
        System.Threading.Tasks.Task.Run(() => {
            string body = HttpGet("http://127.0.0.1:9080/settings", 1500);
            if (body == null) return;
            var m = Regex.Match(body, "\"renderScale\"\\s*:\\s*([0-9.]+)");
            if (!m.Success) return;
            decimal v;
            if (decimal.TryParse(m.Groups[1].Value, System.Globalization.NumberStyles.Any,
                    System.Globalization.CultureInfo.InvariantCulture, out v)) {
                v = Math.Min(2.00m, Math.Max(1.00m, v));
                if (renderScaleNud.InvokeRequired) renderScaleNud.BeginInvoke((Action)(() => renderScaleNud.Value = v));
                else renderScaleNud.Value = v;
            }
        });
    }

    void PopulateDevices()
    {
        string outp = RunAdb("devices", 5000);
        deviceCombo.Items.Clear();
        deviceCombo.Items.Add("(auto - single device)");
        var found = new List<string>();
        if (outp != null) {
            foreach (var line in outp.Split('\n')) {
                var m = Regex.Match(line.Trim(), @"^(\S+)\s+device$");
                if (m.Success) found.Add(m.Groups[1].Value);
            }
        }
        foreach (var s in found) deviceCombo.Items.Add(s + "  (Quest, USB)");
        deviceCombo.SelectedIndex = 0;
        SizeCombo(deviceCombo);   // device serials can be wider than the placeholder text
        Log("sys", found.Count == 0 ? "no adb devices found (plug in the spectator Quest, USB debugging authorized)"
            : found.Count + " adb device(s) found: " + string.Join(", ", found), found.Count == 0 ? Warn : Accent);
    }

    // Times an adb push of a throwaway 64MB file to the selected (or sole) headset
    // and reports the measured throughput, so a USB 2.0 cable/port masquerading as
    // USB 3.0 can be caught before the stream starts choking on it.
    void MeasureUsbSpeed()
    {
        string serial = SelectedSerial();
        measureBtn.Enabled = false;
        measureLabel.Text = "measuring...";
        var bg = new System.ComponentModel.BackgroundWorker();
        bg.DoWork += (s, e) => {
            string tmp = Path.Combine(Path.GetTempPath(), "vr180_usbtest.bin");
            const long size = 64L * 1024 * 1024;
            try {
                var rnd = new Random();
                var buf = new byte[1024 * 1024];
                using (var f = File.Create(tmp)) {
                    for (long i = 0; i < size / buf.Length; i++) { rnd.NextBytes(buf); f.Write(buf, 0, buf.Length); }
                }
                string sArg = serial != null ? "-s " + serial + " " : "";
                var sw = Stopwatch.StartNew();
                string pushOut = RunAdb(sArg + "push \"" + tmp + "\" /data/local/tmp/vr180_usbtest.bin", 60000);
                sw.Stop();
                RunAdb(sArg + "shell rm -f /data/local/tmp/vr180_usbtest.bin", 5000);
                double secs = sw.Elapsed.TotalSeconds;
                double mbps = secs > 0 ? (size * 8.0 / 1e6) / secs : 0;
                e.Result = mbps;
            } catch (Exception ex) { e.Result = ex; }
            finally { try { File.Delete(tmp); } catch { } }
        };
        bg.RunWorkerCompleted += (s, e) => {
            measureBtn.Enabled = true;
            if (e.Result is double) {
                double mbps = (double)e.Result;
                int targetKbps = SelectedBitrateKbps();
                bool ok = mbps * 1000 > targetKbps * 1.5;   // headroom over the configured stream bitrate
                measureLabel.Text = string.Format("~{0:0} Mbps over USB{1}", mbps,
                    ok ? "" : " - LOW: below 1.5x the configured bitrate, check for a USB 2.0 cable/port");
                measureLabel.ForeColor = ok ? Good : Bad;
                Log("sys", "USB speed test: " + measureLabel.Text, ok ? Good : Bad);
            } else {
                measureLabel.Text = "measurement failed (is the headset connected and authorized?)";
                measureLabel.ForeColor = Bad;
            }
        };
        bg.RunWorkerAsync();
    }

    void KillLeftovers()
    {
        // a previous run (or the old shortcut) may still hold the ports
        foreach (string name in new[] { "mediamtx", "VR180Mirror", "obs64" }) {
            foreach (var p in Process.GetProcessesByName(name)) {
                try {
                    if (name == "obs64" && !ProcessOwnsProfile(p)) continue;
                    p.Kill();
                    Log("sys", "stopped leftover " + name + " (pid " + p.Id + ")", Dim);
                } catch { }
            }
        }
        try {
            var psi = new ProcessStartInfo("powershell",
                "-ExecutionPolicy Bypass -Command \"Get-CimInstance Win32_Process -Filter \\\"Name='node.exe'\\\" | "
                + "Where-Object { $_.CommandLine -like '*VR180Mirror*web*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }\"")
            { UseShellExecute = false, CreateNoWindow = true };
            Process.Start(psi).WaitForExit(8000);
        } catch { }
        try { Process.Start(new ProcessStartInfo("schtasks", "/end /tn \"VR180Mirror OBS\"")
            { UseShellExecute = false, CreateNoWindow = true }).WaitForExit(5000); } catch { }
    }

    static bool ProcessOwnsProfile(Process p)
    {
        try { return p.MainWindowTitle.IndexOf("OBS", StringComparison.OrdinalIgnoreCase) >= 0 || true; }
        catch { return true; }
    }

    void RunToCompletion(string exe, string args, string cwd, string tag)
    {
        try {
            var psi = new ProcessStartInfo(exe, args) {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = cwd,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
            p.OutputDataReceived += (s, e) => { if (e.Data != null) Log(tag, e.Data, ColorFor(e.Data)); };
            p.ErrorDataReceived += (s, e) => { if (e.Data != null) Log(tag, e.Data, Bad); };
            p.Start();
            AssignProcessToJobObject(job, p.Handle);
            p.BeginOutputReadLine(); p.BeginErrorReadLine();
            p.WaitForExit(180000);
        } catch (Exception ex) { Log(tag, "failed: " + ex.Message, Bad); }
    }

    void Spawn(string tag, string exe, string args, string cwd)
    {
        try {
            var psi = new ProcessStartInfo(exe, args) {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = cwd,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
            p.OutputDataReceived += (s, e) => { if (e.Data != null) Log(tag, e.Data, ColorFor(e.Data)); };
            p.ErrorDataReceived += (s, e) => { if (e.Data != null) Log(tag, e.Data, ColorFor(e.Data)); };
            p.Exited += (s, e) => Log(tag, "process exited (code " + SafeExit(p) + ")", Warn);
            p.Start();
            AssignProcessToJobObject(job, p.Handle);   // dies with this window
            p.BeginOutputReadLine(); p.BeginErrorReadLine();
            children.Add(p);
            Log("sys", "started " + tag + " (pid " + p.Id + ")", Good);
        } catch (Exception ex) {
            Log("sys", "could not start " + tag + ": " + ex.Message, Bad);
        }
    }

    static string SafeExit(Process p) { try { return p.ExitCode.ToString(); } catch { return "?"; } }

    // ---------------------------------------------------------------- polling
    string NewestObsLog()
    {
        try {
            var dir = new DirectoryInfo(Environment.ExpandEnvironmentVariables(@"%APPDATA%\obs-studio\logs"));
            if (!dir.Exists) return null;
            FileInfo newest = null;
            foreach (var f in dir.GetFiles("*.txt"))
                if (newest == null || f.LastWriteTime > newest.LastWriteTime) newest = f;
            return newest == null ? null : newest.FullName;
        } catch { return null; }
    }

    static long FileLength(string path)
    {
        try { return (path != null && File.Exists(path)) ? new FileInfo(path).Length : 0; } catch { return 0; }
    }

    void TailFile(ref long pos, string path, string tag)
    {
        try {
            if (path == null || !File.Exists(path)) return;
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
                if (pos == 0 && fs.Length > 4096) pos = fs.Length;   // start at the end
                if (fs.Length < pos) pos = 0;                        // file rotated
                fs.Seek(pos, SeekOrigin.Begin);
                using (var sr = new StreamReader(fs)) {
                    string line;
                    while ((line = sr.ReadLine()) != null) {
                        if (line.Trim().Length > 0) Log(tag, line.Trim(), ColorFor(line));
                    }
                    pos = fs.Position;
                }
            }
        } catch { }
    }

    static string HttpGet(string url, int timeoutMs)
    {
        try {
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.Timeout = timeoutMs; req.ReadWriteTimeout = timeoutMs;
            using (var resp = (HttpWebResponse)req.GetResponse())
            using (var sr = new StreamReader(resp.GetResponseStream())) return sr.ReadToEnd();
        } catch { return null; }
    }

    static void HttpPostJson(string url, string json, int timeoutMs)
    {
        try {
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.Method = "POST"; req.ContentType = "application/json";
            req.Timeout = timeoutMs; req.ReadWriteTimeout = timeoutMs;
            byte[] bytes = Encoding.UTF8.GetBytes(json);
            req.ContentLength = bytes.Length;
            using (var s = req.GetRequestStream()) s.Write(bytes, 0, bytes.Length);
            using (req.GetResponse()) { }
        } catch { }
    }

    long lastBytes = 0; DateTime lastPoll = DateTime.MinValue;

    void Poll()
    {
        if (obsLogPath == null) obsLogPath = NewestObsLog();
        TailFile(ref obsLogPos, obsLogPath, "obs");
        TailFile(ref clientLogPos, Path.Combine(root, @"web\client.log"), "viewer");

        string paths = HttpGet("http://127.0.0.1:9998/v3/paths/list", 800);
        string sb = "";
        if (paths == null) {
            sb = "MediaMTX not answering";
        } else {
            bool ready = paths.Contains("\"ready\": true") || paths.Contains("\"ready\":true");
            var m = Regex.Match(paths, "\"bytesReceived\":\\s*(\\d+)");
            double mbps = 0;
            if (m.Success) {
                long b = long.Parse(m.Groups[1].Value);
                var now = DateTime.Now;
                if (lastPoll != DateTime.MinValue && b >= lastBytes) {
                    double secs = (now - lastPoll).TotalSeconds;
                    if (secs > 0.2) mbps = (b - lastBytes) * 8.0 / 1e6 / secs;
                }
                lastBytes = b; lastPoll = now;
            }
            sb = (ready ? "stream LIVE" : "stream idle") + "   " + mbps.ToString("0") + " Mbps";
        }

        string info = HttpGet("http://127.0.0.1:9080/info", 800);
        if (info != null) {
            var mm = Regex.Match(info, "\"mirrorLive\":(true|false)");
            var fs = Regex.Match(info, "\"srcFps\":(\\d+)");
            var hs = Regex.Match(info, "\"hspan\":([\\d.]+)");
            sb += "   |   mirror " + (mm.Success && mm.Groups[1].Value == "true" ? "LIVE from SteamVR" : "idle (grid)");
            if (fs.Success) sb += " @" + fs.Groups[1].Value + "fps";
            if (hs.Success) sb += "  fov " + hs.Groups[1].Value + "deg";
        } else {
            sb += "   |   web server not answering";
        }

        statusLabel.Text = "  " + sb;
    }

    // ---------------------------------------------------------------- shutdown
    void Shutdown()
    {
        try { if (statusTimer != null) statusTimer.Stop(); } catch { }
        // Be polite first, then let the job guarantee it. Killing rather than
        // asking is deliberate: OBS would otherwise pop a confirmation dialog.
        foreach (var p in children) {
            try { if (!p.HasExited) p.Kill(); } catch { }
        }
        // closing the job handle terminates anything still alive
        try { if (job != IntPtr.Zero) { CloseHandle(job); job = IntPtr.Zero; } } catch { }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
}
