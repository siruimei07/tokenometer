using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using Tokenometer.Models;

namespace Tokenometer.Controls;

public sealed class TokenPulseChart : FrameworkElement
{
    public static readonly DependencyProperty ValuesProperty = DependencyProperty.Register(
        nameof(Values),
        typeof(IReadOnlyList<UsagePoint>),
        typeof(TokenPulseChart),
        new FrameworkPropertyMetadata(Array.Empty<UsagePoint>(), FrameworkPropertyMetadataOptions.AffectsRender));

    private readonly System.Windows.Controls.ToolTip _toolTip = ChartToolTip.Create();
    private int _hoverIndex = -1;

    public TokenPulseChart()
    {
        ToolTip = _toolTip;
        MouseLeave += (_, _) =>
        {
            _hoverIndex = -1;
            _toolTip.IsOpen = false;
            InvalidateVisual();
        };
    }

    public IReadOnlyList<UsagePoint> Values
    {
        get => (IReadOnlyList<UsagePoint>)GetValue(ValuesProperty);
        set => SetValue(ValuesProperty, value);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (Values.Count == 0)
        {
            return;
        }

        var index = (int)(Math.Clamp(e.GetPosition(this).X, 0, Math.Max(0, ActualWidth - 1)) / Math.Max(1, ActualWidth) * Values.Count);
        index = Math.Clamp(index, 0, Values.Count - 1);
        if (index == _hoverIndex)
        {
            return;
        }

        _hoverIndex = index;
        var point = Values[index];
        _toolTip.Content = ChartToolTip.Content(
            point.Timestamp.ToString("HH:mm:ss"),
            $"{point.TotalTokens:N0} tokens",
            $"缓存命中 {point.CachedInputTokens:N0}  ·  输出 {point.OutputTokens:N0}");
        _toolTip.IsOpen = true;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        if (ActualWidth < 30 || ActualHeight < 30 || Values.Count == 0)
        {
            return;
        }

        const int rows = 11;
        var max = Math.Max(1, Values.Max(point => point.TotalTokens));
        var columnWidth = ActualWidth / Values.Count;
        var dotSize = Math.Clamp(Math.Min(columnWidth * .55, ActualHeight / (rows + 1) * .55), 2.3, 5.2);
        var rowPitch = ActualHeight / rows;

        for (var column = 0; column < Values.Count; column++)
        {
            var point = Values[column];
            var filled = Math.Clamp((int)Math.Round(point.TotalTokens / (double)max * rows), 1, rows);
            var outputShare = point.OutputTokens / (double)Math.Max(1, point.TotalTokens);
            var outputDots = Math.Max(1, (int)Math.Round(filled * outputShare));
            var opacity = _hoverIndex < 0 || Math.Abs(column - _hoverIndex) <= 1 ? 1d : .22;

            for (var row = 0; row < rows; row++)
            {
                var center = new Point(columnWidth * (column + .5), ActualHeight - rowPitch * (row + .55));
                Brush brush;
                if (row >= filled)
                {
                    brush = new SolidColorBrush(Color.FromArgb((byte)(32 * opacity), 92, 105, 134));
                }
                else if (row >= filled - outputDots)
                {
                    brush = new SolidColorBrush(Color.FromArgb((byte)(245 * opacity), 154, 106, 242));
                }
                else
                {
                    brush = new SolidColorBrush(Color.FromArgb((byte)(235 * opacity), 57, 184, 216));
                }
                drawingContext.DrawEllipse(brush, null, center, dotSize / 2, dotSize / 2);
            }
        }

        if (_hoverIndex >= 0)
        {
            var x = columnWidth * (_hoverIndex + .5);
            drawingContext.DrawRoundedRectangle(
                new SolidColorBrush(Color.FromArgb(22, 108, 117, 246)),
                new Pen(new SolidColorBrush(Color.FromArgb(70, 108, 117, 246)), 1),
                new Rect(x - columnWidth * .55, 0, columnWidth * 1.1, ActualHeight),
                columnWidth * .4,
                columnWidth * .4);
        }
    }
}
