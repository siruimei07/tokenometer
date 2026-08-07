using System.Windows;

namespace Tokenometer;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);
        System.Windows.Application.Current.Shutdown();
    }
}
