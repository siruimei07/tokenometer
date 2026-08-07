using System.Windows;
using System.Windows.Media;

namespace Tokenometer.Controls;

public sealed class QuotaRing : FrameworkElement
{
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value),
        typeof(double),
        typeof(QuotaRing),
        new FrameworkPropertyMetadata(0d, FrameworkPropertyMetadataOptions.AffectsRender));

    public double Value
    {
        get => (double)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        var size = Math.Min(ActualWidth, ActualHeight);
        if (size <= 0)
        {
            return;
        }

        var center = new Point(ActualWidth / 2, ActualHeight / 2);
        var radius = Math.Max(0, size / 2 - 10);
        var track = new Pen(new SolidColorBrush(Color.FromArgb(72, 109, 120, 150)), 12)
        {
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round
        };
        var progress = new Pen(
            new LinearGradientBrush(
                Color.FromRgb(108, 117, 246),
                Color.FromRgb(75, 199, 181),
                new Point(0, 0),
                new Point(1, 1)),
            12)
        {
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round
        };

        const double startAngle = 140;
        const double sweep = 260;
        drawingContext.DrawGeometry(null, track, CreateArc(center, radius, startAngle, sweep));
        drawingContext.DrawGeometry(null, progress, CreateArc(center, radius, startAngle, sweep * Math.Clamp(Value, 0, 1)));
    }

    private static Geometry CreateArc(Point center, double radius, double startAngle, double sweepAngle)
    {
        var start = PointOnCircle(center, radius, startAngle);
        var end = PointOnCircle(center, radius, startAngle + sweepAngle);
        var geometry = new StreamGeometry();
        using var context = geometry.Open();
        context.BeginFigure(start, false, false);
        context.ArcTo(end, new Size(radius, radius), 0, Math.Abs(sweepAngle) > 180, SweepDirection.Clockwise, true, false);
        geometry.Freeze();
        return geometry;
    }

    private static Point PointOnCircle(Point center, double radius, double angle)
    {
        var radians = angle * Math.PI / 180;
        return new Point(center.X + Math.Cos(radians) * radius, center.Y + Math.Sin(radians) * radius);
    }
}

