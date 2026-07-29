#pragma once

#include "CoreMinimal.h"
#include "Math/Transform.h"
#include "Templates/UniquePtr.h"

struct FVertexMaskForgeScalarMask;
struct FVertexMaskForgeAOCache;
struct FVertexMaskForgeSourceTopologyAOCache;
struct FVertexMaskForgeAOParams;
struct FStaticMeshLODResources;
class UStaticMesh;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace VertexMaskForgeAmbientOcclusionGenerator
{
	/**
	 * Cheap, geometry-cache-FREE structural validation for the Ambient Occlusion slot's ENTRY-LEVEL
	 * gating (see FVertexMaskForgeWorkingMesh::AmbientOcclusionMask's own doc comment) -- mirrors
	 * GenerateAmbientOcclusionMask's own early-exit checks (NumRenderVerts>0, at least one triangle,
	 * normal buffer count matches) WITHOUT touching FVertexMaskForgeAOCache, building a WorldMesh/Tree,
	 * or running a single raycast. AUDITED (double-AO-execution fix): this is what lets
	 * RunAutoUpdatePreview determine Ready/Unavailable for the AO slot
	 * WITHOUT ever calling GenerateAmbientOcclusionMask at entry level -- the ONLY call site that ever
	 * performs the real (expensive) computation is ApplyPreviewToEntry, exactly once per component, per
	 * update. See ApplyPreviewToEntry's own doc comment for the full audit of the bug this replaced.
	 */
	bool IsAmbientOcclusionInputValid(const FStaticMeshLODResources& LOD0);

	/** Sibling of IsAmbientOcclusionInputValid for Source-Topology (Nanite) entries -- validates
	 *  WorkingMesh.Mesh (the SOURCE-topology FDynamicMesh3) instead of LOD0 render buffers. AUDITED
	 *  (index-safety correction): never requires compactness (see
	 *  GenerateAmbientOcclusionMaskFromDynamicMesh's own INDEX SAFETY note); checks for a usable Normal
	 *  Overlay instead, since that is what AO in this domain actually reads (EnsureNormalOverlay
	 *  guarantees one exists for any Ready working mesh, so this only fails for a genuinely empty mesh). */
	bool IsAmbientOcclusionInputValidForDynamicMesh(const UE::Geometry::FDynamicMesh3* Mesh);

	/**
	 * Generates the Ambient Occlusion Mask directly in RENDER VERTEX order for one component, using
	 * CPU hemisphere raycasts against the component's OWN geometry via a GeometryCore
	 * UE::Geometry::FDynamicMeshAABBTree3 (UE 5.8's stable, Runtime-module mesh spatial index).
	 *
	 * AUDITED (space, per the checkpoint's explicit requirement): the occluder tree, sample origins,
	 * and sample directions are ALL built and evaluated in WORLD SPACE, using THIS component's own
	 * ComponentTransform -- never Local Space. This is a deliberate departure from
	 * GenerateBoundingBoxMask's Local-Space-by-default convention: a Bounding Box axis gradient is
	 * scale-invariant by construction (it normalizes by its own measured extent), but Ambient
	 * Occlusion is NOT -- Max Distance and Bias are absolute distances, and under NON-UNIFORM component
	 * scale a Local-Space sphere of radius MaxDistance does not correspond to any sphere in World
	 * Space (it becomes direction-dependent), which would silently make Max Distance mean different
	 * things along different axes. Baking geometry into World Space once per component sidesteps that
	 * entirely: Max Distance and Bias are then simply literal Unreal units, correct under uniform OR
	 * non-uniform scale, with no per-axis correction needed anywhere else in this function. The cost is
	 * that the tree is cached PER COMPONENT (see FVertexMaskForgeAOCache), not shared across multiple
	 * instances of the same asset the way the Bounding Box Mask's entry-level reference is -- an
	 * accepted, documented limitation of this first version (see the checkpoint report).
	 *
	 * NORMALS (audited): read directly from LOD0.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ --
	 * i.e. the REAL RENDER NORMAL, in the SAME render-vertex order/domain as BaselineColors/
	 * WorkingColors (so hard-edge/UV-seam vertices that share a position but have different render
	 * normals are correctly evaluated independently, exactly like BaselineColors already preserves
	 * seams -- see UpdateWorkingColors). Deliberately NOT the Dynamic Mesh's normal overlay: that lives
	 * in a different, POSITION-MATCHED vertex domain (FindMatchingVertexID), which the codebase already
	 * documents as reserved/unused for spatial-only generators (see the audit note above
	 * BuildPositionBuckets) -- reintroducing it here would be exactly the mistake that note warns
	 * against. Transformed to World Space with the inverse-transpose of the component's matrix (the
	 * mathematically correct normal transform under non-uniform scale -- a plain TransformVector would
	 * skew the normal off-perpendicular). A vertex whose render normal fails to normalize (degenerate/
	 * zero -- pathological content only) falls back to the geometric face normal of one of its own
	 * incident triangles in the just-baked WorldMesh (UE::Geometry::FDynamicMesh3::GetTriNormal),
	 * itself a safe, always-available fallback since every render vertex used here is guaranteed to
	 * have at least one incident triangle (degenerate triangles are skipped when the mesh is built, but
	 * a render vertex referenced only by degenerate triangles simply keeps FVector::UpVector as the
	 * final fallback -- never a NaN or unnormalized vector). BOTH fallback paths are counted in
	 * NumDegenerateNormals and logged in aggregate -- never silently substituted without a trace (see
	 * DIAGNOSTICS below).
	 *
	 * SELF-HIT (AUDITED AGAIN, per the "10s wait + black regions" pre-test correction -- supersedes the
	 * PREVIOUS round's FindNearestHitTriangle-plus-epsilon fix): that fix had its own bug -- rejecting
	 * a too-close FindNearestHitTriangle result silently treated the sample as "not occluded", but
	 * since FindNearestHitTriangle only ever returns the SINGLE closest hit, a spurious near-origin
	 * self-intersection could hide a genuine, farther, legitimate occluder on that exact ray -- the
	 * ray was never re-queried past the rejected hit. This under-counted occlusion (AO too LOW) in
	 * exactly the concave/creased regions where self-intersection noise is most likely, which is what
	 * manual testing saw as incoherent "black" patches (this mask's convention is 0 = exposed, so an
	 * artificially-low value reads as if the surface were unoccluded/black even where it visibly is
	 * not -- see EVertexMaskForgeScalarMaskSource::AmbientOcclusion's own doc note on the convention).
	 * It was also markedly slower: FindNearestHitTriangle cannot early-out the way an any-hit query can.
	 * FIX: back to TestAnyHitTriangle (any-hit, early-out), trusting Bias alone -- no epsilon, no
	 * triangle-identity filtering of any kind. Bias (artist-controlled, World Space Unreal units,
	 * minimum 0.001) is solely responsible for moving the ray origin definitively off the originating
	 * surface; if a specific asset still shows self-shadowing artifacts, the fix is a larger Bias, not
	 * more code here -- exactly the contract the original checkpoint request specified for Bias, and
	 * the option this round's correction explicitly said to prefer if it produces a correct result.
	 * This is topology-independent (no dependency on hard-edge/UV-seam splits, since no triangle ID or
	 * vertex ID plays any role in the decision) and never hides a legitimate second occluder, since the
	 * FIRST hit found (any hit) is by definition a real surface at or beyond the Bias-offset origin.
	 *
	 * CORRESPONDENCE (audited): WorldMesh/Tree are built directly from LOD0.VertexBuffers.
	 * PositionVertexBuffer (one AppendVertex per render vertex, in order) and LOD0.IndexBuffer (one
	 * AppendTriangle per LOD0 triangle, using the SAME render vertex indices) -- never MeshDescription,
	 * never FDynamicMesh3 position-matching, never the WedgeMap. This guarantees Mask.Values.Num() ==
	 * NumRenderVerts exactly, by the same "dense by construction" contract GenerateBoundingBoxMask
	 * already relies on, and needs no separate correspondence step at all.
	 *
	 * CACHE: WorldMesh/Tree are rebuilt whenever Mesh identity, LOD0.DerivedDataKey, NumRenderVerts,
	 * NumIndices, or ComponentTransform no longer match what they were last built from (see
	 * FVertexMaskForgeAOCache's own doc comment for the full matrix -- AUDITED, DerivedDataKey
	 * false-mismatch fix: the key is now plain-equality compared, so an asset whose DerivedDataKey is
	 * never populated no longer permanently defeats the cache); RawValues are rebuilt only when
	 * Samples/MaxDistance/Bias actually changed, or Layer 1 was just rebuilt. Invert is applied AFTER
	 * this cache lookup, every call, directly from RawValues
	 * -- never touches the tree or recomputes a single raycast. Blend Mode/Opacity/Channel Filter/
	 * Preview Mode never call this function at all (they operate entirely downstream on already-
	 * composed WorkingColors), so they can never trigger either layer's rebuild.
	 *
	 * EFFECTIVE SAMPLES: the caller (RunAutoUpdatePreview, the tool's single live-regeneration entry
	 * point) always passes the full, user-chosen Samples value -- this function itself always raycasts
	 * at whatever Params.Samples it is given and reports the ACTUAL value used in
	 * Mask.UsedAOParams.Samples (diagnostic-accurate, matching UsedAxisParams' own contract). AOSamples
	 * itself is never mutated by live regeneration, only read.
	 *
	 * SAMPLING (AUDITED, banding fix): BuildHemisphereSampleDirections is now COSINE-WEIGHTED (see its
	 * own doc comment), and every render vertex's copy of that fixed direction set is additionally
	 * rotated about the normal by ComputeDeterministicScrambleAngle(WorldPos) -- a position-keyed,
	 * deterministic azimuthal offset -- before being transformed into World Space via that vertex's own
	 * tangent frame. This breaks up the structured/coherent banding a single, unrotated direction set
	 * produced when combined with FindBestAxisVectors' basis discontinuities between neighboring
	 * vertices, while remaining fully deterministic (Test D) and seam-consistent (position-keyed, not
	 * index-keyed -- see ComputeDeterministicScrambleAngle's own doc comment).
	 *
	 * PARALLELIZATION (AUDITED, per explicit source-level confirmation): the per-vertex raycast loop
	 * runs via ParallelFor. TMeshAABBTree3 (UE::Geometry::FDynamicMeshAABBTree3's template) declares no
	 * `mutable` members anywhere in Spatial/MeshAABBTree3.h, and its query methods (TestAnyHitTriangle/
	 * FindNearestHitTriangle) are const and read-only over BoxToIndex/Mesh/RootIndex, all populated
	 * once at Build() time and never touched again -- confirmed by reading the real UE 5.8 header, not
	 * assumed. This exact pattern (ParallelFor over render vertices, querying one shared, already-built
	 * FDynamicMeshAABBTree3 per worker) is Epic's own established idiom in the SAME engine version --
	 * e.g. Engine/Plugins/Runtime/MeshModelingToolset's RenderCaptureFunctions.cpp (occlusion/capture
	 * baking) and MeshVertexPaintTool.cpp both do exactly this. Each worker here writes only its own
	 * render vertex index into Cache.RawValues (pre-sized before the ParallelFor, never reallocated
	 * during it); WorldMesh/Tree/RenderTangents/NormalMatrix/LocalSampleDirs/Options are all read-only,
	 * captured by reference; no UObject is touched inside the parallel body (only FDynamicMesh3/
	 * FStaticMeshVertexBuffer/FMatrix/FDynamicMeshAABBTree3, none of them UObjects) -- so no Game
	 * Thread/GC safety concern applies. Determinism is preserved: each vertex's own result depends only
	 * on its own index/position/normal and the shared, already-built, read-only Tree -- never on
	 * execution order between vertices.
	 *
	 * LOGGING (post-cleanup checkpoint): silent on cache hits and on routine parameter changes. A
	 * single compact Verbose line reports WHY a cache layer missed (no addresses/hashes/full transform
	 * dumps); a single Log-level "AO generated: N vertices, S samples, T ms" line fires only when a
	 * real raycast pass actually runs (never on a RawValues cache hit); a Warning fires only if the
	 * raycast pass itself produces NaN/out-of-range values (a genuine data anomaly).
	 */
	FVertexMaskForgeScalarMask GenerateAmbientOcclusionMask(
		TUniquePtr<FVertexMaskForgeAOCache>& CachePtr,
		const UStaticMesh* Mesh,
		const FStaticMeshLODResources& LOD0,
		const FTransform& ComponentTransform,
		const FVertexMaskForgeAOParams& RawParams);

	/**
	 * AUDITED (Nanite source-topology support -- CORRECTED per hard-edge review): sibling of
	 * GenerateAmbientOcclusionMask, used ONLY for entries in Source-Topology mode. Same algorithm end
	 * to end (cosine-weighted hemisphere sampling, position-keyed deterministic scramble,
	 * TestAnyHitTriangle any-hit self-hit handling, ParallelFor parallelization -- see
	 * GenerateAmbientOcclusionMask's own doc comments, unchanged and not repeated here) -- the source of
	 * geometry is SourceMesh (WorkingMesh.Mesh, built from the asset's SOURCE MeshDescription) instead
	 * of FStaticMeshLODResources' render buffers, which is what makes AO correct for a Nanite mesh:
	 * SourceMesh is the SAME full-fidelity topology Paint Vertex Colors itself reads and writes (see
	 * the native-tool audit), never the reduced/re-clustered LOD 0 fallback proxy.
	 *
	 * INDEXED BY NORMAL OVERLAY ELEMENT ID, NOT VERTEX ID (corrected): a raw AO value is a pure
	 * function of (position, normal) -- so two corners at the SAME vertex position with DIFFERENT
	 * normals (a hard edge) must get independently-cast rays and independently-cached results, exactly
	 * like the old render-vertex path preserved via one array slot per render vertex (which already
	 * duplicates positions at seams). NormalOverlay->GetElement(id) gives that exact (position, normal)
	 * pair directly (position via GetParentVertex), so this function computes and caches one raw value
	 * per Normal Overlay element -- never an averaged per-DynamicMesh-vertex normal, which would
	 * silently smooth hard edges. A caller resolves a given triangle corner's AO value via
	 * NormalOverlay->GetTriangle(TriangleID)[corner] as the lookup index (see
	 * UpdateWorkingColorsSourceTopology). See EnsureNormalOverlay for the one narrow exception (source
	 * genuinely has no normals at all) where a synthesized, smooth fallback is used and logged.
	 *
	 * INDEX SAFETY (corrected): never assumes SourceMesh or its Normal Overlay is compact. Sized by
	 * NormalOverlay->MaxElementID() (sparse-safe via IsElement()); WorldMesh (a fresh, always-compact
	 * mesh built purely for raycasting) is populated via an explicit VertexID -> dense-WorldMesh-index
	 * remap (VertexIdToWorldIndex), never assuming SourceMesh's own VertexID/TriangleID are already
	 * dense.
	 *
	 * CACHE IDENTITY (corrected): keyed by CachedSourceMesh (address) AND CachedGeometryFingerprint
	 * (positions + normals content hash, see ComputeDynamicMeshGeometryFingerprint) -- vertex/triangle
	 * counts alone are not proof of identical geometry.
	 */
	FVertexMaskForgeScalarMask GenerateAmbientOcclusionMaskFromDynamicMesh(
		TUniquePtr<FVertexMaskForgeSourceTopologyAOCache>& CachePtr,
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const uint32 SourceGeometryFingerprint,
		const FTransform& ComponentTransform,
		const FVertexMaskForgeAOParams& RawParams);
}
