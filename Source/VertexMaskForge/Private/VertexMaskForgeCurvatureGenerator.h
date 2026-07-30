#pragma once

#include "CoreMinimal.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

struct FMeshDescription;
struct FStaticMeshLODResources;

namespace VertexMaskForgeCurvatureGenerator
{
	/**
	 * Generates the Curvature Mask in RENDER VERTEX order for one entry (non-Nanite/non-Source-Topology)
	 * -- ensures the entry's cached raw analysis is current (EnsureCurvatureRawCache), reprocesses it
	 * through Type/Multiplier/Blur/Levels (ApplyCurvatureArtisticParams, Dynamic Mesh Vertex domain),
	 * then re-indexes into render-vertex domain via CurvatureRenderVertexToDynamicMeshVertex -- so every
	 * render vertex sharing a source mesh vertex (a UV seam or hard edge split) reads the IDENTICAL
	 * value, satisfying the "propagate to vertex instances" requirement. A render vertex with no
	 * correspondence (INDEX_NONE) is left unwritten (bHasValue false), never guessed.
	 */
	FVertexMaskForgeScalarMask GenerateCurvatureMask(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		FVertexMaskForgeGeneratorState& GeneratorState,
		const FMeshDescription* MeshDescription,
		const FStaticMeshLODResources& LOD0,
		const EVertexMaskForgeCurvatureType Type,
		const float Multiplier,
		const float Blur,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert);

	/**
	 * Sibling of GenerateCurvatureMask for Source-Topology (Nanite) entries -- indexed directly by
	 * DYNAMIC MESH VERTEX ID (Mesh.MaxVertexID()-sized, sparse-safe), no render-vertex correspondence
	 * needed: UpdateWorkingColorsSourceTopology already looks this mask up by
	 * Mesh.GetTriangle(TriangleID)[Corner] per corner (see its IndexOverride switch), exactly the same
	 * domain BoundingBoxMask already uses in this mode.
	 */
	FVertexMaskForgeScalarMask GenerateCurvatureMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		FVertexMaskForgeGeneratorState& GeneratorState,
		const EVertexMaskForgeCurvatureType Type,
		const float Multiplier,
		const float Blur,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert);
}
