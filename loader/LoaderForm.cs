using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace aimwhere
{
    /// Borderless, fully hand-drawn window: banner, version line, a big inject button, exit, status.
    /// Everything is laid out at 96 DPI and scaled once, so it stays sharp on scaled displays.
    internal sealed class LoaderForm : Form
    {
        private const int W = 460, H = 372;

        private static readonly Color Bg = Color.FromArgb(17, 17, 19);
        private static readonly Color Field = Color.FromArgb(28, 28, 33);
        private static readonly Color FieldHot = Color.FromArgb(36, 36, 43);
        private static readonly Color Accent = Color.FromArgb(173, 192, 255);
        private static readonly Color AccentHot = Color.FromArgb(198, 211, 255);
        private static readonly Color AccentDim = Color.FromArgb(92, 100, 132);
        private static readonly Color TextDim = Color.FromArgb(128, 128, 140);
        private static readonly Color Error = Color.FromArgb(255, 120, 130);

        private readonly Image m_banner;
        private readonly float m_scale;

        private readonly RectangleF m_inject = new RectangleF(70, 220, 320, 58);
        private readonly RectangleF m_exit = new RectangleF(70, 288, 320, 36);
        private readonly RectangleF m_close = new RectangleF(W - 30, 10, 20, 20);

        private readonly Font m_font_button = new Font("Segoe UI Semibold", 13f, FontStyle.Regular, GraphicsUnit.Pixel);
        private readonly Font m_font_inject = new Font("Segoe UI", 19f, FontStyle.Bold, GraphicsUnit.Pixel);
        private readonly Font m_font_small = new Font("Segoe UI", 12f, FontStyle.Regular, GraphicsUnit.Pixel);

        private string m_hover;
        private string m_pressed;
        private bool m_busy;
        private string m_version_line = "checking for updates...";
        private string m_status = "cs2 launches automatically if it isn't open";
        private bool m_status_error;
        private float m_pulse;

        private readonly Updater m_updater = new Updater();
        private Task<Updater.Result> m_update;

        public LoaderForm()
        {
            Text = "aimwhere";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Bg;
            DoubleBuffered = true;
            AutoScaleMode = AutoScaleMode.None;

            using (var g = CreateGraphics())
                m_scale = g.DpiX / 96f;
            ClientSize = new Size((int)(W * m_scale), (int)(H * m_scale));

            var asm = typeof(LoaderForm).Assembly;
            m_banner = Image.FromStream(asm.GetManifestResourceStream("banner.png"));
            Icon = new Icon(asm.GetManifestResourceStream("aimwhere.ico"));

            // Rounded window corners on Windows 11; ignored elsewhere.
            try
            {
                var round = 2;
                DwmSetWindowAttribute(Handle, 33, ref round, sizeof(int));
            }
            catch (Exception) { }

            var timer = new Timer { Interval = 33 };
            timer.Tick += (s, e) =>
            {
                if (!m_busy)
                    return;
                m_pulse = (m_pulse + 0.06f) % 1f;
                Invalidate();
            };
            timer.Start();

            Shown += (s, e) => StartUpdate();
        }

        private void StartUpdate()
        {
            m_update = m_updater.EnsureLatestAsync(s => BeginInvoke((Action)(() => SetStatus(s))));
            m_update.ContinueWith(t =>
            {
                BeginInvoke((Action)(() =>
                {
                    if (t.IsFaulted)
                    {
                        m_version_line = "couldn't reach the update server";
                        SetStatus(Root(t.Exception).Message, true);
                    }
                    else
                    {
                        m_version_line = "v" + t.Result.Version + "  ·  " + t.Result.Note;
                        if (!m_busy)
                            SetStatus("cs2 launches automatically if it isn't open");
                    }
                    Invalidate();
                }));
            });
        }

        private static Exception Root(Exception e)
        {
            while (e is AggregateException && e.InnerException != null)
                e = e.InnerException;
            return e;
        }

        private void SetStatus(string text, bool error = false)
        {
            m_status = text;
            m_status_error = error;
            Invalidate();
        }

        private async void Inject()
        {
            if (m_busy)
                return;
            m_busy = true;
            Invalidate();

            try
            {
                if (m_update == null || (m_update.IsFaulted && Updater.CachedVersion == null))
                    StartUpdate();
                if (!m_update.IsCompleted)
                    SetStatus("checking for updates...");
                try { await m_update; }
                catch (Exception) when (Updater.CachedVersion != null) { }

                if (Updater.CachedVersion == null)
                    throw new Exception("no build downloaded yet, check your connection");

                var process = Game.Find();
                if (process == null)
                {
                    SetStatus("launching cs2...");
                    Game.Launch();
                    process = await Game.WaitForProcessAsync(TimeSpan.FromMinutes(3));
                    if (process == null)
                        throw new Exception("cs2 didn't start, is steam logged in?");
                }

                SetStatus("waiting for cs2 to finish loading...");
                if (!await Game.WaitUntilLoadedAsync(process, TimeSpan.FromMinutes(4)))
                    throw new Exception(process.HasExited ? "cs2 closed before it finished loading" : "cs2 is taking too long to load");

                if (Game.IsInjected(process))
                {
                    SetStatus("already injected, press delete in game for the menu");
                    return;
                }

                // Loaded modules are not the same as a finished main menu; the renderer and Steam API want
                // a moment more before the hooks go in.
                SetStatus("injecting...");
                await Task.Delay(4000);
                if (process.HasExited)
                    throw new Exception("cs2 closed before injecting");

                await Task.Run(() => Game.Inject(process, Updater.DllPath));

                SetStatus("injected! press delete in game for the menu");
                await Task.Delay(3500);
                Close();
            }
            catch (Exception e)
            {
                SetStatus(Root(e).Message, true);
            }
            finally
            {
                m_busy = false;
                Invalidate();
            }
        }

        // ---- input ----------------------------------------------------------------------------------

        private PointF Logical(Point p) => new PointF(p.X / m_scale, p.Y / m_scale);

        private string HitTest(Point p)
        {
            var l = Logical(p);
            if (m_inject.Contains(l)) return "inject";
            if (m_exit.Contains(l)) return "exit";
            if (m_close.Contains(l)) return "close";
            return null;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            var hit = HitTest(e.Location);
            if (hit != m_hover)
            {
                m_hover = hit;
                Cursor = hit != null ? Cursors.Hand : Cursors.Default;
                Invalidate();
            }
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            m_hover = null;
            Invalidate();
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left)
                return;

            m_pressed = HitTest(e.Location);
            if (m_pressed == null)
            {
                // Drag the borderless window from anywhere that isn't a button.
                ReleaseCapture();
                SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero);
                return;
            }
            Invalidate();
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            var hit = HitTest(e.Location);
            var pressed = m_pressed;
            m_pressed = null;
            Invalidate();

            if (hit == null || hit != pressed)
                return;
            if (hit == "inject") Inject();
            else if (hit == "exit" || hit == "close") Close();
        }

        // ---- drawing --------------------------------------------------------------------------------

        private static GraphicsPath Rounded(RectangleF r, float radius)
        {
            var d = radius * 2;
            var path = new GraphicsPath();
            path.AddArc(r.X, r.Y, d, d, 180, 90);
            path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            return path;
        }

        private static void Centered(Graphics g, string text, Font font, Color color, RectangleF r)
        {
            using (var brush = new SolidBrush(color))
            using (var fmt = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap })
                g.DrawString(text, font, brush, r, fmt);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.ScaleTransform(m_scale, m_scale);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;

            g.DrawImage(m_banner, 0, 0, W, m_banner.Height * (float)W / m_banner.Width);

            // Accent strip along the top edge, brightest in the middle, like the in-game menu.
            using (var strip = new LinearGradientBrush(new RectangleF(0, 0, W, 2), AccentDim, AccentDim, 0f))
            {
                strip.InterpolationColors = new ColorBlend
                {
                    Colors = new[] { Color.FromArgb(140, 160, 255), Accent, Color.FromArgb(140, 160, 255) },
                    Positions = new[] { 0f, 0.5f, 1f },
                };
                g.FillRectangle(strip, 0, 0, W, 2);
            }

            // Close cross over the banner.
            using (var pen = new Pen(m_hover == "close" ? Color.White : TextDim, 1.6f))
            {
                var c = m_close;
                g.DrawLine(pen, c.X + 5, c.Y + 5, c.Right - 5, c.Bottom - 5);
                g.DrawLine(pen, c.Right - 5, c.Y + 5, c.X + 5, c.Bottom - 5);
            }

            Centered(g, m_version_line, m_font_small, TextDim, new RectangleF(0, 186, W, 22));

            // Inject: solid accent, lighter on hover, pressed nudges down a pixel, pulses while working.
            var inject = m_inject;
            if (m_pressed == "inject" && m_hover == "inject")
                inject.Offset(0, 1);

            var fill = m_busy ? AccentDim : m_hover == "inject" ? AccentHot : Accent;
            using (var path = Rounded(inject, 6))
            {
                using (var glow = new Pen(Color.FromArgb(m_busy ? 0 : m_hover == "inject" ? 90 : 45, Accent), 6f))
                    g.DrawPath(glow, path);
                using (var brush = new SolidBrush(fill))
                    g.FillPath(brush, path);

                if (m_busy)
                {
                    // A light band sweeping across the button while it works.
                    var x = inject.X - 80 + (inject.Width + 160) * m_pulse;
                    var state = g.Save();
                    g.SetClip(path);
                    using (var sweep = new LinearGradientBrush(new RectangleF(x, inject.Y, 80, inject.Height), Color.FromArgb(0, Accent), Color.FromArgb(0, Accent), 0f))
                    {
                        sweep.InterpolationColors = new ColorBlend
                        {
                            Colors = new[] { Color.FromArgb(0, Accent), Color.FromArgb(150, Accent), Color.FromArgb(0, Accent) },
                            Positions = new[] { 0f, 0.5f, 1f },
                        };
                        g.FillRectangle(sweep, x, inject.Y, 80, inject.Height);
                    }
                    g.Restore(state);
                }
            }
            Centered(g, m_busy ? "WORKING" : "INJECT", m_font_inject, Color.FromArgb(18, 18, 24), inject);

            var exit = m_exit;
            using (var path = Rounded(exit, 5))
            using (var brush = new SolidBrush(m_hover == "exit" ? FieldHot : Field))
                g.FillPath(brush, path);
            Centered(g, "Exit", m_font_button, m_hover == "exit" ? Color.White : Color.FromArgb(205, 205, 215), exit);

            Centered(g, m_status, m_font_small, m_status_error ? Error : TextDim, new RectangleF(16, 334, W - 32, 22));

            // One-pixel outline so the window edge reads against dark desktops.
            using (var pen = new Pen(Color.FromArgb(38, 38, 46)))
                g.DrawRectangle(pen, 0, 0, W - 1, H - 1);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                m_banner.Dispose();
                m_font_button.Dispose();
                m_font_inject.Dispose();
                m_font_small.Dispose();
            }
            base.Dispose(disposing);
        }

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        private static extern bool ReleaseCapture();

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        private static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wparam, IntPtr lparam);

        [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    }
}
