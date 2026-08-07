using System.Windows;
using System.Windows.Media;

namespace Tokenometer.Controls;

public sealed class TokenCompositionBar : FrameworkElement
{
    public static readonly DependencyProperty InputProperty = Register(nameof(Input));
    public static readonly DependencyProperty CachedProperty = Register(nameof(Cached));
    public static readonly DependencyProperty OutputProperty = Register(nameof(Output));

    public double Input { get => (double)GetValue(InputProperty); set => SetValue(InputProperty, value); }
    public double Cached { get => (double)GetValue(CachedProperty); set => SetValue(CachedProperty, value); }
    public double Output { get => (double)GetValue(OutputProperty); set => SetValue(OutputProperty, value); }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        var radius = Math.Max(0, ActualHeight / 2);
        drawingContext.PushClip(new RectangleGeometry(new Rect(0, 0, ActualWidth, ActualHeight), radius, radius));
        drawingContext.DrawRectangle(new SolidColorBrush(Color.FromArgb(50, 103, 114, 143)), null, new Rect(0, 0, ActualWidth, ActualHeight));

        var input = Math.Max(0, Input - Cached);
        var cached = Math.Max(0, Cached);
        var output = Math.Max(0, Output);
        var total = input + cached + output;
        if (total > 0)
        {
            var inputWidth = ActualWidth * input / total;
            var cachedWidth = ActualWidth * cached / total;
            drawingContext.DrawRectangle(new SolidColorBrush(Color.FromRgb(57, 184, 216)), null, new Rect(0, 0, inputWidth, ActualHeight));
            drawingContext.DrawRectangle(new SolidColorBrush(Color.FromRgb(88, 199, 161)), null, new Rect(inputWidth, 0, cachedWidth, ActualHeight));
            drawingContext.DrawRectangle(new SolidColorBrush(Color.FromRgb(154, 106, 242)), null, new Rect(inputWidth + cachedWidth, 0, Math.Max(0, ActualWidth - inputWidth - cachedWidth), ActualHeight));
        }
        drawingContext.Pop();
    }

    private static DependencyProperty Register(string name) => DependencyProperty.Register(
        name,
        typeof(double),
        typeof(TokenCompositionBar),
        new FrameworkPropertyMetadata(0d, FrameworkPropertyMetadataOptions.AffectsRender));
}

