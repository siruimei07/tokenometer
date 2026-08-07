using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;

namespace Tokenometer.Controls;

internal static class ChartToolTip
{
    public static ToolTip Create()
    {
        return new ToolTip
        {
            Background = Brushes.Transparent,
            BorderThickness = new System.Windows.Thickness(0),
            Padding = new System.Windows.Thickness(0),
            Placement = PlacementMode.MousePoint,
            HorizontalOffset = 14,
            VerticalOffset = 16,
            StaysOpen = true
        };
    }

    public static Border Content(string eyebrow, string value, string detail)
    {
        var panel = new StackPanel();
        panel.Children.Add(new TextBlock
        {
            Text = eyebrow,
            FontSize = 11,
            Foreground = new SolidColorBrush(Color.FromRgb(128, 138, 157))
        });
        panel.Children.Add(new TextBlock
        {
            Text = value,
            Margin = new System.Windows.Thickness(0, 3, 0, 2),
            FontSize = 16,
            FontWeight = System.Windows.FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Color.FromRgb(27, 35, 54))
        });
        panel.Children.Add(new TextBlock
        {
            Text = detail,
            FontSize = 11,
            Foreground = new SolidColorBrush(Color.FromRgb(98, 108, 128))
        });

        return new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(238, 255, 255, 255)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(220, 255, 255, 255)),
            BorderThickness = new System.Windows.Thickness(1),
            CornerRadius = new System.Windows.CornerRadius(13),
            Padding = new System.Windows.Thickness(12, 9, 12, 9),
            Effect = new System.Windows.Media.Effects.DropShadowEffect
            {
                Color = Color.FromRgb(60, 72, 110),
                BlurRadius = 18,
                ShadowDepth = 4,
                Opacity = .18
            },
            Child = panel
        };
    }
}

