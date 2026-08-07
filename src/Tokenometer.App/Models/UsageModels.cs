namespace Tokenometer.Models;

public enum UsageSourceKind
{
    Demo,
    CodexLocal,
    OpenAiAccount
}

public sealed record UsagePoint(
    DateTimeOffset Timestamp,
    long InputTokens,
    long CachedInputTokens,
    long OutputTokens)
{
    public long TotalTokens => InputTokens + OutputTokens;
}

public sealed record SessionSummary(
    string Title,
    string Model,
    DateTimeOffset UpdatedAt,
    long TotalTokens,
    double ContextRatio,
    string Accent);

public sealed record UsageSnapshot(
    UsageSourceKind SourceKind,
    string SourceLabel,
    DateTimeOffset UpdatedAt,
    string CurrentModel,
    long CurrentInputTokens,
    long CurrentCachedInputTokens,
    long CurrentOutputTokens,
    double? ContextRatio,
    double? AllowanceRemainingRatio,
    DateTimeOffset? AllowanceResetsAt,
    string AllowanceWindowLabel,
    IReadOnlyList<UsagePoint> Trend,
    IReadOnlyList<UsagePoint> Pulse,
    IReadOnlyList<SessionSummary> RecentSessions)
{
    public long CurrentTotalTokens => CurrentInputTokens + CurrentOutputTokens;
}

