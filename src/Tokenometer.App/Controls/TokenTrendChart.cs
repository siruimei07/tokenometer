using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using Tokenometer.Models;

namespace Tokenometer.Controls;

public sealed class TokenTrendChart : FrameworkElement
{
    public static readonly DependencyProperty ValuesProperty = DependencyProperty.Register(
        nameof(Values),
        typeof(IReadOnlyList<UsagePoint>),
        typeof(TokenTrendChart),
        new FrameworkPropertyMetadata(Array.Empty<UsagePoint>(), FrameworkPropertyMetadataOptions.AffectsRender));

    private readonly System.Windows.Controls.ToolTip _toolTip = ChartToolTip.Create();
    private int _hoverIndex = -1;

    public TokenTrendChart()
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
        if (Values.Count == 0 || ActualWidth <= 24)
        {
            return;
        }

        var x = Math.Clamp(e.GetPosition(this).X - 12, 0, ActualWidth - 24);
        var index = (int)Math.Round(x / Math.Max(1, ActualWidth - 24) * (Values.Count - 1));
        if (index == _hoverIndex)
        {
            return;
        }

        _hoverIndex = Math.Clamp(index, 0, Values.Count - 1);
        var point = Values[_hoverIndex];
        _toolTip.Content = ChartToolTip.Content(
            point.Timestamp.ToString("HH:mm"),
            $"{point.TotalTokens:N0} tokens",
            $"输入 {point.InputTokens:N0}  ·  输出 {point.OutputTokens:N0}");
        _toolTip.IsOpen = true;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        if (ActualWidth < 40 || ActualHeight < 40 || Values.Count < 2)
        {
            return;
        }

        var plot = new Rect(12, 10, ActualWidth - 24, ActualHeight - 34);
        var max = Math.Max(1, Values.Max(point => point.TotalTokens) * 1.12);
        var gridPen = new Pen(new SolidColorBrush(Color.FromArgb(35, 89, 101, 130)), 1);
        for (var row = 0; row < 4; row++)
        {
            var y = plot.Top + plot.Height * row / 3;
            drawingContext.DrawLine(gridPen, new Point(plot.Left, y), new Point(plot.Right, y));
        }

        var points = Values.Select((point, index) => new Point(
            plot.Left + plot.Width * index / (Values.Count - 1),
            plot.Bottom - plot.Height * point.TotalTokens / max)).ToArray();

        var area = new StreamGeometry();
        using (var context = area.Open())
        {
            context.BeginFigure(new Point(points[0].X, plot.Bottom), true, true);
            foreach (var point in points)
            {
                context.LineTo(point, true, false);
            }
            context.LineTo(new Point(points[^1].X, plot.Bottom), true, false);
        }
        area.Freeze();
        drawingContext.DrawGeometry(
            new LinearGradientBrush(
                Color.FromArgb(95, 108, 117, 246),
                Color.FromArgb(3, 108, 117, 246),
                new Point(0, 0),
                new Point(0, 1)),
            null,
            area);

        var line = new StreamGeometry();
        using (var context = line.Open())
        {
            context.BeginFigure(points[0], false, false);
            for (var index = 1; index < points.Length; index++)
            {
                context.LineTo(points[index], true, false);
            }
        }
        line.Freeze();
        drawingContext.DrawGeometry(null, new Pen(new SolidColorBrush(Color.FromRgb(108, 117, 246)), 2.3)
        {
            LineJoin = PenLineJoin.Round,
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round
        }, line);

        DrawAxisLabel(drawingContext, Values[0].Timestamp.ToString("HH:mm"), new Point(plot.Left, plot.Bottom + 8), TextAlignment.Left);
        DrawAxisLabel(drawingContext, Values[^1].Timestamp.ToString("HH:mm"), new Point(plot.Right, plot.Bottom + 8), TextAlignment.Right);

        if (_hoverIndex >= 0 && _hoverIndex < points.Length)
        {
            var focus = points[_hoverIndex];
            drawingContext.DrawLine(new Pen(new SolidColorBrush(Color.FromArgb(85, 108, 117, 246)), 1) { DashStyle = DashStyles.Dash }, new Point(focus.X, plot.Top), new Point(focus.X, plot.Bottom));
            drawingContext.DrawEllipse(new SolidColorBrush(Color.FromArgb(55, 108, 117, 246)), null, focus, 9, 9);
            drawingContext.DrawEllipse(Brushes.White, new Pen(new SolidColorBrush(Color.FromRgb(108, 117, 246)), 2), focus, 4.5, 4.5);
        }
    }

    private void DrawAxisLabel(DrawingContext context, string text, Point point, TextAlignment alignment)
    {
        var formatted = new FormattedText(
            text,
            CultureInfo.CurrentCulture,
            FlowDirection.LeftToRight,
            new Typeface("Segoe UI Variable Text"),
            10.5,
            new SolidColorBrush(Color.FromRgb(132, 142, 160)),
            VisualTreeHelper.GetDpi(this).PixelsPerDip)
        {
            TextAlignment = alignment
        };
        context.DrawText(formatted, point);
    }
}

