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
Texture2D Source1 : register(t1);
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
    float3 color = Source0.SampleLevel(LinearClamp, sourceUv, 0).rgb;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    return float4(lerp(luminance.xxx, color, 0.88), 1.0);
}

float4 Blur(float2 uv, float2 direction)
{
    float2 stepUv = TexelSize * direction * 2.2;
    float3 color = Source0.SampleLevel(LinearClamp, uv, 0).rgb * 0.227027;
    color += Source0.SampleLevel(LinearClamp, uv + stepUv * 1.384615, 0).rgb * 0.316216;
    color += Source0.SampleLevel(LinearClamp, uv - stepUv * 1.384615, 0).rgb * 0.316216;
    color += Source0.SampleLevel(LinearClamp, uv + stepUv * 3.230769, 0).rgb * 0.070270;
    color += Source0.SampleLevel(LinearClamp, uv - stepUv * 3.230769, 0).rgb * 0.070270;
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

float Noise(float2 pixel)
{
    return frac(sin(dot(pixel, float2(12.9898, 78.233))) * 43758.5453);
}

float4 GlassPS(VertexOutput input) : SV_Target
{
    float2 sharpUv = SourceOffset + input.uv * SourceScale;
    float3 blurred = Source0.SampleLevel(LinearClamp, input.uv, 0).rgb;
    float3 sharp = Source1.SampleLevel(LinearClamp, sharpUv, 0).rgb;
    float distanceToEdge = RoundedDistance(input.uv * OutputSize);
    float shape = 1.0 - smoothstep(-1.0, 1.0, distanceToEdge);
    float border = shape * (1.0 - smoothstep(0.0, BorderWidth + 1.0, -distanceToEdge));

    float3 color = lerp(blurred, sharp, 0.10);
    color = lerp(color, Tint.rgb, 0.13);
    color += (Noise(input.position.xy) - 0.5) / 255.0;
    color += border * 0.16;

    float alpha = shape * Tint.a;
    return float4(color * alpha, alpha);
}
