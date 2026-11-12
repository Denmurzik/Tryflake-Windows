// Один проход: снежинки, сугробы, куски, снежки и детали снеговика.
// Цвет на выходе — premultiplied alpha (DXGI_ALPHA_MODE_PREMULTIPLIED).

cbuffer Viewport : register(b0) {
    float2 viewport;  // логический размер сцены (DIP)
    float2 padding;
};

struct VSIn {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float2 style : TEXCOORD1;
};

struct PSIn {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation float2 style : TEXCOORD1;
};

PSIn vsMain(VSIn v) {
    PSIn o;
    o.position = float4(v.position.x / viewport.x * 2 - 1, 1 - v.position.y / viewport.y * 2, 0, 1);
    o.uv = v.uv;
    o.color = v.color;
    o.style = v.style;
    return o;
}

float4 psMain(PSIn i) : SV_Target {
    float alpha = i.color.a;
    float3 color = i.color.rgb;
    float r = length(i.uv);
    float edge = max(fwidth(r), 0.008);
    if (i.style.x < 0.5) {
        // мягкая снежинка / пылинка
        alpha *= 1 - smoothstep(0.35, 1.0, r);
    } else if (i.style.x < 1.5) {
        // объёмный диск: снежок, угольки
        alpha *= 1 - smoothstep(1 - edge, 1.0, r);
        float light = clamp(0.91 - i.uv.y * 0.07 - i.uv.x * 0.035, 0.78, 1.0);
        color *= light;
    } else if (i.style.x > 2.5) {
        // крупная шестилучевая снежинка
        float angle = atan2(i.uv.y, i.uv.x) + i.style.y;
        float arms = pow(abs(cos(angle * 3)), 14.0);
        float shape = max(1 - smoothstep(0.07, 0.27, r), arms * (1 - smoothstep(0.2, 0.95, r)));
        alpha *= shape;
    }
    return float4(color * alpha, alpha);
}
