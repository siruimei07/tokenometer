using System.Windows;
using System.IO;
using Tokenometer.Services;

namespace Tokenometer;

public partial class App : System.Windows.Application
{
    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var snapshotArgument = Array.IndexOf(e.Args, "--snapshot");
        var snapshotPath = snapshotArgument >= 0 && snapshotArgument + 1 < e.Args.Length
            ? e.Args[snapshotArgument + 1]
            : null;
        var window = new MainWindow();
        window.Show();

        if (snapshotPath is not null)
        {
            try
            {
                await Task.Delay(900);
                window.CaptureSnapshot(snapshotPath);
            }
            catch (Exception exception)
            {
                File.WriteAllText(snapshotPath + ".error.txt", exception.ToString());
            }
            finally
            {
                window.Hide();
                Shutdown();
            }
        }
    }
}
