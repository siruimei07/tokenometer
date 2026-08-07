cbuffer ShaderParams : register(b0)
{
    float2 SourceScale;
    float2 SourceOffset;
    float2 TexelSize;
    float2 Padding;
    float4 Tint;
    float2 OutputSize;
    float CornerRadius;
    float BorderWidth;
};

Texture2D Source0 : register(t0);
SamplerState LinearClamp : register(s0);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PrefilterPS(VertexOutput input) : SV_Target
{
    float2 sourceUv = SourceOffset + input.uv * SourceScale;
    float2 sampleOffset = SourceScale * TexelSize * 0.375;
    float3 color =
        Source0.SampleLevel(LinearClamp, sourceUv + sampleOffset * float2(-1.0, -1.0), 0).rgb +
        Source0.SampleLevel(LinearClamp, sourceUv + sampleOffset * float2(1.0, -1.0), 0).rgb +
        Source0.SampleLevel(LinearClamp, sourceUv + sampleOffset * float2(-1.0, 1.0), 0).rgb +
        Source0.SampleLevel(LinearClamp, sourceUv + sampleOffset * float2(1.0, 1.0), 0).rgb;
    color *= 0.25;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    return float4(lerp(luminance.xxx, color, 0.08), 1.0);
}

float4 Blur(float2 uv, float2 direction)
{
    float2 stepUv = TexelSize * direction * 1.75;
    float3 color = Source0.SampleLevel(LinearClamp, uv, 0).rgb * 0.19648255;
    color += Source0.SampleLevel(LinearClamp, uv + stepUv * 1.41176471, 0).rgb * 0.29690696;
    color += Source0.SampleLevel(LinearClamp, uv - stepUv * 1.41176471, 0).rgb * 0.29690696;
    color += Source0.SampleLevel(LinearClamp, uv + stepUv * 3.29411765, 0).rgb * 0.09447040;
    color += Source0.SampleLevel(LinearClamp, uv - stepUv * 3.29411765, 0).rgb * 0.09447040;
    color += Source0.SampleLevel(LinearClamp, uv + stepUv * 5.17647059, 0).rgb * 0.01038136;
    color += Source0.SampleLevel(LinearClamp, uv - stepUv * 5.17647059, 0).rgb * 0.01038136;
    return float4(color, 1.0);
}

float4 BlurHPS(VertexOutput input) : SV_Target
{
    return Blur(input.uv, float2(1.0, 0.0));
}

float4 BlurVPS(VertexOutput input) : SV_Target
{
    return Blur(input.uv, float2(0.0, 1.0));
}

float RoundedDistance(float2 pixel)
{
    float radius = min(CornerRadius, min(OutputSize.x, OutputSize.y) * 0.5);
    float2 halfSize = OutputSize * 0.5;
    float2 q = abs(pixel - halfSize) - (halfSize - radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float PixelNoise(float2 pixel)
{
    return frac(sin(dot(pixel, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
}

float4 GlassPS(VertexOutput input) : SV_Target
{
    float distanceToEdge = RoundedDistance(input.uv * OutputSize);
    float inside = max(-distanceToEdge, 0.0);
    float shape = 1.0 - smoothstep(-1.0, 1.0, distanceToEdge);
    float edgeWidth = max(OutputSize.y * 0.10, 1.0);
    float edge = exp2(-4.0 * inside / edgeWidth);
    float2 gradient = float2(ddx(distanceToEdge), ddy(distanceToEdge));
    float2 outward = gradient * rsqrt(max(dot(gradient, gradient), 1e-6));
    float warp = edge * OutputSize.y * 0.055;
    float2 glassUv = saturate(input.uv - outward * warp / OutputSize);
    float3 blurred = Source0.SampleLevel(LinearClamp, glassUv, 0).rgb;

    float3 color = blurred * float3(0.325, 0.328, 0.325) + Tint.rgb;
    float2 local = input.uv * 2.0 - 1.0;
    float directional = 0.5 + 0.5 * sin(atan2(local.y, local.x) - 0.5);
    color *= 1.0 + (directional - 0.5) * 0.12 * edge;
    color += PixelNoise(input.position.xy) / 255.0;
    float border = shape * (1.0 - smoothstep(0.0, BorderWidth, inside));
    color += border * float3(0.120, 0.122, 0.112) * (0.7 + 0.3 * directional);
    color = min(color, float3(0.46, 0.47, 0.46));

    return float4(color * shape, shape);
}
