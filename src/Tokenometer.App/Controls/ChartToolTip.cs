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
            Foreground = new SolidColorBrush(Color.FromRgb(176, 190, 205))
        });
        panel.Children.Add(new TextBlock
        {
            Text = value,
            Margin = new System.Windows.Thickness(0, 3, 0, 2),
            FontSize = 16,
            FontWeight = System.Windows.FontWeights.SemiBold,
            Foreground = Brushes.White
        });
        panel.Children.Add(new TextBlock
        {
            Text = detail,
            FontSize = 11,
            Foreground = new SolidColorBrush(Color.FromRgb(190, 203, 216))
        });

        return new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(235, 16, 27, 41)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(120, 255, 255, 255)),
            BorderThickness = new System.Windows.Thickness(1),
            CornerRadius = new System.Windows.CornerRadius(13),
            Padding = new System.Windows.Thickness(12, 9, 12, 9),
            Effect = new System.Windows.Media.Effects.DropShadowEffect
            {
                Color = Color.FromRgb(2, 7, 15),
                BlurRadius = 22,
                ShadowDepth = 6,
                Opacity = .42
            },
            Child = panel
        };
    }
}
