# Copyright MadFall. All Rights Reserved.
#
# The house style, in one place: the HLSL both voxel materials end with, and the
# numbers behind it. Scripts/make_voxel_material.py (procedural patterns) and
# Scripts/make_pbr_material.py (photo texture sets) each paste STYLISE into
# their shader just before the crack overlay, so a scanned rock block and a
# pattern-drawn concrete one come out of the same filter.
#
# WHY: the first playtests looked like two games in one frame - photo-scanned
# granite and planks beside flat green cubes and painted grass. The geometry is
# blocky and stays blocky, so the textures move: banded instead of continuous,
# a little more colour, matt instead of glossy, and their fine relief flattened
# so the shape of a block reads before its surface does.
#
# Bump STYLE_VERSION when these change: both material scripts fold it into
# their own version tag, so an edit here rebuilds both materials.

STYLE_VERSION = "3"

# Every number the look depends on, with what it costs if it moves.
# Detail lives around the surface's own colour, not in absolute brightness:
# banding raw photo luminance crushed a scanned rock into black blotches, and a
# scan and a drawn pattern still disagreed about what colour stone is.
DETAIL = 0.5         # how much of the texture's variation survives. 0 is a flat colour chip.
BANDS = 7.0          # steps of lightness, applied after flattening. Fewer reads as poster art.
DETAIL_KEEP = 0.45   # 0 is hard bands, 1 leaves the (already flattened) texture smooth.
SATURATION = 1.06    # a touch more colour than the scan; 1.2 turned grass acid green.
FLATTEN_NORMAL = 0.7 # 1 ignores the normal map entirely; the bevel and AO carry the shape.
MIN_ROUGH = 0.6      # no wet sheen on dry ground. Rain and puddles still override it.
SPECULAR = 0.28      # default 0.5 is plastic under a low sun.

STYLISE = """
// --- house style (Scripts/style.py) ---------------------------------------
// Pulls a photo scan and a drawn pattern to the same place: lightness in
// bands, a little more colour, matt, and the fine relief flattened so the
// block's shape reads before its surface does. Weather has already had its
// say above - wet gloss and puddles survive this, which is why the roughness
// floor is a lerp toward the maximum rather than a clamp on everything.
{
	// 1. The surface's own colour is the base; the texture becomes variation
	//    around it. This is what makes a photo scan and a drawn pattern of the
	//    same material agree, and it keeps scans out of black and white.
	float3 StyleBase = max(VC.rgb, 0.02);
	Col = lerp(StyleBase, Col, %DETAIL%);
	// 2. Band what is left, so the variation reads as painted steps.
	float StyleLum = max(dot(Col, float3(0.299, 0.587, 0.114)), 1e-4);
	float StyleStep = floor(StyleLum * %BANDS% + 0.5) / %BANDS%;
	Col *= lerp(StyleStep, StyleLum, %DETAIL_KEEP%) / StyleLum;
	float StyleGrey = dot(Col, float3(0.299, 0.587, 0.114));
	Col = max(lerp(StyleGrey.xxx, Col, %SATURATION%), 0.0);
	Rough = lerp(max(Rough, %MIN_ROUGH%), Rough, saturate(1.0 - %MIN_ROUGH%) * 0.35);
	WN = normalize(lerp(WN, NN, %FLATTEN_NORMAL%));
}
"""


def hlsl():
    """STYLISE with the numbers above substituted in."""
    return (STYLISE
            .replace("%BANDS%", "%.2f" % BANDS)
            .replace("%DETAIL_KEEP%", "%.2f" % DETAIL_KEEP)
            .replace("%SATURATION%", "%.2f" % SATURATION)
            .replace("%DETAIL%", "%.2f" % DETAIL)
            .replace("%MIN_ROUGH%", "%.2f" % MIN_ROUGH)
            .replace("%FLATTEN_NORMAL%", "%.2f" % FLATTEN_NORMAL))
