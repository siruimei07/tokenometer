using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using Tokenometer.Interop;
using Tokenometer.Services;
using Tokenometer.ViewModels;

namespace Tokenometer;

public partial class WidgetWindow : Window
{
    private const double ExpandedWidth = 366;
    private const double ExpandedHeight = 236;
    private const double CollapsedWidth = 252;
    private const double CollapsedHeight = 60;
    private bool _allowClose;
    private bool _positioned;

    public WidgetWindow(DashboardViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
        SourceInitialized += (_, _) => DwmBackdrop.Apply(this, transient: true);
    }

    public event EventHandler? OpenDashboardRequested;
    public event EventHandler? HideRequested;

    public void ShowAtDefaultPosition()
    {
        if (!_positioned)
        {
            var workArea = SystemParameters.WorkArea;
            Left = workArea.Right - Width - 24;
            Top = workArea.Bottom - Height - 24;
            _positioned = true;
        }

        Show();
    }

    public void CaptureSnapshot(string path)
    {
        var liveBackdrop = WidgetBackdropScene.Background;
        WidgetBackdropScene.Background = new SolidColorBrush(Color.FromRgb(78, 88, 82));
        try
        {
            SnapshotService.Capture(WidgetRoot, path);
        }
        finally
        {
            WidgetBackdropScene.Background = liveBackdrop;
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

    private void DragSurface_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left)
        {
            return;
        }

        if (e.ClickCount == 2)
        {
            OpenDashboardRequested?.Invoke(this, EventArgs.Empty);
            return;
        }

        DragMove();
    }

    private void Open_Click(object sender, RoutedEventArgs e) => OpenDashboardRequested?.Invoke(this, EventArgs.Empty);

    private void Hide_Click(object sender, RoutedEventArgs e) => HideRequested?.Invoke(this, EventArgs.Empty);

    private void Pin_Click(object sender, RoutedEventArgs e)
    {
        Topmost = !Topmost;
        PinButton.Content = Topmost ? "\uE718" : "\uE77A";
        PinButton.ToolTip = Topmost ? "取消置顶" : "保持置顶";
    }

    private void Collapse_Click(object sender, RoutedEventArgs e)
    {
        ExpandedPanel.Visibility = Visibility.Collapsed;
        CollapsedPanel.Visibility = Visibility.Visible;
        AnimateSize(CollapsedWidth, CollapsedHeight);
    }

    private void Expand_Click(object sender, RoutedEventArgs e)
    {
        CollapsedPanel.Visibility = Visibility.Collapsed;
        ExpandedPanel.Visibility = Visibility.Visible;
        AnimateSize(ExpandedWidth, ExpandedHeight);
    }

    private void AnimateSize(double width, double height)
    {
        var duration = TimeSpan.FromMilliseconds(180);
        BeginAnimation(WidthProperty, new DoubleAnimation(width, duration)
        {
            EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut }
        });
        BeginAnimation(HeightProperty, new DoubleAnimation(height, duration)
        {
            EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut }
        });
    }
}
