// VR180Mirror bandwidth monitor
// Shows, once per second:
//   IN  - what OBS feeds MediaMTX (encoder output)
//   OUT - what is actually delivered to the spectator (WebRTC session bytes)
//   the configured encoder cap, and the NIC utilisation vs link speed.
// Data comes from the MediaMTX control API on 127.0.0.1:9998. Optional tool:
// close the window any time; Start-Spectator -NoMonitor skips launching it.
//
// Built by tools\build-monitor.ps1 with the .NET Framework csc (no packages).

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

public class MonitorForm : Form
{
    const string API = "http://127.0.0.1:9998";
    static readonly Color BG = ColorTranslator.FromHtml("#0e141b");
    static readonly Color INK = ColorTranslator.FromHtml("#e2eaf1");
    static readonly Color DIM = ColorTranslator.FromHtml("#93a3b1");
    static readonly Color ACC = ColorTranslator.FromHtml("#3ec3dc");
    static readonly Color ORANGE = ColorTranslator.FromHtml("#ef9350");
    static readonly Color GOOD = ColorTranslator.FromHtml("#58c274");
    static readonly Color GRID = ColorTranslator.FromHtml("#273240");

    long prevIn = -1, prevOut = -1;
    double inMbps, outMbps;
    double capMbps = 150, maxMbps;
    bool usbViewer, apiOk;
    int viewers;
    string nicName = "";
    double nicMbps, nicLinkMbps;
    long prevNicBytes = -1;
    readonly List<double> history = new List<double>();
    readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
    DateTime lastCapRead = DateTime.MinValue;

    [STAThread]
    public static void Main()
    {
        Application.EnableVisualStyles();
        Application.Run(new MonitorForm());
    }

    public MonitorForm()
    {
        Text = "VR180 Bandwidth Monitor";
        ClientSize = new Size(640, 400);
        BackColor = BG;
        DoubleBuffered = true;
        FormBorderStyle = FormBorderStyle.Sizable;
        timer.Interval = 1000;
        timer.Tick += (s, e) => { Sample(); Invalidate(); };
        timer.Start();
        Sample();
    }

    static string HttpGet(string url)
    {
        var req = (HttpWebRequest)WebRequest.Create(url);
        req.Timeout = 800;
        req.ReadWriteTimeout = 800;
        using (var resp = (HttpWebResponse)req.GetResponse())
        using (var sr = new StreamReader(resp.GetResponseStream()))
            return sr.ReadToEnd();
    }

    void Sample()
    {
        try
        {
            // encoder output arriving at MediaMTX
            string paths = HttpGet(API + "/v3/paths/list");
            var m = Regex.Match(paths, "\"name\":\\s*\"vr180\"[^}]*?\"bytesReceived\":\\s*(\\d+)");
            if (!m.Success) m = Regex.Match(paths, "\"bytesReceived\":\\s*(\\d+)");
            long bytesIn = m.Success ? long.Parse(m.Groups[1].Value) : 0;
            if (prevIn >= 0 && bytesIn >= prevIn) inMbps = (bytesIn - prevIn) * 8.0 / 1e6;
            prevIn = bytesIn;

            // delivered to spectators (webrtc read sessions on the path)
            string sess = HttpGet(API + "/v3/webrtcsessions/list");
            long bytesOut = 0; int nView = 0; bool usb = false;
            foreach (Match c in Regex.Matches(sess, "\\{[^{}]*\\}"))
            {
                string chunk = c.Value;
                if (!chunk.Contains("\"path\": \"vr180\"") && !chunk.Contains("\"path\":\"vr180\"")) continue;
                if (!chunk.Contains("\"state\": \"read\"") && !chunk.Contains("\"state\":\"read\"")) continue;
                var bs = Regex.Match(chunk, "\"bytesSent\":\\s*(\\d+)");
                if (bs.Success) bytesOut += long.Parse(bs.Groups[1].Value);
                if (chunk.Contains("127.0.0.1")) usb = true;
                nView++;
            }
            if (prevOut >= 0 && bytesOut >= prevOut) outMbps = (bytesOut - prevOut) * 8.0 / 1e6;
            prevOut = bytesOut;
            viewers = nView;
            usbViewer = usb;
            apiOk = true;
        }
        catch { apiOk = false; inMbps = outMbps = 0; prevIn = prevOut = -1; viewers = 0; }

        // configured cap (encoder settings), re-read every 10 s
        if ((DateTime.Now - lastCapRead).TotalSeconds > 10)
        {
            lastCapRead = DateTime.Now;
            try
            {
                string enc = File.ReadAllText(Environment.ExpandEnvironmentVariables(
                    "%APPDATA%\\obs-studio\\basic\\profiles\\VR180Mirror\\streamEncoder.json"));
                var mb = Regex.Match(enc, "\"bitrate\":\\s*(\\d+)");
                if (mb.Success) capMbps = long.Parse(mb.Groups[1].Value) / 1000.0;
                var mm = Regex.Match(enc, "\"max_bitrate\":\\s*(\\d+)");
                maxMbps = mm.Success ? long.Parse(mm.Groups[1].Value) / 1000.0 : 0;
            }
            catch { }
        }

        // NIC utilisation vs link speed (the fastest operational non-loopback NIC)
        try
        {
            NetworkInterface best = null;
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                if (ni.Description.ToLower().Contains("virtual")) continue;
                if (best == null || ni.Speed > best.Speed) best = ni;
            }
            if (best != null)
            {
                nicName = best.Name;
                nicLinkMbps = best.Speed / 1e6;
                long tx = best.GetIPStatistics().BytesSent;
                if (prevNicBytes >= 0 && tx >= prevNicBytes) nicMbps = (tx - prevNicBytes) * 8.0 / 1e6;
                prevNicBytes = tx;
            }
        }
        catch { }

        history.Add(outMbps);
        if (history.Count > 120) history.RemoveAt(0);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(BG);
        int W = ClientSize.Width, H = ClientSize.Height;
        var fBig = new Font("Segoe UI", 22, FontStyle.Bold);
        var fMed = new Font("Segoe UI", 11, FontStyle.Bold);
        var fSm = new Font("Segoe UI", 9);
        int pad = 20;

        if (!apiOk)
        {
            g.DrawString("Pipeline offline", fBig, new SolidBrush(DIM), pad, pad);
            g.DrawString("MediaMTX API (127.0.0.1:9998) is not answering.\nStart the pipeline with the desktop shortcut.", fSm, new SolidBrush(DIM), pad, pad + 48);
            return;
        }

        // headline numbers
        g.DrawString("OUT to spectator", fSm, new SolidBrush(DIM), pad, pad);
        g.DrawString(outMbps.ToString("0.0") + " Mbps", fBig, new SolidBrush(ACC), pad - 4, pad + 14);
        g.DrawString("IN from OBS", fSm, new SolidBrush(DIM), pad + 230, pad);
        g.DrawString(inMbps.ToString("0.0") + " Mbps", fBig, new SolidBrush(ORANGE), pad + 226, pad + 14);
        string capTxt = maxMbps > 0
            ? "encoder " + capMbps.ToString("0") + "-" + maxMbps.ToString("0") + " Mbps VBR"
            : "encoder cap " + capMbps.ToString("0") + " Mbps CBR";
        g.DrawString(capTxt, fSm, new SolidBrush(DIM), pad + 450, pad);
        string vTxt = viewers == 0 ? "no viewer connected"
            : viewers + (viewers == 1 ? " viewer" : " viewers") + (usbViewer ? " (USB tunnel)" : " (Wi-Fi/LAN)");
        g.DrawString(vTxt, fMed, new SolidBrush(viewers > 0 ? GOOD : DIM), pad + 450, pad + 22);

        // usage bar: OUT vs cap
        double capRef = Math.Max(1, maxMbps > 0 ? maxMbps : capMbps);
        int barY = pad + 70, barH = 22;
        g.FillRectangle(new SolidBrush(GRID), pad, barY, W - 2 * pad, barH);
        int fill = (int)Math.Min(W - 2 * pad, (outMbps / capRef) * (W - 2 * pad));
        g.FillRectangle(new SolidBrush(ACC), pad, barY, fill, barH);
        g.DrawString("delivered vs configured bitrate", fSm, new SolidBrush(DIM), pad, barY + barH + 3);

        // NIC bar: available headroom (only meaningful for Wi-Fi/LAN viewers)
        int nicY = barY + 62;
        g.FillRectangle(new SolidBrush(GRID), pad, nicY, W - 2 * pad, barH);
        if (nicLinkMbps > 0)
        {
            int nf = (int)Math.Min(W - 2 * pad, (nicMbps / nicLinkMbps) * (W - 2 * pad));
            g.FillRectangle(new SolidBrush(GOOD), pad, nicY, nf, barH);
        }
        string nicTxt = usbViewer
            ? "NIC " + nicName + ": " + nicMbps.ToString("0.0") + " / " + nicLinkMbps.ToString("0") + " Mbps  -  spectator is on the USB cable, LAN unaffected"
            : "NIC " + nicName + ": " + nicMbps.ToString("0.0") + " used / " + nicLinkMbps.ToString("0") + " Mbps link  (" + Math.Max(0, nicLinkMbps - nicMbps).ToString("0") + " available)";
        g.DrawString(nicTxt, fSm, new SolidBrush(DIM), pad, nicY + barH + 3);

        // 2-minute history sparkline of OUT
        int gy = nicY + 64, gh = H - gy - pad - 14;
        if (gh > 30)
        {
            g.FillRectangle(new SolidBrush(ColorTranslator.FromHtml("#131a22")), pad, gy, W - 2 * pad, gh);
            double peak = capRef;
            foreach (var v in history) peak = Math.Max(peak, v);
            // cap reference line
            float capLine = (float)(gy + gh - (capRef / peak) * gh);
            g.DrawLine(new Pen(ORANGE) { DashStyle = DashStyle.Dash }, pad, capLine, W - pad, capLine);
            if (history.Count > 1)
            {
                var pts = new PointF[history.Count];
                for (int i = 0; i < history.Count; i++)
                {
                    float x = pad + (float)i / (120 - 1) * (W - 2 * pad);
                    float y = (float)(gy + gh - (history[i] / peak) * gh);
                    pts[i] = new PointF(x, Math.Max(gy, Math.Min(gy + gh, y)));
                }
                g.DrawLines(new Pen(ACC, 2f), pts);
            }
            g.DrawString("delivered bitrate, last 2 minutes (dashed = configured)", fSm, new SolidBrush(DIM), pad, gy + gh + 2);
        }
    }
}
