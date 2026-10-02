Texture2DArray atlas : register(t0);
Texture2D terrain_shadow_map_props : register(t3);
SamplerState samLinear : register(s0);
SamplerComparisonState shadowSampler : register(s1);

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
    float fog_start_y;
    float fog_end_y;
    uint should_render_flags;   // bit 5 = Classic light mode; whole word in PerFrameCB.h
    // The user's environment light gain for the map. 1.0 unless the owner moves it, and only ever
    // non-1.0 while the world pass is running.
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
    uint highlight_state;
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

struct PixelInputType
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 lightingColor : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
    float2 uv4 : TEXCOORD4;
    float2 uv5 : TEXCOORD5;
    float4 reflectionSpacePos : TEXCOORD6;
    float4 lightSpacePos : TEXCOORD7;
    float3 world_position : TEXCOORD8;
    float3x3 TBN : TEXCOORD9;
};

struct PSOutput
{
    float4 rt_0_output : SV_TARGET0;
    float4 rt_1_output : SV_TARGET1;
};

float4 SampleAtlas(float2 uv, float2 dx, float2 dy)
{
    // Decode Atlas UV to Layer + Local UV
    // Atlas Size: 2048, Tile Size: 256
    // Grid: 8x8
    
    float2 pixelPos = uv * 2048.0f;
    uint2 slotCoords = uint2(pixelPos) / 256;
    uint slotIndex = slotCoords.y * 8 + slotCoords.x;
    
    // Neutral texture is Slot 0 (Layer -1, so effectively transparent)
    if (slotIndex == 0)
    {
        return float4(0, 0, 0, 0);
    }
    
    uint layerIndex = slotIndex - 1;
    
    // Local UV within the 256x256 tile
    // Using frac ensures we handle the border offset correctly relative to the tile
    float2 localUV = frac(pixelPos / 256.0f);
    
    return atlas.SampleGrad(samLinear, float3(localUV, layerIndex), dx, dy);
}

PSOutput main(PixelInputType input)
{
    // Calculate derivatives for mipmapping (scaled for 256x256 tiles vs 2048x2048 atlas)
    // We calculate per-UV derivatives to handle rotation/scaling correctly
    float2 dx0 = ddx(input.uv0) * 8.0f;
    float2 dy0 = ddy(input.uv0) * 8.0f;
    float2 dx1 = ddx(input.uv1) * 8.0f;
    float2 dy1 = ddy(input.uv1) * 8.0f;
    float2 dx2 = ddx(input.uv2) * 8.0f;
    float2 dy2 = ddy(input.uv2) * 8.0f;

    // Sample all texture layers
    float4 t0 = SampleAtlas(input.uv0, dx0, dy0);
    float4 t1 = SampleAtlas(input.uv1, dx1, dy1);
    float4 t2 = SampleAtlas(input.uv2, dx2, dy2);

    // Progressive alpha blending
    float4 result = t0;
    result = lerp(result, t1, t1.a);
    result = lerp(result, t2, t2.a);
    
    // ---- THE CLIENT'S OWN TERRAIN LIGHTING ---------------------------------------------------
    //
    // Terrain does NOT take the light the way a model does, and this used to. The client's terrain
    // pixel shader is a raw token stream in the executable, and decoded it is four texture layers
    // blended by their own alphas and then ONE lerp between two endpoint colours:
    //
    //     colour = albedo * lerp(lightAmbient, lightDiffusePlusAmbient, lightMap)
    //
    // with, computed once per frame on the CPU:
    //
    //     lightAmbient            = ambient_bytes / 255 * (ambient_intensity / 256)   -- unclamped
    //     lightDiffusePlusAmbient = min(1, lightAmbient + sun_bytes / 255 * (cos(a) * sun_I))
    //
    // where `a` is the map's single sun angle, an elevation measured from straight up. Note what
    // is NOT in it: no per-vertex N.L on the sun term at all - the sun contributes a flat amount
    // scaled only by `cos(a)` - and no specular. The shading variation comes entirely from the
    // third factor, a baked light map, and THAT is where the surface normal enters.
    //
    // THE BAKE, and it is the part that matters most. The client bakes, per terrain vertex:
    //
    //     m    = saturate(dot(N, L))
    //     byte = round(255 * (1 - (1 - m)^4))
    //
    // a quartic soft shoulder, not a linear N.L. At `m = 0.5` it gives 0.94, not 0.5; at 0.25 it
    // gives 0.68. Only fully back-facing ground agrees with the linear form. Using `N.L` directly -
    // which is what `input.lightingColor` carries - makes every slope far darker than the client's,
    // and that was the bulk of "the terrain is too dark".
    //
    // Two deliberate differences, both recorded in the note under docs/appearance_data:
    //   * the client bakes this per VERTEX on a 96-unit grid from an area-blended four-triangle
    //     normal and bilinearly interpolates it into a 256x256 light map per chunk. This evaluates
    //     the same curve per PIXEL from the interpolated normal, which is smoother than the
    //     client's but has the same shape and the same endpoints.
    //   * the client also multiplies in an authored 272x272 one-bit shadow mask per chunk, read
    //     out of the map file. We do not load it, so baked terrain shadow is missing; the runtime
    //     shadow map below stands in for it.
    float3 trn_normal = normalize(input.normal);
    float3 trn_light_dir = normalize(-directionalLight.direction);
    float trn_ndotl = saturate(dot(trn_normal, trn_light_dir));
    float trn_one_minus = 1.0 - trn_ndotl;
    float trn_sq = trn_one_minus * trn_one_minus;
    float trn_bake = 1.0 - trn_sq * trn_sq;

    // THE SUN ELEVATION IS NOT APPLIED A SECOND TIME HERE, and this is the one step in the
    // terrain law that is a conclusion rather than a reading.
    //
    // The client's `TrnTexSetLight` computes its sun endpoint as
    // `min(1, ambient + sun * (cos(a) * sun_intensity))` - the elevation IS in that function, and
    // that was read correctly. What could NOT be established is that this function's output is
    // what the terrain's own shader receives: the terrain pass has no vertex shader and its shader
    // binds no named constants, so the two endpoints arrive as vertex colours filled by code that
    // was never found.
    //
    // Two things say the elevation is not in them:
    //   * it would be applied TWICE. The baked light map already contains `dot(N, L)`, and for
    //     flat ground that dot IS cos(a). Multiplying the sun by cos(a) as well attenuates flat
    //     ground by cos(a) squared - the geometric term applied twice, which is not what any
    //     renderer does.
    //   * it is refuted by measurement. On the owner's Corrupted Isle reference frame the lit
    //     basin reads (0.376, 0.243, 0.166). With cos(a) in the sun term the brightest this law
    //     can produce there is 0.172 even at an albedo of 1, so the texture would have to be 2.19
    //     - impossible. Without it the required albedo is (0.95, 0.75, 0.52), an ordinary warm
    //     sandy brown, and the hue comes out warm instead of grey, which is what the frame shows.
    //
    // So the elevation is left to the bake, where it enters through N.L. `directionalLight.pad`
    // carries cos(a) and is deliberately unused here; it is there for whoever closes the link.
    //
    // THE CLIENT APPLIES NO GAIN HERE, and that was established rather than assumed. Its terrain
    // pixel program is an authored ps_1_1 in the executable's own data - `albedo * lerp(v0, v1,
    // lightMap.a)` - and every destination token in it carries a zero shift and no saturate; the
    // one terrain program in the image that does carry a x2 shift has no reference anywhere and
    // cannot be reached; and the per-map 2x switch that doubles PROP layers is read only by the
    // model module, never by the terrain module. So the gain below is the user's, not the game's,
    // and 1.00x is the level the game itself draws.
    //
    // THE GAIN GOES ON THE LIGHT'S INPUTS, BEFORE THE PER-CHANNEL CLAMP, and that is the whole
    // point of it: both endpoints stay colours clamped to 1, the way the client's own pair of
    // interpolators is, so what the control changes is the LIGHT and not the picture. Applied
    // after the clamp instead - which is what this did first - a cold entry with ambient
    // (0.098, 0.170, 0.435) and sun (0.084, 0.097, 0.115) runs its blue to 2.20 at 4x while red
    // stays at 0.73, and the whole map goes saturated royal blue. Clamped per channel, blue stops
    // at 1.00 and red and green keep climbing towards it: brighter, and far less saturated, which
    // is what the game's own cold side looks like. One more thing comes free with it - the terrain
    // can no longer hand a value above 1 to the haze blend further down.
    //
    // At 1.00x this is byte for byte what it was: the ambient a map can express is
    // `bytes/255 * intensity/256` and cannot reach 1, so its clamp is inert there.
    float3 trn_ambient = min(1.0, directionalLight.ambient.rgb * map_light_gain);
    float3 trn_lit     = min(1.0, (directionalLight.ambient.rgb + directionalLight.diffuse.rgb)
                                      * map_light_gain);

    float3 trn_light = lerp(trn_ambient, trn_lit, trn_bake);

    // ---- WHICH LAW THIS PIXEL USES: flag bit 5, SET = CLASSIC --------------------------------
    // Classic is the last commit's formula unchanged - `result * 1.4 * input.lightingColor`, the
    // vertex program's `ambient + sun*N.L + specular`. Not the client's law, but the picture the
    // owner approved; the host holds the gain at 1.0 there. Clear = the client law above, kept
    // whole. A constant branch, so the compiler drops the unused half. Word: PerFrameCB.h.
    bool classic_map_light = (should_render_flags & 32) != 0;

    float4 color = result * 1.4 * input.lightingColor;
    if (!classic_map_light)
    {
        color = float4(result.rgb * trn_light, 1.0);
    }
    color.a = 1.0f;

    bool should_render_shadow = should_render_flags & 1;
    if (should_render_shadow)
    {
        // ============ SHADOW MAP START =====================
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
        float2 texelSize = shadowmap_texel_size;
        for (int x = -1; x <= 1; x++)
        {
            for (int y = -1; y <= 1; y++)
            {
                float2 samplePos = shadowTexCoord + float2(x, y) * texelSize;
                shadow += terrain_shadow_map_props.SampleCmpLevelZero(shadowSampler, samplePos, shadowDepth);
            }
        }

        // Normalize the shadow value
        shadow /= pcf_samples;

        // Apply shadow to final color
        color.rgb *= lerp(0.65, 1.0, shadow);
        // ============ SHADOW MAP END =====================
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
        color = lerp(float4(fogColor, 1.0), color, fogFactor);
    }

    PSOutput output;
    output.rt_0_output = color;

    float4 colorId = float4(0, 0, 0, 1);
    colorId.r = (float) ((object_id & 0x00FF0000) >> 16) / 255.0f;
    colorId.g = (float) ((object_id & 0x0000FF00) >> 8) / 255.0f;
    colorId.b = (float) ((object_id & 0x000000FF)) / 255.0f;
    output.rt_1_output = colorId;

    return output;
}
