using System.IO;
using System.Windows;
using Tokenometer.Services;
using Tokenometer.ViewModels;

namespace Tokenometer;

public partial class App : System.Windows.Application
{
    private DashboardViewModel? _viewModel;
    private MainWindow? _mainWindow;
    private WidgetWindow? _widgetWindow;
    private TrayIconService? _trayIcon;
    private bool _isExiting;
    private bool _shownBackgroundTip;

    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _viewModel = new DashboardViewModel(new DemoUsageSource());
        await _viewModel.StartAsync();

        var dashboardSnapshot = ArgumentAfter(e.Args, "--snapshot");
        var widgetSnapshot = ArgumentAfter(e.Args, "--widget-snapshot");
        if (dashboardSnapshot is not null)
        {
            await CaptureDashboardAsync(dashboardSnapshot);
            return;
        }
        if (widgetSnapshot is not null)
        {
            await CaptureWidgetAsync(widgetSnapshot);
            return;
        }

        _mainWindow = new MainWindow(_viewModel);
        _widgetWindow = new WidgetWindow(_viewModel);
        _trayIcon = new TrayIconService();

        _mainWindow.HideRequested += (_, _) => HideDashboardToWidget();
        _mainWindow.WidgetRequested += (_, _) => ShowWidget();
        _widgetWindow.OpenDashboardRequested += (_, _) => OpenDashboard();
        _widgetWindow.HideRequested += (_, _) => HideWidget();
        _trayIcon.OpenDashboardRequested += (_, _) => OpenDashboard();
        _trayIcon.ToggleWidgetRequested += (_, _) => ToggleWidget();
        _trayIcon.RefreshRequested += async (_, _) => await _viewModel.RefreshAsync();
        _trayIcon.ExitRequested += (_, _) => ExitApplication();

        MainWindow = _mainWindow;
        _mainWindow.Show();
        _trayIcon.SetWidgetVisible(false);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _isExiting = true;
        _trayIcon?.Dispose();
        _viewModel?.Dispose();
        base.OnExit(e);
    }

    private async Task CaptureDashboardAsync(string path)
    {
        _mainWindow = new MainWindow(_viewModel!);
        MainWindow = _mainWindow;
        _mainWindow.Show();
        try
        {
            await Task.Delay(900);
            _mainWindow.CaptureSnapshot(path);
        }
        catch (Exception exception)
        {
            File.WriteAllText(path + ".error.txt", exception.ToString());
        }
        finally
        {
            ExitApplication();
        }
    }

    private async Task CaptureWidgetAsync(string path)
    {
        _widgetWindow = new WidgetWindow(_viewModel!);
        _widgetWindow.ShowAtDefaultPosition();
        try
        {
            await Task.Delay(900);
            _widgetWindow.CaptureSnapshot(path);
        }
        catch (Exception exception)
        {
            File.WriteAllText(path + ".error.txt", exception.ToString());
        }
        finally
        {
            ExitApplication();
        }
    }

    private void HideDashboardToWidget()
    {
        if (_isExiting || _mainWindow is null || _widgetWindow is null)
        {
            return;
        }

        _mainWindow.Hide();
        _widgetWindow.ShowAtDefaultPosition();
        _trayIcon?.SetWidgetVisible(true);
        if (!_shownBackgroundTip)
        {
            _shownBackgroundTip = true;
            _trayIcon?.ShowBackgroundTip();
        }
    }

    private void OpenDashboard()
    {
        if (_isExiting || _mainWindow is null)
        {
            return;
        }

        _widgetWindow?.Hide();
        _trayIcon?.SetWidgetVisible(false);
        _mainWindow.Show();
        _mainWindow.WindowState = WindowState.Normal;
        _mainWindow.Activate();
    }

    private void HideWidget()
    {
        _widgetWindow?.Hide();
        _trayIcon?.SetWidgetVisible(false);
    }

    private void ShowWidget()
    {
        if (_widgetWindow is null)
        {
            return;
        }

        _widgetWindow.ShowAtDefaultPosition();
        _trayIcon?.SetWidgetVisible(true);
    }

    private void ToggleWidget()
    {
        if (_widgetWindow is null)
        {
            return;
        }

        if (_widgetWindow.IsVisible)
        {
            HideWidget();
        }
        else
        {
            ShowWidget();
        }
    }

    private void ExitApplication()
    {
        if (_isExiting)
        {
            return;
        }

        _isExiting = true;
        _trayIcon?.Dispose();
        _trayIcon = null;
        _widgetWindow?.CloseForExit();
        _mainWindow?.CloseForExit();
        Shutdown();
    }

    private static string? ArgumentAfter(string[] arguments, string name)
    {
        var index = Array.IndexOf(arguments, name);
        return index >= 0 && index + 1 < arguments.Length ? arguments[index + 1] : null;
    }
}
