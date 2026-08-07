using System.ComponentModel;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Windows.Threading;
using Tokenometer.Models;
using Tokenometer.Services;

namespace Tokenometer.ViewModels;

public sealed class DashboardViewModel : INotifyPropertyChanged, IDisposable
{
    private readonly IUsageSource _source;
    private readonly DispatcherTimer _timer;
    private readonly CancellationTokenSource _lifetime = new();
    private UsageSnapshot? _snapshot;
    private bool _isRefreshing;

    public DashboardViewModel(IUsageSource source)
    {
        _source = source;
        _timer = new DispatcherTimer(TimeSpan.FromSeconds(2), DispatcherPriority.Background, OnTimerTick, Dispatcher.CurrentDispatcher);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public string SourceLabel => _snapshot?.SourceLabel ?? _source.DisplayName;
    public string SourceBadge => _snapshot?.SourceKind == UsageSourceKind.Demo ? "DEMO" : "LIVE";
    public string UpdatedDisplay => _snapshot is null ? "正在连接" : $"更新于 {_snapshot.UpdatedAt:HH:mm:ss}";
    public string CurrentModel => _snapshot?.CurrentModel ?? "--";
    public string CurrentTotalDisplay => FormatCompact(_snapshot?.CurrentTotalTokens);
    public string InputDisplay => FormatCompact(_snapshot?.CurrentInputTokens);
    public string CachedDisplay => FormatCompact(_snapshot?.CurrentCachedInputTokens);
    public string OutputDisplay => FormatCompact(_snapshot?.CurrentOutputTokens);
    public long CurrentInputTokens => _snapshot?.CurrentInputTokens ?? 0;
    public long CurrentCachedInputTokens => _snapshot?.CurrentCachedInputTokens ?? 0;
    public long CurrentOutputTokens => _snapshot?.CurrentOutputTokens ?? 0;
    public string ContextDisplay => _snapshot?.ContextRatio is double ratio ? ratio.ToString("P0", CultureInfo.CurrentCulture) : "--";
    public double ContextRatio => _snapshot?.ContextRatio ?? 0;
    public string RemainingDisplay => _snapshot?.AllowanceRemainingRatio is double ratio ? ratio.ToString("P0", CultureInfo.CurrentCulture) : "--";
    public double AllowanceRemainingRatio => _snapshot?.AllowanceRemainingRatio ?? 0;
    public string AllowanceWindowLabel => _snapshot?.AllowanceWindowLabel ?? "配额窗口";
    public string ResetDisplay => _snapshot?.AllowanceResetsAt is DateTimeOffset reset ? $"{reset:HH:mm} 重置" : "未提供重置时间";
    public string TodayTotalDisplay => FormatCompact((_snapshot?.Trend.Sum(point => point.TotalTokens) ?? 0));
    public string TodayChangeDisplay => "+12.8%";
    public IReadOnlyList<UsagePoint> Trend => _snapshot?.Trend ?? Array.Empty<UsagePoint>();
    public IReadOnlyList<UsagePoint> Pulse => _snapshot?.Pulse ?? Array.Empty<UsagePoint>();
    public IReadOnlyList<SessionSummary> RecentSessions => _snapshot?.RecentSessions ?? Array.Empty<SessionSummary>();

    public async Task StartAsync()
    {
        await RefreshAsync();
        _timer.Start();
    }

    public async Task RefreshAsync()
    {
        if (_isRefreshing)
        {
            return;
        }

        _isRefreshing = true;
        try
        {
            _snapshot = await _source.GetSnapshotAsync(_lifetime.Token);
            RaiseAll();
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
        }
        finally
        {
            _isRefreshing = false;
        }
    }

    public void Dispose()
    {
        _timer.Stop();
        _lifetime.Cancel();
        _lifetime.Dispose();
    }

    private async void OnTimerTick(object? sender, EventArgs e)
    {
        await RefreshAsync();
    }

    private void RaiseAll()
    {
        foreach (var property in GetType().GetProperties().Where(property => property.Name != nameof(PropertyChanged)))
        {
            OnPropertyChanged(property.Name);
        }
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    private static string FormatCompact(long? value)
    {
        return value switch
        {
            null => "--",
            >= 1_000_000 => $"{value / 1_000_000d:0.00}M",
            >= 1_000 => $"{value / 1_000d:0.0}K",
            _ => value.Value.ToString("N0", CultureInfo.CurrentCulture)
        };
    }
}
