////////////////////////////////////////////////////////////////////////////////////////////////
//
//  モデル描画シェーダ
//  
//
////////////////////////////////////////////////////////////////////////////////////////////////

// パラメータ宣言
// 座標変換行列
cbuffer GeometryBuffer
{
    float4x4 S_WorldViewProjMatrix;
    float4x4 S_WorldMatrix;
    float4x4 S_ViewMatrix;
    float4x4 S_ProjMatrix;
    float4x4 S_ViewProjMatrix;
    float4x4 S_LightWorldViewProjMatrix;
}

// マテリアル色
cbuffer MaterialBuffer
{
	float4 S_MaterialDiffuse;
	float3 S_MaterialAmbient;
	float3 S_MaterialEmissive;
	float3 S_MaterialSpecular;
	float    S_SpecularPower;

	float4 S_MaterialToon;
	float4 S_EdgeColor;
	float	 S_EdgeWidth;
	float	 S_TessellationFactor;
	bool	 S_spadd;
}

cbuffer SceneBuffer
{
	float3 S_CameraPosition;

	float3 S_LightDirection;
	float3 S_LightDiffuse;
	float3 S_LightAmbient;
	float3 S_LightSpecular;

	float	 S_ShadowDeep;
	float	 S_ShadowDeepPlus;
	float	 S_ShadowDeepMinus;
}

bool usetoon : USETOON;

//////////////////////////////////////////////
// 頂点シェーダ系
//////////////////////////////////////////////
struct STREAM_VS_INPUT
{
	float4 Pos			: POSITION;
	float3 Normal			: NORMAL;
	float2 Tex			: TEXCOORD0;
	float4 AddUV1			: TEXCOORD1;
	float EdgeWeight 		: EDGEWEIGHT;
	float Index 			: INDEX;
};

struct VS_OUTPUT
{
	float4 Pos	: SV_POSITION;	// 射影変換座標
	float4 Position	: POSITION;	// 射影変換座標(位置保存用)
	float2 Tex	: TEXCOORD0;	// テクスチャ
	float4 SubTex	: TEXCOORD1;	// スフィアマップテクスチャ座標
	float3 Normal	: NORMAL;	// 法線
	float3 Eye	: EYEPOSITION;	// カメラとの相対位置
	float4 SSTex	: SELFSHADOW;	// セルフシャドウテクスチャ座標
	float4 Color	: COLOR0;	// ライトによる色
};

//////////////////////////////////////////////
// ピクセルシェーダ系
//////////////////////////////////////////////

// サンプラ
SamplerState S_DefaultSampler
{
    Filter = MIN_MAG_MIP_LINEAR;
    AddressU = Wrap;
    AddressV = Wrap;
};

// オブジェクトのテクスチャ
Texture2D ObjectTexture         : MATERIALTEXTURE;

// スフィアマップのテクスチャ
Texture2D ObjectSphereMap       : MATERIALSPHEREMAP;


//////////////////////////////////////////////
// Toon計算
//////////////////////////////////////////////
float3 GetToonColor(VS_OUTPUT S_PSInput, float3 color)
{
	
	if ((color.g + color.b) / color.r < 0.2f)	//赤系
		color = color * 0.5f;
	if (color.r + color.g + color.b > 2.5f)
		color.gb = color.gb * 0.8f;
	else if (color.b / (color.r + color.g) < 0.1f)	//黄色系
		color.gb = color.gb * 0.5f;
	else if ((color.r + color.g) / color.b < 0.2f)	//青系
		color.gb = color.gb * 0.5f;
	else if ((color.r + color.b) / color.g < 0.2f)	//緑系
		color = color * 0.5f;
	else
		color.rg = color.rg * 0.5f;

	return color;
}

//////////////////////////////////////////////
// シャドウ系
//////////////////////////////////////////////

// セルフシャドウマップのテクスチャ
Texture2D ObjectSelfShadowMap       : SELFSHADOWDEPTHMAP;
SamplerState ObjSelfShadowSampler
{
    Filter = MIN_MAG_MIP_LINEAR;
    AddressU = Wrap;
    AddressV = Wrap;
};

float3 GetShadowColor(float4 SSTex, float3 darkcolor)
{
    // テクスチャ座標に変換
    float2 TransTexCoord;
    TransTexCoord.x = (1.0f + SSTex.x) * 0.5f;
    TransTexCoord.y = (1.0f - SSTex.y) * 0.5f;

    if (any(saturate(TransTexCoord) != TransTexCoord))
    {
        // シャドウバッファ外
        return float3(1, 1, 1);
    }
    else
    {
        float smz, tm;
        float3 color = float3(1.0f, 1.0f, 1.0f);
        float w, h;
        ObjectSelfShadowMap.GetDimensions(w, h);
        w = 1.0 / w;

        smz = ObjectSelfShadowMap.Sample(ObjSelfShadowSampler, TransTexCoord);
        smz += ObjectSelfShadowMap.Sample(ObjSelfShadowSampler, TransTexCoord + float2(0, -w));
        smz += ObjectSelfShadowMap.Sample(ObjSelfShadowSampler, TransTexCoord + float2(-w, 0));
        smz += ObjectSelfShadowMap.Sample(ObjSelfShadowSampler, TransTexCoord + float2(w, 0));
        smz += ObjectSelfShadowMap.Sample(ObjSelfShadowSampler, TransTexCoord + float2(0, w));

        smz = smz / 5 + 0.0001;

        tm = SSTex.z;
        if (tm > smz)
        {
            tm = 1.0 - saturate(((tm - smz) * 100.0f)) * S_ShadowDeepMinus;
            color = darkcolor;//lerp(tm, 1.0, darkcolor * S_ShadowDeepPlus);
        }

        return color;
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////
// 輪郭描画
////////////////////////////////////////////////////////////////////////////////////////////////
float4 Edge_VS_NoTessellation(STREAM_VS_INPUT IN) : SV_POSITION
{
	//座標
	float4 position = IN.Pos;
	position = position + float4(IN.Normal, 0) * IN.EdgeWeight * S_EdgeWidth * 0.015f;// *distance(position.xyz, S_CameraPosition);

	// カメラとの相対位置
	float3 Eye = S_CameraPosition - position.xyz;
	return mul(position, S_WorldViewProjMatrix);
}
// ピクセルシェーダ
float4 Edge_PS() : SV_Target
{
	// 輪郭色で塗りつぶし
    return float4(S_EdgeColor.rgb, S_EdgeColor.a * S_MaterialDiffuse.a);
}

////////////////////////////////////////////////////////////////////////////////////////////////
// セルフシャドウ描画
////////////////////////////////////////////////////////////////////////////////////////////////
struct VS_SELFSHADOW_OUTPUT
{
	float4 Pos	: SV_POSITION;	// 射影変換座標
	float4 Shadow	: POSITION;	// 座標
	float2 Tex	: TEXCOORD0;	// テクスチャ座標
};

VS_SELFSHADOW_OUTPUT SelfShadow_VS(STREAM_VS_INPUT IN)
{
    VS_SELFSHADOW_OUTPUT Out = (VS_SELFSHADOW_OUTPUT)0;

    //座標
    Out.Pos = mul(IN.Pos, S_LightWorldViewProjMatrix);
    Out.Shadow = Out.Pos;
    Out.Tex = IN.Tex;

    return Out;
}

// ピクセルシェーダ
float4 SelfShadow_PS(VS_SELFSHADOW_OUTPUT IN) : SV_Target
{
	//テクスチャ色
	float4 tex = ObjectTexture.Sample(S_DefaultSampler, IN.Tex);
	if (tex.a< 1)
        	discard;

	// 深度を書き込む
	float d = IN.Shadow.z;
	return float4(d, 0, 0, 1.0f);
}

///////////////////////////////////////////////////////////////////////////////////////////////
// 頂点シェーダ
////////////////////////////////////////////////////////////////////////////////////////////////
VS_OUTPUT VS_NOTESSELLATION(STREAM_VS_INPUT IN, uniform bool S_useTexture, uniform bool S_useSphereMap, uniform bool S_useSelfShadow)
{

	VS_OUTPUT Out = (VS_OUTPUT)0;
	//位置を保存
	float4 Pos = float4(IN.Pos.xyz, 1.0);
	// カメラとの相対位置
	Out.Eye = S_CameraPosition - IN.Pos.xyz;

	Out.Pos = mul(Pos, S_WorldViewProjMatrix);
	Out.Normal = mul(IN.Normal, (float3x3)S_WorldMatrix);
	Out.Tex = IN.Tex;

	if (S_useSphereMap)
	{
		// スフィアマップテクスチャ座標
		float2 NormalWV = mul(Out.Normal, (float3x3)S_ViewMatrix);
		Out.SubTex.z = NormalWV.x * 0.5f + 0.5f;
		Out.SubTex.w = NormalWV.y * -0.5f + 0.5f;
	}
	//位置保存
	Out.Position = Out.Pos;

	// ディフューズ色＋アンビエント色 計算
	float4 DiffuseColor = S_MaterialDiffuse * float4(S_LightAmbient, 1.0f);
	float3 AmbientColor = saturate(S_MaterialEmissive * S_LightAmbient + S_MaterialAmbient);

	Out.Color = saturate(float4(AmbientColor, S_MaterialDiffuse.a));

	if (S_useSelfShadow)
	{
		// ライト視点によるワールドビュー射影変換
		Out.SSTex = mul(Pos, S_LightWorldViewProjMatrix);
	}

	return Out;

    return Out;
}

///////////////////////////////////////////////////////////////////////
// ピクセルシェーダ
///////////////////////////////////////////////////////////////////////
float4 PS(VS_OUTPUT S_PSInput, uniform bool S_useTexture, uniform bool S_useSphereMap, uniform bool S_useSelfShadow) : SV_Target
{
	float4 Color = S_PSInput.Color;
	float3 shadowcolor = float3(1, 1, 1);

	float4 spherecolor = 1.0;
	float4 texColor = float4(1, 1, 1, 1);
	float spalpha = 1.0;

	if (S_useTexture) {
		// テクスチャのみ適用
		texColor = ObjectTexture.Sample(S_DefaultSampler, S_PSInput.Tex);
	}
	Color *= texColor;

	if (S_useSphereMap) {
		// スフィアマップ適用
		spherecolor = ObjectSphereMap.Sample(S_DefaultSampler, S_PSInput.SubTex.zw);
		spalpha = spherecolor.a;

		if (S_spadd)
            		Color.rgb += spherecolor.rgb;
		else
			Color.rgb *= spherecolor.rgb;
		Color.a *= spalpha;
	}

	//影の色
	float3 darkcolor = GetToonColor(S_PSInput, Color.rgb);
	//陰影
	if (usetoon)
	{
		float LightNormal = dot(S_PSInput.Normal, -S_LightDirection);
		if (LightNormal < 0.5f)
			shadowcolor = darkcolor;
	}
	else
	{
		float LightNormal = dot(normalize(S_PSInput.Normal), -S_LightDirection);
		if (LightNormal < -0.6)
			shadowcolor = darkcolor;
	}

	//セルフシャドウ
	if (S_useSelfShadow)
		Color.rgb *= Color.rgb * (1.0 - Color.a) + min(shadowcolor, GetShadowColor(S_PSInput.SSTex, darkcolor)) * Color.a;
	else
		Color.rgb *= shadowcolor;

	clip(Color.a - 0.0000000000001);

	return Color;
}

///////////////////////////////////////////////////////////////////////
// オブジェクト描画用テクニック
///////////////////////////////////////////////////////////////////////
#define RENDER_TECHNIQUE_NOTESSELLATION(_name, _usetex, _usesphere, _useselfshadow) \
	technique10 _name < string Target = "object"; bool UseTexture = _usetex; bool UseSphereMap = _usesphere; bool UseSelfShadow = _useselfshadow; bool UseTessellation = false; > { \
		pass P0 \
    { \
			SetVertexShader(CompileShader(vs_5_0, VS_NOTESSELLATION(_usetex, _usesphere, _useselfshadow))); \
			SetHullShader(NULL); \
			SetDomainShader(NULL); \
			SetPixelShader(CompileShader(ps_5_0, PS(_usetex, _usesphere, _useselfshadow))); \
	} \
}

RENDER_TECHNIQUE_NOTESSELLATION(Render00, false, false, false)
RENDER_TECHNIQUE_NOTESSELLATION(Render01, true, false, false)
RENDER_TECHNIQUE_NOTESSELLATION(Render02, false, true, false)
RENDER_TECHNIQUE_NOTESSELLATION(Render03, true, true, false)
RENDER_TECHNIQUE_NOTESSELLATION(Render04, false, false, true)
RENDER_TECHNIQUE_NOTESSELLATION(Render05, true, false, true)
RENDER_TECHNIQUE_NOTESSELLATION(Render06, false, true, true)
RENDER_TECHNIQUE_NOTESSELLATION(Render07, true, true, true)


technique10 Render64 < string Target = "edge"; >
{
	//エッジ描画
	pass P0
	{
		SetVertexShader(CompileShader(vs_4_0, Edge_VS_NoTessellation()));
		SetHullShader(NULL);
		SetDomainShader(NULL);
		SetPixelShader(CompileShader(ps_4_0, Edge_PS()));
	}
}

technique10 Render67 < string Target = "zplot"; >
{
    //Z値描画
    pass P0
    {
        SetVertexShader(CompileShader(vs_4_0, SelfShadow_VS()));
        SetHullShader(NULL);
        SetDomainShader(NULL);
        SetPixelShader(CompileShader(ps_4_0, SelfShadow_PS()));
    }
}



