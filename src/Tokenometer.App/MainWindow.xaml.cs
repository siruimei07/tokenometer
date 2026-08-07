using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using Tokenometer.Interop;
using Tokenometer.Services;
using Tokenometer.ViewModels;

namespace Tokenometer;

public partial class MainWindow : Window
{
    private readonly DashboardViewModel _viewModel;
    private CancellationTokenSource? _toastLifetime;
    private bool _allowClose;

    public MainWindow(DashboardViewModel viewModel)
    {
        InitializeComponent();
        _viewModel = viewModel;
        DataContext = viewModel;
        SourceInitialized += (_, _) => DwmBackdrop.Apply(this, transient: true);
        StateChanged += (_, _) =>
        {
            if (WindowState == WindowState.Minimized)
            {
                WindowState = WindowState.Normal;
                HideRequested?.Invoke(this, EventArgs.Empty);
            }
        };
    }

    public event EventHandler? HideRequested;

    public event EventHandler? WidgetRequested;

    public void CaptureSnapshot(string path)
    {
        var liveBackdrop = BackdropScene.Background;
        BackdropScene.Background = SnapshotService.CreateProofBackdrop();
        TrendGlass.SetInteractionPreview(new Point(.73, .12));
        try
        {
            SnapshotService.Capture(WindowShell, path);
        }
        finally
        {
            BackdropScene.Background = liveBackdrop;
            TrendGlass.SetInteractionPreview(null);
        }
    }

    public void CloseForExit()
    {
        _allowClose = true;
        Close();
    }

    protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
    {
        if (!_allowClose)
        {
            e.Cancel = true;
            HideRequested?.Invoke(this, EventArgs.Empty);
            return;
        }

        base.OnClosing(e);
    }

    protected override void OnClosed(EventArgs e)
    {
        _toastLifetime?.Cancel();
        _toastLifetime?.Dispose();
        base.OnClosed(e);
    }

    private void DragRegion_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left)
        {
            return;
        }

        if (e.ClickCount == 2)
        {
            ToggleMaximize();
            return;
        }

        if (WindowState == WindowState.Normal)
        {
            DragMove();
        }
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e) => await _viewModel.RefreshAsync();

    private void Minimize_Click(object sender, RoutedEventArgs e) => HideRequested?.Invoke(this, EventArgs.Empty);

    private void Maximize_Click(object sender, RoutedEventArgs e) => ToggleMaximize();

    private void Close_Click(object sender, RoutedEventArgs e) => HideRequested?.Invoke(this, EventArgs.Empty);

    private void Widget_Click(object sender, RoutedEventArgs e) => WidgetRequested?.Invoke(this, EventArgs.Empty);

    private void ReservedNavigation_Checked(object sender, RoutedEventArgs e) =>
        ShowToast("数据接入后启用该页面");

    private void ReservedButton_Click(object sender, RoutedEventArgs e) =>
        ShowToast("接口已保留，将随真实数据源启用");

    private void ToggleMaximize()
    {
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    }

    private async void ShowToast(string message)
    {
        _toastLifetime?.Cancel();
        _toastLifetime?.Dispose();
        _toastLifetime = new CancellationTokenSource();
        var token = _toastLifetime.Token;

        FeatureToastText.Text = message;
        FeatureToast.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(140)));
        try
        {
            await Task.Delay(1_650, token);
            FeatureToast.BeginAnimation(OpacityProperty, new DoubleAnimation(0, TimeSpan.FromMilliseconds(220)));
        }
        catch (OperationCanceledException)
        {
        }
    }
}
