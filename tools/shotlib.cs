//  shotlib.cs - screen capture and image comparison helpers for the
//  acceptance test.  Kept in C# because the PowerShell equivalents are
//  far too slow for a per-pixel diff, and because the capture must be
//  in true device pixels: a DPI-unaware process gets virtualised
//  coordinates and silently measures the wrong thing.
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Cap {
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, int rop);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);

    public static void Init() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }

    //  Captures a window's OWN content, whatever is on top of it.  This
    //  is the only way to see the HUD while a full screen application is
    //  running: a screen capture shows whatever the compositor put on
    //  top, which for the HUD is nothing at all - that is the whole
    //  point of the program.
    public static bool WindowShot(IntPtr hwnd, string path, int w, int h) {
        using (var b = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(b)) {
            IntPtr dst = g.GetHdc();
            bool ok = PrintWindow(hwnd, dst, 2);      //  PW_RENDERFULLCONTENT
            g.ReleaseHdc(dst);
            b.Save(path, ImageFormat.Png);
            return ok;
        }
    }

    //  Captures a rectangle of the screen in true device pixels.
    public static void Shot(string path, int x, int y, int w, int h) {
        using (var b = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(b)) {
            IntPtr dst = g.GetHdc(), src = GetDC(IntPtr.Zero);
            BitBlt(dst, 0, 0, w, h, src, x, y, 0x00CC0020);
            g.ReleaseHdc(dst);
            ReleaseDC(IntPtr.Zero, src);
            b.Save(path, ImageFormat.Png);
        }
    }

    //  Counts the pixels that differ between two images and returns the
    //  bounding box of the difference.
    public static string Diff(string a, string b) {
        using (var ba = new Bitmap(a))
        using (var bb = new Bitmap(b)) {
            int n = 0, minx = int.MaxValue, miny = int.MaxValue, maxx = -1, maxy = -1;
            for (int y = 0; y < ba.Height; y++)
                for (int x = 0; x < ba.Width; x++) {
                    var ca = ba.GetPixel(x, y); var cb = bb.GetPixel(x, y);
                    if (Math.Abs(ca.R - cb.R) > 12 || Math.Abs(ca.G - cb.G) > 12 || Math.Abs(ca.B - cb.B) > 12) {
                        n++;
                        if (x < minx) minx = x;
                        if (y < miny) miny = y;
                        if (x > maxx) maxx = x;
                        if (y > maxy) maxy = y;
                    }
                }
            if (n == 0) return "identical";
            return n + " px differ, box=(" + minx + "," + miny + ")-(" + maxx + "," + maxy + ")";
        }
    }

    //  How many of the pixels that changed between the baseline and the
    //  HUD shot are still changed when an application is on top.  Zero
    //  means the application covered the HUD completely.
    public static int Overlap(string baseline, string hud, string app) {
        using (var bb = new Bitmap(baseline))
        using (var bh = new Bitmap(hud))
        using (var ba = new Bitmap(app)) {
            int n = 0;
            for (int y = 0; y < bb.Height; y++)
                for (int x = 0; x < bb.Width; x++) {
                    var c0 = bb.GetPixel(x, y); var c1 = bh.GetPixel(x, y); var c2 = ba.GetPixel(x, y);
                    bool d1 = Math.Abs(c0.R - c1.R) > 12 || Math.Abs(c0.G - c1.G) > 12 || Math.Abs(c0.B - c1.B) > 12;
                    if (!d1) continue;
                    bool d2 = Math.Abs(c0.R - c2.R) > 12 || Math.Abs(c0.G - c2.G) > 12 || Math.Abs(c0.B - c2.B) > 12;
                    if (d2) n++;
                }
            return n;
        }
    }

    //  Counts the pixels inside a rectangle of image b that still look
    //  like image a.  Used the other way round: if the application shot
    //  still looks like the HUD shot inside the HUD's rectangle, the
    //  HUD is still on top of the application.
    public static int MatchCount(string a, string b, int x, int y, int w, int h, int tol) {
        using (var ba = new Bitmap(a))
        using (var bb = new Bitmap(b)) {
            int n = 0;
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++) {
                    var ca = ba.GetPixel(x + i, y + j); var cb = bb.GetPixel(x + i, y + j);
                    if (Math.Abs(ca.R - cb.R) <= tol && Math.Abs(ca.G - cb.G) <= tol &&
                        Math.Abs(ca.B - cb.B) <= tol) n++;
                }
            return n;
        }
    }

    //  Saves a rectangle of an image, for looking at.
    public static void Crop(string src, string dst, int x, int y, int w, int h) {
        using (var b = new Bitmap(src))
        using (var c = b.Clone(new Rectangle(x, y, w, h), b.PixelFormat)) {
            c.Save(dst, ImageFormat.Png);
        }
    }

    //  The share of pixels that are (within a tolerance) the given
    //  colour.  Used to check that a covering window really is on top.
    public static double ColorFraction(string path, int r, int g, int b, int tol) {
        using (var im = new Bitmap(path)) {
            int n = 0;
            for (int y = 0; y < im.Height; y++)
                for (int x = 0; x < im.Width; x++) {
                    var c = im.GetPixel(x, y);
                    if (Math.Abs(c.R - r) <= tol && Math.Abs(c.G - g) <= tol && Math.Abs(c.B - b) <= tol) n++;
                }
            return (double)n / (im.Width * im.Height);
        }
    }
}
