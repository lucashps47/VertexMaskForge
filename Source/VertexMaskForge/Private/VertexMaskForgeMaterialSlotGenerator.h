#pragma once

#include "CoreMinimal.h"

struct FVertexMaskForgeScalarMask;
struct FVertexMaskForgeWorkingMesh;
struct FStaticMeshLODResources;

namespace VertexMaskForgeMaterialSlotGenerator
{
	/**
	 * AUDITED (V2-D): the binary raw mask, render-vertex domain -- RawMask[i] = 1.0 iff
	 * WorkingMesh.RenderVertexToMaterialSlot[i] == SelectedSlotIndex, else 0.0; Invert complements
	 * (1.0<->0.0) AFTER that comparison, per the explicit formula. Refuses to generate (Unavailable) if
	 * the lookup itself is invalid/ambiguous (see BuildMaterialSlotLookups) or SelectedSlotIndex is out
	 * of range -- never silently produces a wrong/empty mask.
	 */
	FVertexMaskForgeScalarMask GenerateMaterialSlotMask(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FStaticMeshLODResources& LOD0,
		int32 SelectedSlotIndex,
		bool bInvert);

	/**
	 * AUDITED (V2-D): sibling of GenerateMaterialSlotMask for Source-Topology (Nanite) entries --
	 * CORNER-EXACT (Mesh.TriangleCount()*3, indexed by CornerIndex directly), deliberately NOT
	 * Dynamic-Mesh-Vertex-domain like Curvature/Noise: all three corners of a triangle share that
	 * triangle's OWN resolved slot (WorkingMesh.DynamicTriangleToMaterialSlot[TriangleID]), so two
	 * corners at the same position/VertexID on opposite sides of a material boundary correctly read
	 * different values -- see UpdateWorkingColorsSourceTopology's own IndexOverride switch (CornerIndex
	 * case) for how this is consumed.
	 */
	FVertexMaskForgeScalarMask GenerateMaterialSlotMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		int32 SelectedSlotIndex,
		bool bInvert);
}
