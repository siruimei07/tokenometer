using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace Tokenometer.Interop;

internal static class DwmBackdrop
{
    private const int WindowCornerPreference = 33;
    private const int SystemBackdropType = 38;

    public static void Apply(Window window, bool transient = false)
    {
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000))
        {
            return;
        }

        try
        {
            var handle = new WindowInteropHelper(window).Handle;
            if (handle == IntPtr.Zero)
            {
                return;
            }

            if (HwndSource.FromHwnd(handle) is { CompositionTarget: { } target })
            {
                target.BackgroundColor = Colors.Transparent;
            }

            var margins = new Margins(-1);
            _ = DwmExtendFrameIntoClientArea(handle, ref margins);

            var corner = 2; // DWMWCP_ROUND
            _ = DwmSetWindowAttribute(handle, WindowCornerPreference, ref corner, sizeof(int));

            if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22621))
            {
                var backdrop = transient ? 3 : 2; // acrylic-like transient / mica main window
                _ = DwmSetWindowAttribute(handle, SystemBackdropType, ref backdrop, sizeof(int));
            }
        }
        catch (DllNotFoundException)
        {
        }
        catch (EntryPointNotFoundException)
        {
        }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmExtendFrameIntoClientArea(IntPtr windowHandle, ref Margins margins);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr windowHandle, int attribute, ref int value, int valueSize);

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct Margins
    {
        public Margins(int value)
        {
            Left = value;
            Right = value;
            Top = value;
            Bottom = value;
        }

        public int Left { get; }
        public int Right { get; }
        public int Top { get; }
        public int Bottom { get; }
    }
}

