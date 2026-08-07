using System.Windows.Forms;

namespace Tokenometer.Services;

internal sealed class TrayIconService : IDisposable
{
    private readonly NotifyIcon _icon;
    private readonly ContextMenuStrip _menu;
    private readonly ToolStripMenuItem _widgetItem;
    private bool _disposed;

    public TrayIconService()
    {
        var openItem = new ToolStripMenuItem("打开 Tokenometer");
        openItem.Click += (_, _) => OpenDashboardRequested?.Invoke(this, EventArgs.Empty);

        _widgetItem = new ToolStripMenuItem("显示桌面组件");
        _widgetItem.Click += (_, _) => ToggleWidgetRequested?.Invoke(this, EventArgs.Empty);

        var refreshItem = new ToolStripMenuItem("立即刷新");
        refreshItem.Click += (_, _) => RefreshRequested?.Invoke(this, EventArgs.Empty);

        var exitItem = new ToolStripMenuItem("退出");
        exitItem.Click += (_, _) => ExitRequested?.Invoke(this, EventArgs.Empty);

        _menu = new ContextMenuStrip();
        _menu.Items.AddRange([
            openItem,
            _widgetItem,
            new ToolStripSeparator(),
            refreshItem,
            new ToolStripSeparator(),
            exitItem
        ]);

        _icon = new NotifyIcon
        {
            ContextMenuStrip = _menu,
            Icon = System.Drawing.SystemIcons.Application,
            Text = "Tokenometer · Codex 用量",
            Visible = true
        };
        _icon.DoubleClick += (_, _) => OpenDashboardRequested?.Invoke(this, EventArgs.Empty);
    }

    public event EventHandler? OpenDashboardRequested;
    public event EventHandler? ToggleWidgetRequested;
    public event EventHandler? RefreshRequested;
    public event EventHandler? ExitRequested;

    public void SetWidgetVisible(bool visible)
    {
        _widgetItem.Text = visible ? "隐藏桌面组件" : "显示桌面组件";
        _widgetItem.Checked = visible;
    }

    public void ShowBackgroundTip()
    {
        _icon.BalloonTipTitle = "Tokenometer 仍在运行";
        _icon.BalloonTipText = "实时用量已移到桌面组件；双击托盘图标可恢复主界面。";
        _icon.BalloonTipIcon = ToolTipIcon.Info;
        _icon.ShowBalloonTip(3000);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _icon.Visible = false;
        _icon.Dispose();
        _menu.Dispose();
    }
}
