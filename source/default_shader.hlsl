//IA
struct VertexPosColor
{
    float4 Position : POSITION;
    float4 Color : COLOR;
};

struct VertexShaderOutput
{
    float4 Color : COLOR;
    float4 Position : SV_Position;
};

//Bindings
struct vs_constants
{
	matrix model_to_projection;
};
ConstantBuffer<vs_constants> vs_cb : register(b0);

VertexShaderOutput VS(VertexPosColor IN)
{
    VertexShaderOutput OUT;

    /*OUT.Position = float4(IN.Position, 1.0f);*/
    /*OUT.Color = float4(IN.Color, 1.0f);*/

    OUT.Position = IN.Position;
    OUT.Color = IN.Color;
    return OUT;
}

struct PixelShaderInput
{
    float4 Color : COLOR;
};

float4 PS(PixelShaderInput IN) : SV_Target
{
    return IN.Color;
}
