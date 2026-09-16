# Copyright MadFall. All Rights Reserved.
#
# The cracks a damaged block wears, shared by the textured surface material
# (make_pbr_material.py) and the procedural one (make_voxel_material.py) so the
# two cannot drift apart.
#
# WHY IN THE MATERIAL AT ALL: a block's damage was invisible. A wall a horde had
# spent a night on looked exactly like a fresh one, and mining gave no feedback
# between the first hit and the block vanishing - the only readout was the
# target line's "42% damaged". Damage already travelled to the mesher (every
# SetVoxel remeshes its chunk), so drawing it costs no new meshing work; the
# mesher puts the voxel's damage in UV2.x and these lines draw it.

# A method for the shader's helper struct: it uses the struct's own hash, H.
CRACK_METHOD = """	float Crack(float2 P, float Dmg)
	{
		// A jittered cell network: a pixel that is nearly the same distance from
		// two cell centres sits on a boundary between them, which is where a
		// brittle thing splits. Cells whose gate hash is low crack first, so
		// damage spreads as a few fissures before it becomes a web.
		float2 I = floor(P);
		float2 Fr = P - I;
		float Best = 8.0;
		float Second = 8.0;
		float Gate = 0.0;
		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				float2 Cell = float2(dx, dy);
				float2 Centre = Cell + float2(H(float3(I + Cell, 3.0)), H(float3(I + Cell, 7.0)));
				float D = length(Fr - Centre);
				if (D < Best) { Second = Best; Best = D; Gate = H(float3(I + Cell, 11.0)); }
				else if (D < Second) { Second = D; }
			}
		}
		float Appear = saturate(Dmg * 2.4 - Gate * 0.85);
		// Wide enough to read on a busy rock face, not so wide that concrete
		// turns to gravel: about 1 cm of a 33 cm cell at a quarter damage, 4 cm
		// at full. Tuned by eye against both, which is what these numbers are.
		float Width = (0.02 + 0.11 * Dmg) * Appear;
		return saturate(1.0 - (Second - Best) / max(Width, 1e-4));
	}
"""

# Applied late, after weather: cracks are in the surface, so snow and water sit
# in them rather than the other way round. Expects Col, Rough, WN, NN, WP,
# AxisU, AxisV, Dist and Dmg to be in scope, and F to be the helper struct.
CRACK_APPLY = """
// --- damage -----------------------------------------------------------------
// Cracks cost three evaluations, but only on a damaged block within 45 m: a
// whole one (Dmg 0, which is nearly every block in view) takes the branch and
// none of them. They fade out with distance like the patterns, where the cell
// network would alias into a grey haze.
float DmgAmt = saturate(Dmg);
if (DmgAmt > 0.004)
{
	float NearBy = 1.0 - saturate((Dist - 1500.0) / 3000.0);
	if (NearBy > 0.004)
	{
		// Three cells a voxel: fewer reads as a pattern, more as gravel.
		float2 CrackP = float2(dot(WP, AxisU), -dot(WP, AxisV)) / 33.3333;
		float Crack = F.Crack(CrackP, DmgAmt) * NearBy;
		if (Crack > 0.002)
		{
			// A crack is a groove, so the normal tilts out of it on both sides:
			// the height is -Crack and the slope its gradient, as the patterns do.
			const float Eps = 0.05;
			float CrackU = F.Crack(CrackP + float2(Eps, 0.0), DmgAmt) * NearBy;
			float CrackV = F.Crack(CrackP + float2(0.0, Eps), DmgAmt) * NearBy;
			WN = normalize(WN + ((CrackU - Crack) * AxisU - (CrackV - Crack) * AxisV) * 2.2);
			Col *= lerp(1.0, 0.16, Crack);
			Rough = lerp(Rough, 0.95, Crack);
		}
	}
}
"""
