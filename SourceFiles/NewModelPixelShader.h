#pragma once

struct NewModelPixelShader
{
    static constexpr char shader_ps[] = R"(
sampler ss : register(s0);
SamplerComparisonState shadowSampler : register(s1);

Texture2D terrain_shadow_map_props : register(t0);
Texture2D shaderTextures[8] : register(t3);

#define DARKGREEN float3(0.4, 1.0, 0.4)
#define LIGHTGREEN float3(0.0, 0.5, 0.0)

struct DirectionalLight
{
	float4 ambient;
	float4 diffuse;
	float4 specular;
	float3 direction;
	float pad;
};

cbuffer PerFrameCB : register(b0)
{
    DirectionalLight directionalLight;
    float time_elapsed;
    float3 fog_color_rgb;
    float fog_start;
    float fog_end;
    float fog_start_y; // The height at which fog starts.
    float fog_end_y; // The height at which fog ends.
    uint should_render_flags; // Shadows, Water reflection, fog (shadows at bit 0, water reflection at bit 1, fog at bit 2)
    // The user's environment light gain for the map. Only ever non-1.0 inside the world pass.
    float map_light_gain;
};

cbuffer PerObjectCB : register(b1)
{
    matrix World;
    uint4 uv_indices[2];
    uint4 texture_indices[2];
    uint4 blend_flags[2];
    uint4 texture_types[2];
    uint num_uv_texture_pairs;
    uint object_id;
    uint highlight_state; // 0 is not hightlight, 1 is dark green, 2 is lightgreen
    float pad1[1];
};

cbuffer PerCameraCB : register(b2)
{
    matrix View;
    matrix Projection;
    matrix directional_light_view;
    matrix directional_light_proj;
    matrix reflection_view;
    matrix reflection_proj;
    float3 cam_position;
    float2 shadowmap_texel_size;
    float2 reflection_texel_size;
};

cbuffer PerTerrainCB : register(b3)
{
	int grid_dim_x;
	int grid_dim_y;
	float min_x;
	float max_x;
	float min_y;
	float max_y;
	float water_level;
	float pad[3];
};

struct PixelInputType
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 lightingColor : COLOR0;
    float2 tex_coords0 : TEXCOORD0;
    float2 tex_coords1 : TEXCOORD1;
    float2 tex_coords2 : TEXCOORD2;
    float2 tex_coords3 : TEXCOORD3;
    float2 tex_coords4 : TEXCOORD4;
    float2 tex_coords5 : TEXCOORD5;
    float4 reflectionSpacePos : TEXCOORD6;
    float4 lightSpacePos : TEXCOORD7;
    float3 world_position : TEXCOORD8;
    float3x3 TBN : TEXCOORD9;
};

struct PSOutput
{
	float4 rt_0_output : SV_TARGET0; // Goes to first render target (usually the screen)
	float4 rt_1_output : SV_TARGET1; // Goes to second render target
};

float3 compute_normalmap_lighting(const float3 normalmap_sample, const float3x3 TBN, const float3 pos)
{
    // Transform the normal from tangent space to world space
    const float3 normal = normalize(mul(normalmap_sample, TBN));

    // Compute lighting with the Phong reflection model for a directional light
    const float3 light_dir = -directionalLight.direction; // The light direction points towards the light source
    const float3 view_dir = normalize(cam_position - pos);
    const float3 reflect_dir = reflect(-light_dir, normal);

    const float shininess = 2;

    const float diff = max(dot(normal, light_dir), 0.0);
    const float spec = pow(max(dot(view_dir, reflect_dir), 0.0), shininess);

    const float3 diffuse = diff * directionalLight.ambient.rbg;
    const float3 specular = spec * directionalLight.specular.rgb;

    return float3(1, 1, 1) + diffuse + specular;
};

)" R"(
// NPC TEAM GLOW - the coloured rim the client puts on a team's NPCs in PvP (guild lord, guards,
// pets, spirits, minions). Gw.exe draws it as one extra layer appended to every material of the
// NPC's model (model effect 10, "glow"), and that layer is all there is to it:
//
//     u      = 0.5 + 0.5 * n_view.x              the view-space normal, sideways component only
//     colour = lit_colour + ramp(u) * team_rgb   added after lighting, unlit, alpha untouched
//
// The ramp is Gw.dat file 206253 (256x4 DXT1, four identical rows), baked below texel for texel.
// It is zero wherever |n.x| < 0.5, so a surface facing the camera is left alone, peaks at about
// 0.91 near |n.x| = 0.8 and falls back to 0.2-0.3 at the very silhouette - the band of colour
// down the sides of the arms and legs. There is no time term: the glow does not pulse.
//
// The team id travels in bits 8-11 of highlight_state (0 = no glow), which keeps the low byte
// for the hover and pick modes. The colours are the client's own table, ids 1..7; the client
// clamps anything above 7 to 7.
static const uint kTeamGlowRamp[256] =
{
    0x303430, 0x383A38, 0x404140, 0x484848, 0x484C48, 0x555655, 0x626162, 0x706C70,
    0x707470, 0x7D807D, 0x8A8C8A, 0x989898, 0xA0A0A0, 0xAAACAA, 0xB5B8B5, 0xC0C4C0,
    0xC0C8C0, 0xCAD1CA, 0xD5DAD5, 0xE0E4E0, 0xE0E4E0, 0xE5E9E5, 0xE8ECE8, 0xE8ECE8,
    0xE8ECE8, 0xE2E8E2, 0xDDE4DD, 0xD8E0D8, 0xD8DCD8, 0xCDD2CD, 0xC2C9C2, 0xB8C0B8,
    0xB8B8B8, 0xAAACAA, 0x9DA09D, 0x909490, 0x909090, 0x828282, 0x757575, 0x686868,
    0x606460, 0x555955, 0x4A4E4A, 0x404440, 0x403C40, 0x353435, 0x2A2C2A, 0x2A2C2A,
    0x201D20, 0x201D20, 0x181618, 0x181618, 0x121012, 0x0D0C0D, 0x0D0C0D, 0x080808,
    0x080808, 0x050505, 0x050505, 0x020202, 0x020102, 0x020102, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x020102, 0x020102, 0x050205,
    0x050505, 0x050505, 0x050505, 0x080808, 0x080808, 0x0D0C0D, 0x121012, 0x121012,
    0x181A18, 0x181A18, 0x202120, 0x282828, 0x282828, 0x323132, 0x3D3A3D, 0x484448,
    0x484848, 0x525252, 0x5D5D5D, 0x686868, 0x687068, 0x787C78, 0x888888, 0x989498,
    0x989C98, 0xA5A8A5, 0xB2B4B2, 0xC0C0C0, 0xC0C4C0, 0xCACDCA, 0xD5D6D5, 0xE0E0E0,
    0xE0E5E0, 0xE8EAE8, 0xE8EAE8, 0xE8EAE8, 0xE8ECE8, 0xE8ECE8, 0xE2E8E2, 0xDDE4DD,
    0xD5D6D5, 0xD5D6D5, 0xCACDCA, 0xC0C4C0, 0xC0BCC0, 0xB2B1B2, 0xA5A6A5, 0x989C98,
    0x909490, 0x858882, 0x7A7C75, 0x707068, 0x686868, 0x606060, 0x585858, 0x505050
};

static const float3 kTeamGlowColors[8] =
{
    float3(0, 0, 0),
    float3(0, 0, 1),        // 1 blue
    float3(1, 0, 0),        // 2 red
    float3(1, 1, 0),        // 3 yellow
    float3(0, 1, 1),        // 4 cyan
    float3(1, 0, 1),        // 5 magenta
    float3(0, 1, 0),        // 6 green
    float3(0.376, 0.376, 0.376) // 7 grey (96, 96, 96)
};

float3 TeamGlowRampTexel(uint i)
{
    uint p = kTeamGlowRamp[i];
    return float3((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF) / 255.0;
}

// Linear filtering across the 256 texels, clamped at both ends.
float3 TeamGlow(float3 world_normal, uint team_glow_id)
{
    float nx = normalize(mul(world_normal, (float3x3)View)).x;
    float x = saturate(0.5 + 0.5 * nx) * 256.0 - 0.5;
    uint i0 = (uint)clamp(floor(x), 0.0, 255.0);
    uint i1 = min(i0 + 1, 255);
    float f = saturate(x - (float)i0);
    float3 ramp = lerp(TeamGlowRampTexel(i0), TeamGlowRampTexel(i1), f);
    return ramp * kTeamGlowColors[min(team_glow_id, 7)];
}

PSOutput main(PixelInputType input)
{
    uint highlight_mode = highlight_state & 0xFF;
    float4 sampled_texture_color = float4(1, 1, 1, 1);
    float2 tex_coords_array[6] =
    {
        input.tex_coords0, input.tex_coords1, input.tex_coords2, input.tex_coords3,
                                 input.tex_coords4, input.tex_coords5
    };

    float3 lighting_color = float3(1, 1, 1);

    bool first_non_normalmap_texture_applied = false;
    for (int i = 0; i < num_uv_texture_pairs; ++i)
    {
        uint uv_set_index = uv_indices[i / 4][i % 4];
        uint texture_index = texture_indices[i / 4][i % 4];
        uint blend_flag = blend_flags[i / 4][i % 4];
        uint texture_type = texture_types[i / 4][i % 4] & 0xFF;

        for (int t = 0; t < 6; ++t)
        {
            if (t == texture_index)
            {
                if (texture_type == 2)
                {
					// Compute normal map lighting
                    const float3 normal_from_map = shaderTextures[t].Sample(ss, tex_coords_array[uv_set_index]).rgb * 2.0 - 1.0;
                    float3 pos = input.position.xyz;
                    lighting_color = compute_normalmap_lighting(normal_from_map, input.TBN, pos);
                }
                else
                {
                    if (!first_non_normalmap_texture_applied)
                    {
                        float4 current_sampled_texture_color = shaderTextures[t].Sample(ss, tex_coords_array[uv_set_index]);
                        if (blend_flag == 0)
                        {
                            current_sampled_texture_color.a = 1;
                        }

                        sampled_texture_color = saturate(sampled_texture_color * current_sampled_texture_color);
                        first_non_normalmap_texture_applied = true;
                        break;
                    }
                }
            }
        }
    }

    if (sampled_texture_color.a <= 0.0f)
    {
        discard;
    }

    // This program never reads the vertex lighting - a new-format model is drawn at its own texel
    // brightness unless it carries a normal map - so the map light gain is applied here instead of
    // in the vertex shader. It is 1.0 outside the world pass, so a skinned draw is untouched.
    //
    // THE GAIN IS INSIDE THE PER-CHANNEL CLAMP, as it is on the terrain's two endpoints. There is
    // no light term of its own to scale here, so the brighter light is expressed on the lit texel
    // and clamped at 1 before anything else reads it: a channel that reaches full stays at full
    // while the others climb towards it, instead of an above-1 value surviving into the normal
    // map's highlight and the haze blend. At 1.00x the clamp is inert, because a sampled texel is
    // already saturated, so the pixel is unchanged.
    float3 final_color = lighting_color * min(1.0, sampled_texture_color.rgb * map_light_gain);
    
    if (highlight_mode == 1)
    {
        final_color.rgb = lerp(final_color.rgb, DARKGREEN, 0.7);
    }
    else if (highlight_mode == 2)
    {
        final_color.rgb = lerp(final_color.rgb, LIGHTGREEN, 0.4);
    }
    else if (highlight_mode == 5)
    {
        final_color.rgb = saturate(final_color.rgb * 1.15);
    }
    
    bool should_render_model_shadows = should_render_flags & 8;
    if (should_render_model_shadows)
    {
        float3 ndcPos = input.lightSpacePos.xyz / input.lightSpacePos.w;

        // Transform position to shadow map texture space
        float2 shadowTexCoord = float2(ndcPos.x * 0.5 + 0.5, -ndcPos.y * 0.5 + 0.5);
        float shadowDepth = input.lightSpacePos.z / input.lightSpacePos.w;

        // Add a bias to reduce shadow acne, especially on steep surfaces
        float bias = max(0.001 * (1.0 - dot(normalize(input.normal), -normalize(directionalLight.direction))), 0.0005);
        shadowDepth += bias;

        // PCF
        float shadow = 0.0;
        int pcf_samples = 9;
        float2 shadowmap_texelSize = shadowmap_texel_size;
        for (int x = -1; x <= 1; x++)
        {
            for (int y = -1; y <= 1; y++)
            {
                float2 samplePos = shadowTexCoord + float2(x, y) * shadowmap_texelSize;
                shadow += terrain_shadow_map_props.SampleCmpLevelZero(shadowSampler, samplePos, shadowDepth);
            }
        }

        // Normalize the shadow value
        shadow /= pcf_samples;

        // Apply shadow to final color
        final_color.rgb *= lerp(0.65, 1.0, shadow);
    }

    uint team_glow_id = (highlight_state >> 8) & 0xF;
    if (team_glow_id != 0)
    {
        // After lighting and the shadow (the glow is not lit), before the haze.
        final_color.rgb = saturate(final_color.rgb + TeamGlow(input.normal, team_glow_id));
    }

    bool should_render_fog = should_render_flags & 4;
    if (should_render_fog)
    {
        float distance = length(cam_position - input.world_position.xyz);

        // THE CLIENT'S OWN HAZE CURVE, read out of Gw.exe instruction by instruction - see
        // the map lighting model note under docs/appearance_data for the addresses.
        //
        //     fDist   = (fog_end - d) / (fog_end - fog_start)          linear, no curve
        //     fHeight = (y - fog_end_y) / (fog_start_y - fog_end_y)
        //     fNear   = 1 - d / (2 * (fog_start_y - fog_end_y))
        //     f       = clamp( max( min(fDist, fHeight), fNear ), 0, 1 )
        //     colour  = lerp(fogColour, colour, f)          f = 1 is clear, f = 0 is all haze
        //
        // TWO CHANGES FROM WHAT THIS USED TO DO, and they pull in opposite directions.
        //
        // GONE: `clamp(f, 0.20, 1)`. There is no such floor in the client. It capped the haze at
        // 80% however far away a pixel was, so the far distance could never close up - which is
        // exactly the "our image is too clear, the client's distance is washed out" report.
        //
        // NEW: `fNear`, which the client has and this did not. It is a distance-dependent FLOOR
        // that starts at 1 (fully clear) at the camera and reaches 0 at twice the height band's
        // depth, and its job is to stop the HEIGHT term from hazing geometry right in front of the
        // camera. Without it, removing the 0.20 floor alone would put a permanent wash over the
        // near field. The client only applies it when the height term is live; when the height
        // band is degenerate it runs distance-only, which is what `fogNear = 0` reproduces here.
        //
        // Known, deliberate difference: the client uses the VIEW-SPACE DEPTH of the pixel and this
        // uses the radial distance to the camera. They agree on the view axis and this one is
        // slightly larger towards the edges of a wide screen.
        float fogFactorDistance = 1.0;
        float fogDistanceDenom = fog_end - fog_start;
        if (abs(fogDistanceDenom) >= 1e-3)
        {
            fogFactorDistance = (fog_end - distance) / fogDistanceDenom;
        }

        float fogFactorHeight = 1.0;
        float fogNear = 0.0;
        float fogHeightDenom = fog_start_y - fog_end_y;
        // The client's own gate on the height term, and it is a STRICT `>`, not a magnitude test:
        // GrDeviceSetHazeHeight's Dx9 handler compares its two arguments (-fog_z_start, -fog_z_end)
        // and only arms the height half when the second is the greater, i.e. when
        // fog_z_start > fog_z_end. `abs(denom) >= 1e-3` also admitted a NEGATIVE denominator, and
        // that does not merely flip the band: it turns fNear into `1 + d / (2*|denom|)`, which is
        // at least 1 at every distance, so the final max() forces f = 1 and the map loses its haze
        // entirely. The client runs distance-only there instead, which is what falling through to
        // fogFactorHeight = 1 / fogNear = 0 reproduces. No guild hall has an inverted band, so this
        // changes nothing on the sixteen; it stops the failure being silent on anything else.
        if (fogHeightDenom > 0.0)
        {
            fogFactorHeight = (input.world_position.y - fog_end_y) / fogHeightDenom;
            fogNear = 1.0 - distance / (2.0 * fogHeightDenom);
        }

        float fogFactor = saturate(max(min(fogFactorDistance, fogFactorHeight), fogNear));

        float3 fogColor = fog_color_rgb; // Fog color defined in the constant buffer
        final_color = lerp(fogColor, final_color, fogFactor);
    }
    
    PSOutput output;
    output.rt_0_output = float4(final_color, sampled_texture_color.a);

	float4 color_id = float4(0, 0, 0, 1);
	color_id.r = (float) ((object_id & 0x00FF0000) >> 16) / 255.0f;
	color_id.g = (float) ((object_id & 0x0000FF00) >> 8) / 255.0f;
	color_id.b = (float) ((object_id & 0x000000FF)) / 255.0f;

    // Render target for picking
	output.rt_1_output = color_id;

	return output;
}
)";
};

