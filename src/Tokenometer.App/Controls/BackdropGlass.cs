using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Threading;

namespace Tokenometer.Controls;

/// <summary>Samples a sibling background visual and renders it as live, blurred glass.</summary>
[TemplatePart(Name = BackdropPartName, Type = typeof(BackdropLayer))]
[TemplatePart(Name = LensPartName, Type = typeof(LensLayer))]
[TemplatePart(Name = ChromePartName, Type = typeof(ChromeLayer))]
public sealed class BackdropGlass : ContentControl
{
    private const string BackdropPartName = "PART_Backdrop";
    private const string LensPartName = "PART_Lens";
    private const string ChromePartName = "PART_Chrome";
    private static readonly Brush DefaultTint = Frozen(new SolidColorBrush(Color.FromArgb(42, 255, 255, 255)));
    private static readonly Brush DefaultBorder = Frozen(new SolidColorBrush(Color.FromArgb(150, 255, 255, 255)));
    private static readonly Brush DefaultHighlight = Frozen(new LinearGradientBrush(
        Color.FromArgb(220, 255, 255, 255),
        Color.FromArgb(18, 255, 255, 255),
        new Point(0, 0),
        new Point(1, 1)));
    private static readonly ControlTemplate DefaultTemplate = CreateTemplate();

    public static readonly DependencyProperty BackdropVisualProperty = DependencyProperty.Register(
        nameof(BackdropVisual),
        typeof(Visual),
        typeof(BackdropGlass),
        Changed());

    public static readonly DependencyProperty CornerRadiusProperty = DependencyProperty.Register(
        nameof(CornerRadius),
        typeof(CornerRadius),
        typeof(BackdropGlass),
        Changed(new CornerRadius(24)),
        value => IsValid((CornerRadius)value));

    public static readonly DependencyProperty TintBrushProperty = DependencyProperty.Register(
        nameof(TintBrush),
        typeof(Brush),
        typeof(BackdropGlass),
        Changed(DefaultTint));

    public static readonly DependencyProperty BlurRadiusProperty = DependencyProperty.Register(
        nameof(BlurRadius),
        typeof(double),
        typeof(BackdropGlass),
        Changed(28d),
        value => value is double radius && double.IsFinite(radius) && radius is >= 0 and <= 100);

    public static readonly DependencyProperty SaturationTintProperty = DependencyProperty.Register(
        nameof(SaturationTint),
        typeof(Brush),
        typeof(BackdropGlass),
        Changed());

    public static readonly DependencyProperty HighlightBrushProperty = DependencyProperty.Register(
        nameof(HighlightBrush),
        typeof(Brush),
        typeof(BackdropGlass),
        Changed(DefaultHighlight));

    public static readonly DependencyProperty LensWidthProperty = DependencyProperty.Register(
        nameof(LensWidth),
        typeof(double),
        typeof(BackdropGlass),
        Changed(9d),
        value => IsFiniteRange(value, 0, 36));

    public static readonly DependencyProperty RefractionDepthProperty = DependencyProperty.Register(
        nameof(RefractionDepth),
        typeof(double),
        typeof(BackdropGlass),
        Changed(3.5d),
        value => IsFiniteRange(value, 0, 18));

    public static readonly DependencyProperty RefractionOpacityProperty = DependencyProperty.Register(
        nameof(RefractionOpacity),
        typeof(double),
        typeof(BackdropGlass),
        Changed(.72d),
        value => IsFiniteRange(value, 0, 1));

    public static readonly DependencyProperty PointerGlowOpacityProperty = DependencyProperty.Register(
        nameof(PointerGlowOpacity),
        typeof(double),
        typeof(BackdropGlass),
        Changed(.085d),
        value => IsFiniteRange(value, 0, .3));

    private BackdropLayer? _backdropLayer;
    private LensLayer? _lensLayer;
    private ChromeLayer? _chromeLayer;
    private GlassRoot? _root;
    private readonly DispatcherTimer _interactionTimer;
    private Point _pointerPosition = new(.5, 0);
    private double _pointerEnergy;
    private double _pointerTarget;

    static BackdropGlass()
    {
        TemplateProperty.OverrideMetadata(typeof(BackdropGlass), new FrameworkPropertyMetadata(DefaultTemplate));
        BackgroundProperty.OverrideMetadata(typeof(BackdropGlass), Changed(Brushes.Transparent));
        BorderBrushProperty.OverrideMetadata(typeof(BackdropGlass), Changed(DefaultBorder));
        BorderThicknessProperty.OverrideMetadata(typeof(BackdropGlass), Changed(new Thickness(1)));
    }

    public BackdropGlass()
    {
        SnapsToDevicePixels = true;
        SetValue(TemplateProperty, DefaultTemplate);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        _interactionTimer = new DispatcherTimer(
            TimeSpan.FromMilliseconds(16),
            DispatcherPriority.Render,
            OnInteractionTick,
            Dispatcher);
    }

    public Visual? BackdropVisual
    {
        get => (Visual?)GetValue(BackdropVisualProperty);
        set => SetValue(BackdropVisualProperty, value);
    }

    public CornerRadius CornerRadius
    {
        get => (CornerRadius)GetValue(CornerRadiusProperty);
        set => SetValue(CornerRadiusProperty, value);
    }

    public Brush? TintBrush
    {
        get => (Brush?)GetValue(TintBrushProperty);
        set => SetValue(TintBrushProperty, value);
    }

    public double BlurRadius
    {
        get => (double)GetValue(BlurRadiusProperty);
        set => SetValue(BlurRadiusProperty, value);
    }

    /// <summary>An optional color wash; WPF has no native saturation shader.</summary>
    public Brush? SaturationTint
    {
        get => (Brush?)GetValue(SaturationTintProperty);
        set => SetValue(SaturationTintProperty, value);
    }

    public Brush? HighlightBrush
    {
        get => (Brush?)GetValue(HighlightBrushProperty);
        set => SetValue(HighlightBrushProperty, value);
    }

    /// <summary>Width of the edge-only lens band, in device-independent pixels.</summary>
    public double LensWidth
    {
        get => (double)GetValue(LensWidthProperty);
        set => SetValue(LensWidthProperty, value);
    }

    /// <summary>How far the live backdrop is magnified inside the lens band.</summary>
    public double RefractionDepth
    {
        get => (double)GetValue(RefractionDepthProperty);
        set => SetValue(RefractionDepthProperty, value);
    }

    public double RefractionOpacity
    {
        get => (double)GetValue(RefractionOpacityProperty);
        set => SetValue(RefractionOpacityProperty, value);
    }

    public double PointerGlowOpacity
    {
        get => (double)GetValue(PointerGlowOpacityProperty);
        set => SetValue(PointerGlowOpacityProperty, value);
    }

    public override void OnApplyTemplate()
    {
        base.OnApplyTemplate();
        _root = GetTemplateChild("PART_Root") as GlassRoot;
        _backdropLayer = GetTemplateChild(BackdropPartName) as BackdropLayer;
        _lensLayer = GetTemplateChild(LensPartName) as LensLayer;
        _chromeLayer = GetTemplateChild(ChromePartName) as ChromeLayer;
        RefreshLayers();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        RefreshLayers();
    }

    protected override void OnMouseEnter(MouseEventArgs e)
    {
        base.OnMouseEnter(e);
        _pointerTarget = 1;
        _interactionTimer.Start();
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        var point = e.GetPosition(this);
        _pointerPosition = new Point(
            ActualWidth <= 0 ? .5 : Math.Clamp(point.X / ActualWidth, 0, 1),
            ActualHeight <= 0 ? .5 : Math.Clamp(point.Y / ActualHeight, 0, 1));
        _chromeLayer?.InvalidateVisual();
    }

    protected override void OnMouseLeave(MouseEventArgs e)
    {
        base.OnMouseLeave(e);
        _pointerTarget = 0;
        _interactionTimer.Start();
    }

    private void RefreshLayers()
    {
        _root?.UpdateClip();
        _backdropLayer?.InvalidateVisual();
        _lensLayer?.InvalidateVisual();
        _chromeLayer?.InvalidateVisual();
    }

    private void OnInteractionTick(object? sender, EventArgs e)
    {
        _pointerEnergy += (_pointerTarget - _pointerEnergy) * .2;
        if (Math.Abs(_pointerTarget - _pointerEnergy) < .012)
        {
            _pointerEnergy = _pointerTarget;
            _interactionTimer.Stop();
        }
        _chromeLayer?.InvalidateVisual();
    }

    private bool IsSafeSource(Visual? source) =>
        source is not null &&
        !ReferenceEquals(source, this) &&
        !source.IsAncestorOf(this) &&
        !IsAncestorOf(source);

    private static FrameworkPropertyMetadata Changed(object? defaultValue = null) =>
        new(defaultValue, FrameworkPropertyMetadataOptions.AffectsRender, OnGlassPropertyChanged);

    private static void OnGlassPropertyChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs e)
    {
        var glass = (BackdropGlass)dependencyObject;
        glass._backdropLayer?.Reset();
        glass._lensLayer?.Reset();
        glass.RefreshLayers();
    }

    private static ControlTemplate CreateTemplate()
    {
#pragma warning disable CS0618 // FrameworkElementFactory is the only public code-only WPF template builder.
        var root = new FrameworkElementFactory(typeof(GlassRoot), "PART_Root");
        var backdrop = new FrameworkElementFactory(typeof(BackdropLayer), BackdropPartName);
        backdrop.SetValue(FrameworkElement.MarginProperty, new Thickness(-150));
        backdrop.SetValue(IsHitTestVisibleProperty, false);
        root.AppendChild(backdrop);

        var lens = new FrameworkElementFactory(typeof(LensLayer), LensPartName);
        lens.SetValue(IsHitTestVisibleProperty, false);
        root.AppendChild(lens);

        var chrome = new FrameworkElementFactory(typeof(ChromeLayer), ChromePartName);
        chrome.SetValue(IsHitTestVisibleProperty, false);
        root.AppendChild(chrome);

        var content = new FrameworkElementFactory(typeof(ContentPresenter));
        Bind(content, ContentPresenter.ContentProperty, nameof(Content));
        Bind(content, ContentPresenter.ContentTemplateProperty, nameof(ContentTemplate));
        Bind(content, ContentPresenter.ContentTemplateSelectorProperty, nameof(ContentTemplateSelector));
        Bind(content, ContentPresenter.ContentStringFormatProperty, nameof(ContentStringFormat));
        Bind(content, ContentPresenter.MarginProperty, nameof(Padding));
        Bind(content, HorizontalAlignmentProperty, nameof(HorizontalContentAlignment));
        Bind(content, VerticalAlignmentProperty, nameof(VerticalContentAlignment));
        content.SetValue(ContentPresenter.RecognizesAccessKeyProperty, true);
        root.AppendChild(content);

        return new ControlTemplate(typeof(BackdropGlass)) { VisualTree = root };
#pragma warning restore CS0618
    }

#pragma warning disable CS0618
    private static void Bind(FrameworkElementFactory element, DependencyProperty property, string path) =>
        element.SetBinding(property, new Binding(path) { RelativeSource = RelativeSource.TemplatedParent });
#pragma warning restore CS0618

    private static void DrawStroke(DrawingContext context, Rect bounds, CornerRadius radius, Brush brush, double width, double inset)
    {
        var rect = bounds;
        rect.Inflate(-inset, -inset);
        if (rect.Width <= 0 || rect.Height <= 0)
        {
            return;
        }

        var adjusted = new CornerRadius(
            Math.Max(0, radius.TopLeft - inset),
            Math.Max(0, radius.TopRight - inset),
            Math.Max(0, radius.BottomRight - inset),
            Math.Max(0, radius.BottomLeft - inset));
        context.DrawGeometry(null, new Pen(brush, width), RoundedRect(rect, adjusted));
    }

    private static Geometry RoundedRect(Rect rect, CornerRadius radius)
    {
        var halfWidth = rect.Width / 2;
        var halfHeight = rect.Height / 2;
        var topLeft = Math.Min(radius.TopLeft, Math.Min(halfWidth, halfHeight));
        var topRight = Math.Min(radius.TopRight, Math.Min(halfWidth, halfHeight));
        var bottomRight = Math.Min(radius.BottomRight, Math.Min(halfWidth, halfHeight));
        var bottomLeft = Math.Min(radius.BottomLeft, Math.Min(halfWidth, halfHeight));
        var geometry = new StreamGeometry();
        using (var context = geometry.Open())
        {
            context.BeginFigure(new Point(rect.Left + topLeft, rect.Top), true, true);
            context.LineTo(new Point(rect.Right - topRight, rect.Top), true, false);
            context.ArcTo(new Point(rect.Right, rect.Top + topRight), new Size(topRight, topRight), 0, false, SweepDirection.Clockwise, true, false);
            context.LineTo(new Point(rect.Right, rect.Bottom - bottomRight), true, false);
            context.ArcTo(new Point(rect.Right - bottomRight, rect.Bottom), new Size(bottomRight, bottomRight), 0, false, SweepDirection.Clockwise, true, false);
            context.LineTo(new Point(rect.Left + bottomLeft, rect.Bottom), true, false);
            context.ArcTo(new Point(rect.Left, rect.Bottom - bottomLeft), new Size(bottomLeft, bottomLeft), 0, false, SweepDirection.Clockwise, true, false);
            context.LineTo(new Point(rect.Left, rect.Top + topLeft), true, false);
            context.ArcTo(new Point(rect.Left + topLeft, rect.Top), new Size(topLeft, topLeft), 0, false, SweepDirection.Clockwise, true, false);
        }
        geometry.Freeze();
        return geometry;
    }

    private static Geometry RoundedRing(Rect bounds, CornerRadius radius, double inset)
    {
        var outer = RoundedRect(bounds, radius);
        var innerBounds = bounds;
        innerBounds.Inflate(-inset, -inset);
        if (innerBounds.Width <= 0 || innerBounds.Height <= 0)
        {
            return outer;
        }

        var innerRadius = new CornerRadius(
            Math.Max(0, radius.TopLeft - inset),
            Math.Max(0, radius.TopRight - inset),
            Math.Max(0, radius.BottomRight - inset),
            Math.Max(0, radius.BottomLeft - inset));
        return new CombinedGeometry(
            GeometryCombineMode.Exclude,
            outer,
            RoundedRect(innerBounds, innerRadius));
    }

    private static bool NearlyEquals(Rect left, Rect right) =>
        !left.IsEmpty && !right.IsEmpty &&
        Math.Abs(left.X - right.X) < .05 &&
        Math.Abs(left.Y - right.Y) < .05 &&
        Math.Abs(left.Width - right.Width) < .05 &&
        Math.Abs(left.Height - right.Height) < .05;

    private static bool IsValid(CornerRadius radius) =>
        IsValid(radius.TopLeft) && IsValid(radius.TopRight) &&
        IsValid(radius.BottomRight) && IsValid(radius.BottomLeft);

    private static bool IsValid(double value) => double.IsFinite(value) && value >= 0;

    private static bool IsFiniteRange(object value, double minimum, double maximum) =>
        value is double number && double.IsFinite(number) && number >= minimum && number <= maximum;

    private static T Frozen<T>(T freezable) where T : Freezable
    {
        freezable.Freeze();
        return freezable;
    }

    private sealed class GlassRoot : Grid
    {
        protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
        {
            base.OnRenderSizeChanged(sizeInfo);
            UpdateClip();
        }

        public void UpdateClip()
        {
            if (TemplatedParent is BackdropGlass owner && ActualWidth > 0 && ActualHeight > 0)
            {
                Clip = RoundedRect(new Rect(RenderSize), owner.CornerRadius);
            }
        }
    }

    private sealed class BackdropLayer : FrameworkElement
    {
        private readonly BlurEffect _blur = new() { RenderingBias = RenderingBias.Quality };
        private readonly VisualBrush _brush = new()
        {
            AlignmentX = AlignmentX.Left,
            AlignmentY = AlignmentY.Top,
            AutoLayoutContent = false,
            Stretch = Stretch.Fill,
            TileMode = TileMode.None,
            ViewboxUnits = BrushMappingMode.Absolute
        };
        private Rect _lastViewbox = Rect.Empty;

        public BackdropLayer()
        {
            Effect = _blur;
            LayoutUpdated += OnLayoutUpdated;
        }

        public bool HasSample { get; private set; }

        public void Reset()
        {
            _lastViewbox = Rect.Empty;
            _brush.Visual = null;
            HasSample = false;
        }

        protected override void OnRender(DrawingContext drawingContext)
        {
            base.OnRender(drawingContext);
            if (TryGetViewbox(out var owner, out var viewbox))
            {
                _lastViewbox = viewbox;
                _brush.Visual = owner.BackdropVisual;
                _brush.Viewbox = viewbox;
                _blur.Radius = owner.BlurRadius;
                HasSample = true;
                drawingContext.DrawRectangle(_brush, null, new Rect(RenderSize));
            }
            else
            {
                Reset();
            }
        }

        private void OnLayoutUpdated(object? sender, EventArgs e)
        {
            if (TryGetViewbox(out _, out var viewbox) && !NearlyEquals(viewbox, _lastViewbox))
            {
                InvalidateVisual();
            }
        }

        private bool TryGetViewbox(out BackdropGlass owner, out Rect viewbox)
        {
            viewbox = Rect.Empty;
            owner = (BackdropGlass)TemplatedParent;
            var source = owner.BackdropVisual;
            if (!owner.IsSafeSource(source) || RenderSize.Width <= 0 || RenderSize.Height <= 0)
            {
                return false;
            }

            try
            {
                viewbox = TransformToVisual(source).TransformBounds(new Rect(RenderSize));
                return !viewbox.IsEmpty && viewbox.Width > 0 && viewbox.Height > 0;
            }
            catch (InvalidOperationException)
            {
                return false;
            }
        }
    }

    /// <summary>
    /// Re-samples a slightly smaller backdrop region only along the perimeter. The magnified
    /// edge band creates an observable lens bend without distorting foreground content.
    /// </summary>
    private sealed class LensLayer : FrameworkElement
    {
        private readonly VisualBrush _brush = new()
        {
            AlignmentX = AlignmentX.Left,
            AlignmentY = AlignmentY.Top,
            AutoLayoutContent = false,
            Stretch = Stretch.Fill,
            TileMode = TileMode.None,
            ViewboxUnits = BrushMappingMode.Absolute
        };
        private Rect _lastViewbox = Rect.Empty;

        public LensLayer()
        {
            LayoutUpdated += OnLayoutUpdated;
        }

        public void Reset()
        {
            _lastViewbox = Rect.Empty;
            _brush.Visual = null;
        }

        protected override void OnRender(DrawingContext drawingContext)
        {
            base.OnRender(drawingContext);
            var owner = (BackdropGlass)TemplatedParent;
            if (owner.LensWidth <= 0 || owner.RefractionDepth <= 0 ||
                !TryGetViewbox(owner, out var viewbox))
            {
                Reset();
                return;
            }

            _lastViewbox = viewbox;
            var refracted = viewbox;
            var depth = Math.Min(owner.RefractionDepth, Math.Min(viewbox.Width, viewbox.Height) * .16);
            refracted.Inflate(-depth, -depth);
            if (refracted.Width <= 1 || refracted.Height <= 1)
            {
                return;
            }

            _brush.Visual = owner.BackdropVisual;
            _brush.Viewbox = refracted;

            var bounds = new Rect(RenderSize);
            var inset = Math.Min(owner.LensWidth, Math.Min(bounds.Width, bounds.Height) * .45);
            var ring = RoundedRing(bounds, owner.CornerRadius, inset);

            drawingContext.PushClip(ring);
            drawingContext.PushOpacity(owner.RefractionOpacity);
            drawingContext.DrawRectangle(_brush, null, bounds);
            drawingContext.Pop();
            drawingContext.Pop();
        }

        private void OnLayoutUpdated(object? sender, EventArgs e)
        {
            var owner = (BackdropGlass)TemplatedParent;
            if (TryGetViewbox(owner, out var viewbox) && !NearlyEquals(viewbox, _lastViewbox))
            {
                InvalidateVisual();
            }
        }

        private bool TryGetViewbox(BackdropGlass owner, out Rect viewbox)
        {
            viewbox = Rect.Empty;
            var source = owner.BackdropVisual;
            if (!owner.IsSafeSource(source) || RenderSize.Width <= 0 || RenderSize.Height <= 0)
            {
                return false;
            }

            try
            {
                viewbox = TransformToVisual(source).TransformBounds(new Rect(RenderSize));
                return !viewbox.IsEmpty && viewbox.Width > 0 && viewbox.Height > 0;
            }
            catch (InvalidOperationException)
            {
                return false;
            }
        }
    }

    private sealed class ChromeLayer : FrameworkElement
    {
        protected override void OnRender(DrawingContext drawingContext)
        {
            base.OnRender(drawingContext);
            var owner = (BackdropGlass)TemplatedParent;
            var bounds = new Rect(RenderSize);
            if (bounds.IsEmpty || bounds.Width <= 0 || bounds.Height <= 0)
            {
                return;
            }

            if (owner._backdropLayer?.HasSample != true)
            {
                drawingContext.DrawRectangle(owner.Background, null, bounds);
            }
            drawingContext.DrawRectangle(owner.SaturationTint, null, bounds);
            drawingContext.DrawRectangle(owner.TintBrush, null, bounds);

            var opticalThickness = Math.Min(Math.Max(5, owner.LensWidth * .82), Math.Min(bounds.Width, bounds.Height) * .28);
            var opticalEdge = new LinearGradientBrush
            {
                MappingMode = BrushMappingMode.RelativeToBoundingBox,
                StartPoint = new Point(0, 0),
                EndPoint = new Point(1, 1)
            };
            opticalEdge.GradientStops.Add(new GradientStop(Color.FromArgb(72, 255, 255, 255), 0));
            opticalEdge.GradientStops.Add(new GradientStop(Color.FromArgb(24, 205, 236, 255), .26));
            opticalEdge.GradientStops.Add(new GradientStop(Color.FromArgb(4, 255, 255, 255), .58));
            opticalEdge.GradientStops.Add(new GradientStop(Color.FromArgb(22, 255, 146, 116), .84));
            opticalEdge.GradientStops.Add(new GradientStop(Color.FromArgb(10, 9, 19, 31), 1));
            drawingContext.DrawGeometry(opticalEdge, null, RoundedRing(bounds, owner.CornerRadius, opticalThickness));

            var surfaceSheen = new LinearGradientBrush
            {
                MappingMode = BrushMappingMode.RelativeToBoundingBox,
                StartPoint = new Point(.15, 0),
                EndPoint = new Point(.68, .72)
            };
            surfaceSheen.GradientStops.Add(new GradientStop(Color.FromArgb(30, 255, 255, 255), 0));
            surfaceSheen.GradientStops.Add(new GradientStop(Color.FromArgb(8, 205, 234, 255), .34));
            surfaceSheen.GradientStops.Add(new GradientStop(Colors.Transparent, .7));
            drawingContext.DrawGeometry(surfaceSheen, null, RoundedRect(bounds, owner.CornerRadius));

            if (owner._pointerEnergy > 0 && owner.PointerGlowOpacity > 0)
            {
                var alpha = (byte)Math.Clamp(owner.PointerGlowOpacity * owner._pointerEnergy * 255, 0, 255);
                var glow = new RadialGradientBrush
                {
                    MappingMode = BrushMappingMode.RelativeToBoundingBox,
                    Center = owner._pointerPosition,
                    GradientOrigin = owner._pointerPosition,
                    RadiusX = .42,
                    RadiusY = .58
                };
                glow.GradientStops.Add(new GradientStop(Color.FromArgb(alpha, 255, 255, 255), 0));
                glow.GradientStops.Add(new GradientStop(Color.FromArgb((byte)(alpha * .42), 195, 228, 255), .38));
                glow.GradientStops.Add(new GradientStop(Colors.Transparent, 1));
                drawingContext.DrawRectangle(glow, null, bounds);
            }

            var thickness = owner.BorderThickness;
            var borderWidth = Math.Max(0, Math.Max(Math.Max(thickness.Left, thickness.Top), Math.Max(thickness.Right, thickness.Bottom)));
            if (owner.BorderBrush is not null && borderWidth > 0)
            {
                DrawStroke(drawingContext, bounds, owner.CornerRadius, owner.BorderBrush, borderWidth, borderWidth / 2);
            }
            if (owner.HighlightBrush is not null)
            {
                DrawStroke(drawingContext, bounds, owner.CornerRadius, owner.HighlightBrush, .8, borderWidth + 1.1);
            }

            if (owner._pointerEnergy > 0)
            {
                var edgeAlpha = (byte)Math.Clamp(180 * owner._pointerEnergy, 0, 180);
                var edge = new RadialGradientBrush
                {
                    MappingMode = BrushMappingMode.RelativeToBoundingBox,
                    Center = owner._pointerPosition,
                    GradientOrigin = owner._pointerPosition,
                    RadiusX = .5,
                    RadiusY = .7
                };
                edge.GradientStops.Add(new GradientStop(Color.FromArgb(edgeAlpha, 255, 255, 255), 0));
                edge.GradientStops.Add(new GradientStop(Color.FromArgb((byte)(edgeAlpha * .28), 112, 215, 255), .5));
                edge.GradientStops.Add(new GradientStop(Colors.Transparent, 1));
                DrawStroke(drawingContext, bounds, owner.CornerRadius, edge, 1.15, borderWidth + .25);
            }
        }
    }
}
