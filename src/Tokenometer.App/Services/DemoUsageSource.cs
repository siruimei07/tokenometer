using Tokenometer.Models;

namespace Tokenometer.Services;

public sealed class DemoUsageSource : IUsageSource
{
    private readonly DateTimeOffset _startedAt = DateTimeOffset.Now;
    private readonly Random _random = new(2607);
    private readonly List<UsagePoint> _pulse;

    public DemoUsageSource()
    {
        var now = DateTimeOffset.Now;
        _pulse = Enumerable.Range(0, 40)
            .Select(index => CreatePoint(now.AddSeconds((index - 39) * 7.5), index))
            .ToList();
    }

    public string DisplayName => "UI 演示数据";

    public Task<UsageSnapshot> GetSnapshotAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var now = DateTimeOffset.Now;
        var tick = (int)(now - _startedAt).TotalSeconds;
        if (tick > 0 && tick % 2 == 0 && _pulse[^1].Timestamp < now.AddSeconds(-1))
        {
            _pulse.RemoveAt(0);
            _pulse.Add(CreatePoint(now, tick + 40));
        }

        var trend = Enumerable.Range(0, 28)
            .Select(index => CreateTrendPoint(now.AddMinutes(index - 27), index, tick))
            .ToArray();

        var sessions = new[]
        {
            new SessionSummary("桌面监测器 UI", "gpt-5.6-sol", now.AddMinutes(-2), 84_620 + tick * 31L, .63, "#6C75F6"),
            new SessionSummary("配额数据源调研", "gpt-5.6-sol", now.AddMinutes(-28), 42_180, .38, "#39B8D8"),
            new SessionSummary("图表交互验证", "gpt-5.6-terra", now.AddHours(-2), 18_940, .21, "#9A6AF2")
        };

        var snapshot = new UsageSnapshot(
            UsageSourceKind.Demo,
            DisplayName,
            now,
            "gpt-5.6-sol",
            68_440 + tick * 23L,
            41_760 + tick * 18L,
            16_180 + tick * 8L,
            .63,
            .72 - Math.Min(tick / 18_000d, .12),
            now.Date.AddDays(1).AddHours(1),
            "5 小时窗口",
            trend,
            _pulse.ToArray(),
            sessions);

        return Task.FromResult(snapshot);
    }

    private UsagePoint CreatePoint(DateTimeOffset timestamp, int index)
    {
        var wave = (Math.Sin(index * .64) + 1.25) * 1_250;
        var spike = index % 9 == 0 ? 3_400 : 0;
        var input = (long)(1_000 + wave + spike + _random.Next(0, 900));
        var cached = (long)(input * (.42 + _random.NextDouble() * .25));
        var output = 300 + _random.Next(160, 1_800);
        return new UsagePoint(timestamp, input, cached, output);
    }

    private static UsagePoint CreateTrendPoint(DateTimeOffset timestamp, int index, int tick)
    {
        var broadWave = 5_800 + Math.Sin((index + tick / 12d) * .42) * 2_700;
        var localWave = Math.Cos(index * 1.07) * 1_100;
        var input = Math.Max(850, (long)(broadWave + localWave + (index % 8 == 0 ? 3_600 : 0)));
        var cached = (long)(input * (.48 + (index % 4) * .06));
        var output = Math.Max(240, (long)(1_200 + Math.Sin(index * .79) * 740));
        return new UsagePoint(timestamp, input, cached, output);
    }
}

