#pragma once

#include "CoreMinimal.h"

struct FVertexMaskForgeScalarMask;
struct FVertexMaskForgeWorkingMesh;
struct FVertexMaskForgeGeneratorState;
struct FVertexMaskForgeNoiseGenerativeParams;
struct FStaticMeshLODResources;

namespace VertexMaskForgeNoiseGenerator
{
	/**
	 * Generates the Noise Mask in RENDER VERTEX order for one entry (non-Nanite/non-Source-Topology) --
	 * ensures the entry's cached raw pattern is current for the CURRENT generative parameters
	 * (EnsureNoiseRawCache), then reprocesses it through Multiplier/Levels/Invert
	 * (ApplyNoiseArtisticParams). Values are already render-vertex-domain (NoiseRawCache is generated
	 * directly in that domain -- see EnsureNoiseRawCache -- no separate correspondence table is needed
	 * the way Curvature's topology-dependent analysis requires, since Noise depends only on POSITION,
	 * and LOD0's own PositionVertexBuffer already stores the correct per-render-vertex position,
	 * including identical duplicated values for UV-seam-split wedges at the same source position).
	 */
	FVertexMaskForgeScalarMask GenerateNoiseMask(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		FVertexMaskForgeGeneratorState& GeneratorState,
		const FStaticMeshLODResources& LOD0,
		const FVertexMaskForgeNoiseGenerativeParams& Params,
		float Multiplier,
		float LevelsMin,
		float LevelsMax,
		bool bInvert);

	/**
	 * Sibling of GenerateNoiseMask for Source-Topology (Nanite) entries -- indexed directly by DYNAMIC
	 * MESH VERTEX ID (Mesh.MaxVertexID()-sized, sparse-safe), no render-vertex correspondence needed:
	 * UpdateWorkingColorsSourceTopology already looks this mask up by Mesh.GetTriangle(TriangleID)[Corner]
	 * per corner (see its IndexOverride switch), exactly the same domain BoundingBoxMask/CurvatureMask
	 * already use in this mode.
	 */
	FVertexMaskForgeScalarMask GenerateNoiseMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		FVertexMaskForgeGeneratorState& GeneratorState,
		const FVertexMaskForgeNoiseGenerativeParams& Params,
		float Multiplier,
		float LevelsMin,
		float LevelsMax,
		bool bInvert);
}
