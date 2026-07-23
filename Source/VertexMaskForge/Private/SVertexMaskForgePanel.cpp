#include "SVertexMaskForgePanel.h"

#include "Async/ParallelFor.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Set.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Logging/LogMacros.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Math/NumericLimits.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/MessageDialog.h"
#include "RenderResource.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "TimerManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "SPrimaryButton.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogVertexMaskForge, Log, All);

// FDynamicMesh3 is only forward-declared in the header; these special member functions are
// defined here, now that it is a complete type, so FVertexMaskForgeWorkingMesh itself owns the
// responsibility for being safe to destroy/move -- no other class's destructor is relied upon.
FVertexMaskForgeWorkingMesh::~FVertexMaskForgeWorkingMesh() = default;
FVertexMaskForgeWorkingMesh::FVertexMaskForgeWorkingMesh(FVertexMaskForgeWorkingMesh&&) = default;
FVertexMaskForgeWorkingMesh& FVertexMaskForgeWorkingMesh::operator=(FVertexMaskForgeWorkingMesh&&) = default;

/**
 * Full definition of the opaque cache forward-declared in the header (see the header's own doc
 * comment on FVertexMaskForgePreviewComponentState::AOCache). One instance per component, created
 * lazily by VertexMaskForgePanel::GenerateAmbientOcclusionMask.
 *
 * Two independently-invalidated layers, exactly matching the checkpoint's cache-invalidation
 * contract (see that function's own doc comment for the full matrix of what does/doesn't invalidate
 * each layer):
 *   - WorldMesh/Tree: the occluder geometry and its spatial index, baked in WORLD SPACE for this
 *     specific component's transform (see GenerateAmbientOcclusionMask for why World Space, not
 *     Local, is required for correctness under non-uniform scale). Rebuilt whenever ANY of
 *     CachedMesh/CachedDerivedDataKey/CachedNumRenderVerts/CachedNumIndices/CachedTransform no
 *     longer match -- see GenerateAmbientOcclusionMask's own doc comment (AUDITED, geometry cache
 *     key fix) for exactly what each field catches and why.
 *   - RawValues: the raw (pre-Invert) occlusion fraction per render vertex, in [0, 1]. Rebuilt only
 *     when Samples/MaxDistance/Bias actually change, OR when WorldMesh/Tree above was just rebuilt
 *     (new geometry/transform invalidates any previously-computed samples too).
 */
struct FVertexMaskForgeAOCache
{
	TUniquePtr<UE::Geometry::FDynamicMesh3> WorldMesh;
	TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
	bool bTreeValid = false;

	/** Geometric identity WorldMesh/Tree were last built from -- ALL fields below must match the
	 *  CURRENT inputs for a cache hit; see GenerateAmbientOcclusionMask for the exact comparison.
	 *  AUDITED (DerivedDataKey false-mismatch fix): CachedDerivedDataKey is compared by PLAIN equality
	 *  -- "both empty" IS a match (an asset whose LOD never populates this field is not, by itself,
	 *  evidence of a change), never an automatic veto; Mesh/NumRenderVerts/NumIndices/Transform remain
	 *  independent safeguards in the same AND-chain regardless of this field's availability. */
	TWeakObjectPtr<const UStaticMesh> CachedMesh;
	FString CachedDerivedDataKey;
	int32 CachedNumRenderVerts = 0;
	int32 CachedNumIndices = 0;
	FTransform CachedTransform = FTransform::Identity;

	TArray<float> RawValues;
	int32 CachedSamples = 0;
	float CachedMaxDistance = 0.f;
	float CachedBias = 0.f;
	bool bValuesValid = false;
};

/**
 * AUDITED (Nanite source-topology support): sibling of FVertexMaskForgeAOCache, used ONLY for entries
 * in Source-Topology mode (every Nanite-enabled Static Mesh -- see
 * FVertexMaskForgeSelectedMesh::bUseSourceTopology). Same two-layer contract (WorldMesh/Tree geometric
 * cache + RawValues per-parameter cache) as FVertexMaskForgeAOCache, but keyed against WorkingMesh.Mesh
 * (the FDynamicMesh3 built from the SOURCE MeshDescription) instead of FStaticMeshLODResources.
 *
 * AUDITED (cache-key robustness fix): identity is CachedSourceMesh (the FDynamicMesh3 pointer, address
 * only, never dereferenced when stale) AND CachedGeometryFingerprint (see
 * VertexMaskForgePanel::ComputeDynamicMeshGeometryFingerprint) -- vertex/triangle COUNTS alone are
 * insufficient (two genuinely different geometries, or the same geometry with only its normals
 * changed, can share identical counts), so the fingerprint (hash of every vertex position AND every
 * Normal Overlay element) is what actually proves the geometry AND the normals used by the AO rays
 * are unchanged. Never used for, and never shared with, an entry in the render-vertex domain.
 */
struct FVertexMaskForgeSourceTopologyAOCache
{
	TUniquePtr<UE::Geometry::FDynamicMesh3> WorldMesh;
	TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
	bool bTreeValid = false;

	/** Compared by address only -- WorkingMesh.Mesh is rebuilt (new object) on every Refresh Selection,
	 *  and this cache is always destroyed together with the session (see
	 *  FVertexMaskForgePreviewComponentState::SourceTopologyAOCache) before a new WorkingMesh could
	 *  ever be built while this pointer is still alive, so a stale address is never dereferenced. */
	const UE::Geometry::FDynamicMesh3* CachedSourceMesh = nullptr;
	uint32 CachedGeometryFingerprint = 0;
	FTransform CachedTransform = FTransform::Identity;

	/** Indexed by Normal Overlay Element ID -- see GenerateAmbientOcclusionMaskFromDynamicMesh's own
	 *  doc comment for why (hard-edge preservation: two corners at the same vertex position but with
	 *  different normals must never share a raw AO value). Sized to the overlay's MaxElementID(),
	 *  sparse-safe (an index that was never IsElement() is simply never written or read). */
	TArray<float> RawValues;
	int32 CachedSamples = 0;
	float CachedMaxDistance = 0.f;
	float CachedBias = 0.f;
	bool bValuesValid = false;
};

// FVertexMaskForgeAOCache is only forward-declared in the header; these special member functions are
// defined here, now that it is a complete type -- same pattern and reason as
// FVertexMaskForgeWorkingMesh's own special members just above.
FVertexMaskForgePreviewComponentState::~FVertexMaskForgePreviewComponentState() = default;
FVertexMaskForgePreviewComponentState::FVertexMaskForgePreviewComponentState(FVertexMaskForgePreviewComponentState&&) = default;
FVertexMaskForgePreviewComponentState& FVertexMaskForgePreviewComponentState::operator=(FVertexMaskForgePreviewComponentState&&) = default;

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace VertexMaskForgePanel
{
	/** Adds a mesh to the collected list, or merges its source flags if already present. */
	static void AddOrUpdateSelectedMesh(
		TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
		TMap<FString, int32>& InOutPathToIndex,
		const FString& AssetPathString,
		const FString& AssetName,
		const TSoftObjectPtr<UStaticMesh>& SoftMesh,
		UStaticMeshComponent* SourceComponent)
	{
		int32 EntryIndex;
		if (const int32* ExistingIndex = InOutPathToIndex.Find(AssetPathString))
		{
			EntryIndex = *ExistingIndex;
		}
		else
		{
			TSharedPtr<FVertexMaskForgeSelectedMesh> NewEntry = MakeShared<FVertexMaskForgeSelectedMesh>();
			NewEntry->Mesh = SoftMesh;
			NewEntry->AssetName = AssetName;
			NewEntry->AssetPathString = AssetPathString;

			EntryIndex = InOutMeshes.Num();
			InOutPathToIndex.Add(AssetPathString, EntryIndex);
			InOutMeshes.Add(MoveTemp(NewEntry));
		}

		// Track the contributing component (Requirement 7: the same asset can be present on
		// multiple selected components; each must get its own preview state and restoration).
		//
		// AUDITED (Problem 2 -- duplicate SourceComponent entries): dedup key here is
		// SourceComponent's raw pointer IDENTITY (via TWeakObjectPtr::Get() equality), never the
		// StaticMesh asset -- an asset can only route to one FVertexMaskForgeSelectedMesh entry to
		// begin with (InOutPathToIndex is keyed by asset path), so this predicate is the sole gate
		// that decides whether a given UStaticMeshComponent gets a NEW FVertexMaskForgePreviewComponentState.
		// This guarantees:
		//   - the SAME SourceComponent (e.g. the calling Actor appearing twice in GetSelectedObjects,
		//     or GetComponents<> enumerating it more than once) produces AT MOST one State, so at most
		//     one PreviewComponent and at most one Actor-hide acquisition for it;
		//   - two DIFFERENT components that happen to reference the SAME UStaticMesh remain fully
		//     independent (different SourceComponent pointers => different States => independent
		//     PreviewComponents and independent per-instance OverrideVertexColors baselines, per
		//     UpdateWorkingColors).
		// RefreshSelection() rebuilds InOutMeshes/InOutPathToIndex from scratch on every call (after
		// DestroyAllPreviews()), so no duplicate can accumulate across refreshes either.
		if (SourceComponent)
		{
			TArray<FVertexMaskForgePreviewComponentState>& PreviewComponents = InOutMeshes[EntryIndex]->PreviewComponents;
			const bool bAlreadyTracked = PreviewComponents.ContainsByPredicate(
				[SourceComponent](const FVertexMaskForgePreviewComponentState& State)
				{
					return State.SourceComponent.Get() == SourceComponent;
				});
			if (!bAlreadyTracked)
			{
				FVertexMaskForgePreviewComponentState NewComponentState;
				NewComponentState.SourceComponent = SourceComponent;
				PreviewComponents.Add(MoveTemp(NewComponentState));
			}
			else
			{
				UE_LOG(LogVertexMaskForge, Verbose,
					TEXT("Vertex Mask Forge: skipped duplicate selection of component '%s' (already tracked for this refresh)."),
					*SourceComponent->GetName());
			}
		}
	}

	/**
	 * Reads LOD 0 / material / Nanite / CPU access diagnostics from a Static Mesh's
	 * render data. Read-only: never touches SourceModel, MeshDescription, or RenderData.
	 * Safe to call with a null or not-yet-built mesh.
	 */
	static FVertexMaskForgeMeshDiagnostics InspectStaticMesh(const UStaticMesh* Mesh)
	{
		FVertexMaskForgeMeshDiagnostics Diagnostics;

		if (!IsValid(Mesh))
		{
			return Diagnostics;
		}

		Diagnostics.NumLODs = Mesh->GetNumLODs();
		Diagnostics.NumMaterialSlots = Mesh->GetStaticMaterials().Num();
		Diagnostics.bAllowCPUAccess = Mesh->bAllowCPUAccess != 0;
		Diagnostics.bNaniteEnabled = Mesh->HasValidNaniteData();

		if (!Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
		{
			return Diagnostics;
		}

		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			return Diagnostics;
		}

		const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];

		Diagnostics.LOD0NumVertices = LOD0.GetNumVertices();
		Diagnostics.LOD0NumTriangles = LOD0.GetNumTriangles();

		const int32 ColorVertexCount = static_cast<int32>(LOD0.VertexBuffers.ColorVertexBuffer.GetNumVertices());
		Diagnostics.LOD0NumColorVertices = ColorVertexCount;

		if (ColorVertexCount <= 0)
		{
			Diagnostics.VertexColorState = EVertexMaskForgeVertexColorState::None;
		}
		else if (ColorVertexCount == Diagnostics.LOD0NumVertices)
		{
			Diagnostics.VertexColorState = EVertexMaskForgeVertexColorState::Present;
		}
		else
		{
			Diagnostics.VertexColorState = EVertexMaskForgeVertexColorState::PartialOrInvalid;
		}

		Diagnostics.bValid = true;

		return Diagnostics;
	}

	// --- Working mesh (FMeshDescription -> FDynamicMesh3) -----------------------------------

	/** Resolved only for the duration of the calling operation; the caller must not store the result. */
	static const UStaticMesh* ResolveWorkingStaticMesh(const TSoftObjectPtr<UStaticMesh>& SoftMesh)
	{
		return SoftMesh.LoadSynchronous();
	}

	/**
	 * Returns the LOD 0 MeshDescription owned by the mesh, or null. The returned pointer refers to
	 * the asset's internal cache and must be copied before use; it must never be stored or mutated.
	 */
	static const FMeshDescription* GetSourceMeshDescription(const UStaticMesh* Mesh)
	{
		if (!IsValid(Mesh))
		{
			return nullptr;
		}
		return Mesh->GetMeshDescription(0);
	}

	/** Creates a fully independent copy; the result shares no memory with Source. */
	static TUniquePtr<FMeshDescription> CopyMeshDescription(const FMeshDescription* Source)
	{
		if (!Source)
		{
			return nullptr;
		}
		return MakeUnique<FMeshDescription>(*Source);
	}

	/**
	 * Converts a standalone MeshDescription copy into a new working FDynamicMesh3.
	 * Also returns the converter's TriIDMap (Dynamic Mesh triangle ID -> source FTriangleID;
	 * requires bCalculateMaps, which defaults to true) so callers can, if needed, re-derive exact
	 * per-corner correspondence between the two meshes -- see ReconstructOmittedColorOverlay().
	 */
	static TUniquePtr<UE::Geometry::FDynamicMesh3> ConvertToDynamicMesh(
		const FMeshDescription& SourceCopy,
		TArray<FTriangleID>& OutTriIDMap)
	{
		TUniquePtr<UE::Geometry::FDynamicMesh3> DynamicMesh = MakeUnique<UE::Geometry::FDynamicMesh3>();

		FMeshDescriptionToDynamicMesh Converter;
		// Static Mesh polygon group IDs are transformed into contiguous Section Indices, which
		// line up with Material Slot indices for the common (non-remapped) case. This does not by
		// itself prove Section Index == Material Slot index on assets with remapped sections.
		Converter.bUseCompactedPolygonGroupIDValues = true;
		Converter.Convert(&SourceCopy, *DynamicMesh);

		OutTriIDMap = MoveTemp(Converter.TriIDMap);

		return DynamicMesh;
	}

	/**
	 * AUDITED (Nanite source-topology support): guarantees Mesh has a Normal Overlay before any AO
	 * generation reads it (see GenerateAmbientOcclusionMaskFromDynamicMesh, which indexes AO raw values
	 * by Normal Overlay Element ID specifically to preserve hard-edge normal splits). The normal case
	 * (the overwhelming majority of authored assets) is a no-op: FMeshDescriptionToDynamicMesh::Convert
	 * already creates and populates this overlay from the source's own VertexInstanceNormals, hard
	 * edges and all. Only synthesizes smooth per-vertex normals -- logged, since it is a genuine (if
	 * rare) data-quality fallback -- when the source MeshDescription had no usable normals to begin
	 * with, in which case there is no hard-edge data to lose in the first place. Idempotent; safe to
	 * call on any working mesh, Nanite or not (a no-op whenever normals already exist).
	 */
	static void EnsureNormalOverlay(UE::Geometry::FDynamicMesh3& Mesh, const FString& AssetNameForLog)
	{
		using namespace UE::Geometry;

		if (Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals() != nullptr)
		{
			return;
		}

		UE_LOG(LogVertexMaskForge, Warning,
			TEXT("Vertex Mask Forge: '%s' has no usable Normal Overlay after conversion -- synthesizing smooth per-vertex normals for Ambient Occlusion (any hard edges in the source could not be preserved because none were found)."),
			*AssetNameForLog);

		if (!Mesh.HasAttributes())
		{
			Mesh.EnableAttributes();
		}
		if (Mesh.Attributes()->NumNormalLayers() == 0)
		{
			Mesh.Attributes()->SetNumNormalLayers(1);
		}

		FMeshNormals TempNormals(&Mesh);
		TempNormals.ComputeVertexNormals();
		TempNormals.CopyToOverlay(Mesh.Attributes()->PrimaryNormals());
	}

	/**
	 * AUDITED (Nanite source-topology support, AO cache robustness fix -- CORRECTED per follow-up
	 * review): content fingerprint of Mesh, covering everything that can affect the AABBTree/raycast
	 * result: vertex positions, triangle CONNECTIVITY (not just counts), the normals used for ray
	 * origins, and the corner -> Normal Element association. Combined via GetTypeHash/HashCombine in a
	 * fixed, deterministic order (VertexIndicesItr / TriangleIndicesItr / ElementIndicesItr, all stable
	 * for an unedited mesh -- never assumed dense; TriangleIndicesItr never assumed to be enumerated by
	 * TriangleID value alone, see below). Computed ONCE, at working-mesh build time (see
	 * BuildWorkingMeshForStaticMesh), and reused for the rest of the session -- Mesh is never mutated in
	 * place afterward (EnsureNormalOverlay, the one exception, always runs BEFORE this is computed).
	 *
	 * CORRECTED: the original version hashed only positions and normal VALUES, in isolation from each
	 * other and from topology -- two meshes with identical vertex positions/normal values but different
	 * TRIANGLE CONNECTIVITY (e.g. a re-triangulated quad, or a corner rewired to a different Normal
	 * Element) would have produced the SAME fingerprint despite the raycast result genuinely differing
	 * (different triangles occlude different rays; a corner's ray now originates from a different
	 * normal). Fixed by additionally hashing, per triangle (iterated via TriangleIndicesItr(), which
	 * never assumes TriangleID is dense): the triangle's own ordinal position in that iteration (an
	 * explicit delimiter -- see below) plus its three VertexIDs (Mesh.GetTriangle(TriangleID), corner
	 * order preserved) plus, when a Normal Overlay is present and this triangle is set in it
	 * (NormalOverlay->IsSetTriangle(TriangleID)), its three Normal Element IDs
	 * (NormalOverlay->GetTriangle(TriangleID), corner order preserved).
	 *
	 * DELIMITER (per explicit requirement -- "impedir sequências ambíguas"): the running per-triangle
	 * ordinal (0, 1, 2, ...) is hashed in BEFORE each triangle's own three-ID group, and the final
	 * ordinal count is hashed in once more at the very end. This binds every VertexID/Normal Element ID
	 * triple to its exact position in the iteration, so two different triangulations cannot produce the
	 * same flattened ID sequence by coincidence (e.g. triangle boundaries shifting) the way an
	 * undelimited flat concatenation of IDs could.
	 */
	static uint32 ComputeDynamicMeshGeometryFingerprint(const UE::Geometry::FDynamicMesh3& Mesh)
	{
		using namespace UE::Geometry;

		const FDynamicMeshNormalOverlay* NormalOverlay =
			(Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals() != nullptr)
			? Mesh.Attributes()->PrimaryNormals() : nullptr;

		uint32 Hash = GetTypeHash(Mesh.VertexCount());
		Hash = HashCombine(Hash, GetTypeHash(Mesh.TriangleCount()));

		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			const FVector3d P = Mesh.GetVertex(VertexID);
			Hash = HashCombine(Hash, GetTypeHash(P.X));
			Hash = HashCombine(Hash, GetTypeHash(P.Y));
			Hash = HashCombine(Hash, GetTypeHash(P.Z));
		}

		if (NormalOverlay)
		{
			for (const int32 ElementID : NormalOverlay->ElementIndicesItr())
			{
				const FVector3f N = NormalOverlay->GetElement(ElementID);
				Hash = HashCombine(Hash, GetTypeHash(N.X));
				Hash = HashCombine(Hash, GetTypeHash(N.Y));
				Hash = HashCombine(Hash, GetTypeHash(N.Z));
			}
		}

		// Triangle connectivity + corner -> Normal Element association, delimited by ordinal position.
		int32 TriangleOrdinal = 0;
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			Hash = HashCombine(Hash, GetTypeHash(TriangleOrdinal));

			const FIndex3i Tri = Mesh.GetTriangle(TriangleID);
			Hash = HashCombine(Hash, GetTypeHash(Tri.A));
			Hash = HashCombine(Hash, GetTypeHash(Tri.B));
			Hash = HashCombine(Hash, GetTypeHash(Tri.C));

			if (NormalOverlay && NormalOverlay->IsSetTriangle(TriangleID))
			{
				const FIndex3i NormalTri = NormalOverlay->GetTriangle(TriangleID);
				Hash = HashCombine(Hash, GetTypeHash(NormalTri.A));
				Hash = HashCombine(Hash, GetTypeHash(NormalTri.B));
				Hash = HashCombine(Hash, GetTypeHash(NormalTri.C));
			}
			else
			{
				// Explicit "no Normal Element association" marker -- distinguishes this case from a
				// genuine (0,0,0)-valued triple, and from the branch above being taken at all.
				Hash = HashCombine(Hash, GetTypeHash(INDEX_NONE));
			}

			++TriangleOrdinal;
		}
		// Final delimiter: total triangle count actually iterated (as opposed to Mesh.TriangleCount(),
		// which was already hashed above but is being re-affirmed here as a terminator specifically for
		// the per-triangle sequence just written).
		Hash = HashCombine(Hash, GetTypeHash(TriangleOrdinal));

		return Hash;
	}

	/**
	 * Rebuilds the primary color overlay directly from the source's Vertex Instance Colors.
	 *
	 * Only called when LOD 0 RenderData has already proven a full Color Vertex Buffer exists
	 * (FVertexMaskForgeMeshDiagnostics::VertexColorState == Present) but
	 * FMeshDescriptionToDynamicMesh::Convert() dropped its own overlay because every source color
	 * exactly equaled the attribute's default (white) -- see FMeshDescriptionToDynamicMesh::Convert()'s
	 * bFoundNonDefaultVertexInstanceColor handling. This function is never used to invent colors for
	 * a mesh that genuinely has no Color Vertex Buffer.
	 *
	 * Corner correspondence is derived from the converter's own TriIDMap (Dynamic Mesh triangle ID
	 * -> source FTriangleID) plus FMeshDescription::GetTriangleVertexInstances(), using the same
	 * corner ordering FMeshDescriptionToDynamicMesh::Convert() itself uses when it calls
	 * MeshOut.AppendTriangle(VertexIDs, ...) with VertexIDs built from
	 * MeshIn->GetTriangleVertices(TriangleID) in lockstep with TriData.TriInstances -- i.e. corner i
	 * of the Dynamic Mesh triangle corresponds to GetTriangleVertexInstances(SourceTriangleID)[i].
	 */
	static void ReconstructOmittedColorOverlay(
		UE::Geometry::FDynamicMesh3& Mesh,
		const FMeshDescription& SourceMeshDescription,
		const TArray<FTriangleID>& TriIDMap)
	{
		using namespace UE::Geometry;

		const FStaticMeshConstAttributes SourceAttributes(SourceMeshDescription);
		const TVertexInstanceAttributesConstRef<FVector4f> InstanceColors = SourceAttributes.GetVertexInstanceColors();
		if (!InstanceColors.IsValid())
		{
			// RenderData claimed colors exist but the MeshDescription copy has no Color attribute
			// at all; nothing safe to reconstruct from. Leave the overlay disabled; the caller
			// reports this contradiction as Invalid rather than fabricating data.
			return;
		}

		if (!Mesh.HasAttributes())
		{
			Mesh.EnableAttributes();
		}
		Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshColorOverlay* ColorOverlay = Mesh.Attributes()->PrimaryColors();
		if (!ColorOverlay)
		{
			return;
		}

		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			if (!TriIDMap.IsValidIndex(TriangleID))
			{
				continue;
			}

			const FTriangleID SourceTriangleID = TriIDMap[TriangleID];
			const TArrayView<const FVertexInstanceID> SourceInstances =
				SourceMeshDescription.GetTriangleVertexInstances(SourceTriangleID);
			if (SourceInstances.Num() != 3)
			{
				continue;
			}

			FIndex3i ElementTri;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector4f Color = InstanceColors.Get(SourceInstances[Corner]);
				ElementTri[Corner] = ColorOverlay->AppendElement(Color);
			}
			ColorOverlay->SetTriangle(TriangleID, ElementTri);
		}
	}

	/** Reads overlay elements into WorkingMesh.ColorStats and sets VertexColorState to Present/Invalid. */
	static void SummarizeColorOverlay(FVertexMaskForgeWorkingMesh& WorkingMesh, const UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay)
	{
		const int32 ElementCount = ColorOverlay ? ColorOverlay->ElementCount() : 0;
		if (!ColorOverlay || ElementCount <= 0)
		{
			WorkingMesh.VertexColorState = EVertexMaskForgeWorkingVertexColorState::Invalid;
			return;
		}

		FVector4f MinColor(1.f, 1.f, 1.f, 1.f);
		FVector4f MaxColor(0.f, 0.f, 0.f, 0.f);
		int32 NumNonWhite = 0;
		int32 NumNonBlack = 0;

		for (const int32 ElementID : ColorOverlay->ElementIndicesItr())
		{
			const FVector4f Color = ColorOverlay->GetElement(ElementID);

			MinColor.X = FMath::Min(MinColor.X, Color.X);
			MinColor.Y = FMath::Min(MinColor.Y, Color.Y);
			MinColor.Z = FMath::Min(MinColor.Z, Color.Z);
			MinColor.W = FMath::Min(MinColor.W, Color.W);

			MaxColor.X = FMath::Max(MaxColor.X, Color.X);
			MaxColor.Y = FMath::Max(MaxColor.Y, Color.Y);
			MaxColor.Z = FMath::Max(MaxColor.Z, Color.Z);
			MaxColor.W = FMath::Max(MaxColor.W, Color.W);

			const float Tolerance = FVertexMaskForgeWorkingMesh::ColorChannelTolerance;
			const bool bIsWhite = FMath::IsNearlyEqual(Color.X, 1.f, Tolerance)
				&& FMath::IsNearlyEqual(Color.Y, 1.f, Tolerance)
				&& FMath::IsNearlyEqual(Color.Z, 1.f, Tolerance);
			const bool bIsBlack = FMath::IsNearlyEqual(Color.X, 0.f, Tolerance)
				&& FMath::IsNearlyEqual(Color.Y, 0.f, Tolerance)
				&& FMath::IsNearlyEqual(Color.Z, 0.f, Tolerance);

			if (!bIsWhite)
			{
				++NumNonWhite;
			}
			if (!bIsBlack)
			{
				++NumNonBlack;
			}
		}

		WorkingMesh.VertexColorState = EVertexMaskForgeWorkingVertexColorState::Present;
		WorkingMesh.ColorStats.NumElements = ElementCount;
		WorkingMesh.ColorStats.MinColor = MinColor;
		WorkingMesh.ColorStats.MaxColor = MaxColor;
		WorkingMesh.ColorStats.NumNonWhite = NumNonWhite;
		WorkingMesh.ColorStats.NumNonBlack = NumNonBlack;
	}

	/**
	 * Fills in topology / Material ID / Vertex Color statistics for a working mesh that has
	 * already been converted. Only reads from WorkingMesh.Mesh and SourceMeshDescription; never
	 * mutates the source. May enable/populate WorkingMesh.Mesh's own color overlay (see below);
	 * this only ever affects the transient working copy, never the source asset.
	 */
	static void ValidateWorkingMesh(
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FMeshDescription& SourceMeshDescription,
		const TArray<FTriangleID>& TriIDMap,
		const FVertexMaskForgeMeshDiagnostics& Diagnostics)
	{
		using namespace UE::Geometry;

		FDynamicMesh3* Mesh = WorkingMesh.Mesh.Get();
		if (!Mesh)
		{
			return;
		}

		WorkingMesh.DynamicVertexCount = Mesh->VertexCount();
		WorkingMesh.DynamicTriangleCount = Mesh->TriangleCount();
		WorkingMesh.SourceTriangleCount = SourceMeshDescription.Triangles().Num();
		WorkingMesh.DiscardedTriangleCount =
			FMath::Max(0, WorkingMesh.SourceTriangleCount - WorkingMesh.DynamicTriangleCount);

		WorkingMesh.bTopologyValid = Mesh->CheckValidity(
			FDynamicMesh3::FValidityOptions::Permissive(),
			EValidityCheckFailMode::ReturnOnly);

		// Material IDs: FMeshDescriptionToDynamicMesh enables and populates this automatically
		// from the (compacted) polygon group IDs when attributes are not disabled.
		if (Mesh->Attributes() && Mesh->Attributes()->HasMaterialID())
		{
			const FDynamicMeshMaterialAttribute* MaterialIDAttrib = Mesh->Attributes()->GetMaterialID();

			TSet<int32> DistinctIDs;
			bool bAllInRange = true;
			for (const int32 TriangleID : Mesh->TriangleIndicesItr())
			{
				const int32 MaterialID = MaterialIDAttrib->GetValue(TriangleID);
				DistinctIDs.Add(MaterialID);
				if (MaterialID < 0 || MaterialID >= Diagnostics.NumMaterialSlots)
				{
					bAllInRange = false;
				}
			}

			WorkingMesh.MaterialIDState = EVertexMaskForgeMaterialIDState::Preserved;
			WorkingMesh.DistinctMaterialIDCount = DistinctIDs.Num();
			WorkingMesh.bMaterialIDsInRange = bAllInRange;
		}
		else
		{
			WorkingMesh.MaterialIDState = EVertexMaskForgeMaterialIDState::Missing;
		}

		// Vertex colors: ground truth is the LOD 0 RenderData diagnostic computed in the previous
		// checkpoint (Color Vertex Buffer presence/coverage), NOT whether the converter happened
		// to keep its own overlay. FMeshDescriptionToDynamicMesh::Convert() drops the primary color
		// overlay whenever every source Vertex Instance Color exactly equals the attribute default
		// (white) -- see bFoundNonDefaultVertexInstanceColor in its implementation. That optimization
		// must not be read as "no Vertex Colors": an authored, uniformly-white overlay is still real
		// data and must report Present.
		switch (Diagnostics.VertexColorState)
		{
		case EVertexMaskForgeVertexColorState::None:
			// RenderData proves there is no Color Vertex Buffer at all: genuinely colorless source.
			WorkingMesh.VertexColorState = EVertexMaskForgeWorkingVertexColorState::Missing;
			break;

		case EVertexMaskForgeVertexColorState::PartialOrInvalid:
			// RenderData's buffer count does not match LOD 0's vertex count. Do not attempt to
			// reconcile or repair; just surface the inconsistency.
			WorkingMesh.VertexColorState = EVertexMaskForgeWorkingVertexColorState::Invalid;
			break;

		case EVertexMaskForgeVertexColorState::Present:
		default:
			if (Mesh->Attributes() && !Mesh->Attributes()->HasPrimaryColors())
			{
				// RenderData proved the buffer is genuinely present and covers every vertex, but
				// Convert() dropped the overlay because every value was exactly the default.
				// Reconstruct it explicitly from the source instead of reporting an absence.
				ReconstructOmittedColorOverlay(*Mesh, SourceMeshDescription, TriIDMap);
			}

			if (Mesh->Attributes() && Mesh->Attributes()->HasPrimaryColors())
			{
				SummarizeColorOverlay(WorkingMesh, Mesh->Attributes()->PrimaryColors());
			}
			else
			{
				// RenderData said Present but no overlay could be materialized (e.g. the
				// MeshDescription copy has no Color attribute at all, contradicting RenderData).
				// Report the contradiction rather than silently downgrading to Missing.
				WorkingMesh.VertexColorState = EVertexMaskForgeWorkingVertexColorState::Invalid;
			}
			break;
		}
	}

	/**
	 * AUDITED (domain-selection correction): EVERY Nanite-enabled Static Mesh uses Source-Topology
	 * mode, unconditionally -- deliberately NOT conditioned on WedgeMap validity. A Nanite mesh WITH a
	 * valid WedgeMap (an explicit High Res Source Model) would still never show anything via per-
	 * instance OverrideVertexColors, since Nanite's runtime renderer never reads that buffer at all
	 * regardless of WedgeMap availability -- routing such a mesh back to the render-vertex path would
	 * silently reintroduce the exact "no visible result" bug this feature fixes. See
	 * FVertexMaskForgeSelectedMesh::bUseSourceTopology's own doc comment for the full rationale.
	 * Render-Vertex mode (OverrideVertexColors/WedgeMap) is reserved for non-Nanite meshes only.
	 */
	static bool ShouldUseSourceTopology(const UStaticMesh* Mesh)
	{
		return IsValid(Mesh) && Mesh->IsNaniteEnabled();
	}

	/**
	 * Orchestrates the full read-only pipeline for one mesh: resolve, fetch MeshDescription,
	 * copy it, convert the copy, then validate. Never mutates Mesh or its package.
	 *
	 * Dirty-flag proof: captures the package's IsDirty() before and after the operation. Only a
	 * clean -> dirty transition is attributed to Vertex Mask Forge (logged as a warning, since it
	 * would indicate a bug); a package that was already dirty beforehand is left alone and merely
	 * noted, and the dirty flag itself is never cleared or set by this code.
	 */
	static FVertexMaskForgeWorkingMesh BuildWorkingMeshForStaticMesh(
		const UStaticMesh* Mesh,
		const FVertexMaskForgeMeshDiagnostics& Diagnostics)
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;

		if (!IsValid(Mesh))
		{
			WorkingMesh.State = EVertexMaskForgeWorkingMeshState::InvalidSource;
			return WorkingMesh;
		}

		const FMeshDescription* SourceMeshDescription = GetSourceMeshDescription(Mesh);
		if (!SourceMeshDescription)
		{
			WorkingMesh.State = EVertexMaskForgeWorkingMeshState::SourceMeshDescriptionUnavailable;
			return WorkingMesh;
		}

		const UPackage* Package = Mesh->GetPackage();
		const bool bWasDirtyBefore = Package && Package->IsDirty();
		if (bWasDirtyBefore)
		{
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: package for '%s' was already dirty before building its working mesh (pre-existing, not caused by this operation)."),
				*Mesh->GetName());
		}

		// The copy is local to this function; it is never stored on the entry and does not
		// outlive this call. Only the resulting FDynamicMesh3 is kept.
		TUniquePtr<FMeshDescription> MeshDescriptionCopy = CopyMeshDescription(SourceMeshDescription);
		TArray<FTriangleID> TriIDMap;
		if (MeshDescriptionCopy.IsValid())
		{
			WorkingMesh.Mesh = ConvertToDynamicMesh(*MeshDescriptionCopy, TriIDMap);
		}

		const bool bIsDirtyAfter = Package && Package->IsDirty();
		if (!bWasDirtyBefore && bIsDirtyAfter)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("Vertex Mask Forge: building the working mesh for '%s' unexpectedly marked its package as dirty."),
				*Mesh->GetName());
		}

		const bool bSourceHasVertices = SourceMeshDescription->Vertices().Num() > 0;
		if (!WorkingMesh.Mesh.IsValid() || (bSourceHasVertices && WorkingMesh.Mesh->VertexCount() == 0))
		{
			WorkingMesh.State = EVertexMaskForgeWorkingMeshState::ConversionFailed;
			return WorkingMesh;
		}

		ValidateWorkingMesh(WorkingMesh, *MeshDescriptionCopy, TriIDMap, Diagnostics);

		// AUDITED (Nanite source-topology support): persisted for the Accept commit path -- see
		// FVertexMaskForgeWorkingMesh::TriIDMap's own doc comment. Moved (not copied): TriIDMap is a
		// local variable, never read again after this point in this function.
		WorkingMesh.TriIDMap = MoveTemp(TriIDMap);

		// AUDITED (Nanite source-topology support): guarantee a Normal Overlay exists, then compute the
		// geometry fingerprint AFTER it -- both must happen exactly once, here, before Mesh is ever
		// handed to a generator or cache. See EnsureNormalOverlay/ComputeDynamicMeshGeometryFingerprint
		// for why (hard-edge AO correctness / cache identity robustness).
		EnsureNormalOverlay(*WorkingMesh.Mesh, Mesh->GetName());
		WorkingMesh.GeometryFingerprint = ComputeDynamicMeshGeometryFingerprint(*WorkingMesh.Mesh);

		WorkingMesh.State = EVertexMaskForgeWorkingMeshState::Ready;
		return WorkingMesh;
	}


	// --- Bounding Box Mask (Local X / Local Y / Local Z, each Local- or World-Space, Mirror) -----

	/** Selects the coordinate of P along Axis. */
	static float GetAxisCoordinate(const FVector3f& P, const EVertexMaskForgeBoundsAxis Axis)
	{
		switch (Axis)
		{
		case EVertexMaskForgeBoundsAxis::X:
			return P.X;
		case EVertexMaskForgeBoundsAxis::Y:
			return P.Y;
		case EVertexMaskForgeBoundsAxis::Z:
		default:
			return P.Z;
		}
	}

	/**
	 * The already-validated Local Z base-gradient formula (unchanged), now shared by every axis:
	 *   Lower = Position - SafeTransitionWidth * 0.5
	 *   Gradient = clamp((T - Lower) / SafeTransitionWidth, 0, 1)
	 * T is a normalized coordinate in [0,1] (see GenerateBoundingBoxMask); NOT Invert -- that is
	 * applied by the caller to this function's result. Mirror (also applied by the caller) does NOT
	 * call this function twice and take a maximum (that compressed the achievable range -- see the
	 * audit note at the Mirror call site); it instead remaps T itself into a symmetric "tent" domain
	 * BEFORE this single call, so this function's own formula and meaning are unchanged either way.
	 */
	static float EvaluateAxisBaseGradient(const float T, const float Position, const float SafeTransitionWidth)
	{
		const float Lower = Position - SafeTransitionWidth * 0.5f;
		return FMath::Clamp((T - Lower) / SafeTransitionWidth, 0.f, 1.f);
	}

#if !UE_BUILD_SHIPPING
	/**
	 * One-time runtime sanity check (non-shipping builds only) of the Mirror remap's symmetry, per
	 * the explicit checkpoint requirement: Mask(0)==Mask(1), Mask(0.25)==Mask(0.75), and the full
	 * 0-1 range is reached (Mask(0)==0, Mask(0.5)==1) at the representative default Position=0.5,
	 * TransitionWidth=1.0. Purely diagnostic (UE_LOG only); never affects composition or generation.
	 * Runs once (guarded by a static bool), the first time GenerateBoundingBoxMask processes an
	 * enabled Mirror axis.
	 */
	static void VerifyMirrorSymmetryOnce()
	{
		static bool bVerified = false;
		if (bVerified)
		{
			return;
		}
		bVerified = true;

		constexpr float Position = 0.5f;
		constexpr float TransitionWidth = 1.0f;
		const float SampleTs[5] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f };
		float Values[5];
		for (int32 i = 0; i < 5; ++i)
		{
			const float MirroredT = 1.f - FMath::Abs(2.f * SampleTs[i] - 1.f);
			Values[i] = EvaluateAxisBaseGradient(MirroredT, Position, TransitionWidth);
		}

		constexpr float Tolerance = 1e-4f;
		const bool bEdgesMatch = FMath::IsNearlyEqual(Values[0], Values[4], Tolerance);
		const bool bQuartersMatch = FMath::IsNearlyEqual(Values[1], Values[3], Tolerance);
		const bool bFullRange = FMath::IsNearlyEqual(Values[0], 0.f, Tolerance) && FMath::IsNearlyEqual(Values[2], 1.f, Tolerance);

		if (!bEdgesMatch || !bQuartersMatch || !bFullRange)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("Vertex Mask Forge: Mirror symmetry self-check FAILED -- Mask(0)=%.4f Mask(0.25)=%.4f Mask(0.5)=%.4f Mask(0.75)=%.4f Mask(1)=%.4f"),
				Values[0], Values[1], Values[2], Values[3], Values[4]);
		}
		else
		{
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: Mirror symmetry self-check passed -- Mask(0)=%.4f Mask(0.25)=%.4f Mask(0.5)=%.4f Mask(0.75)=%.4f Mask(1)=%.4f"),
				Values[0], Values[1], Values[2], Values[3], Values[4]);
		}
	}
#endif

	/** The Static Mesh's own unit local axis vector for Axis: (1,0,0) / (0,1,0) / (0,0,1). */
	static FVector GetLocalAxisUnitVector(const EVertexMaskForgeBoundsAxis Axis)
	{
		switch (Axis)
		{
		case EVertexMaskForgeBoundsAxis::X:
			return FVector(1.0, 0.0, 0.0);
		case EVertexMaskForgeBoundsAxis::Y:
			return FVector(0.0, 1.0, 0.0);
		case EVertexMaskForgeBoundsAxis::Z:
		default:
			return FVector(0.0, 0.0, 1.0);
		}
	}

	/**
	 * AUDITED (Unified Bounds + Local Space fix): for ONE enabled Local-space axis with more than
	 * one participating component, resolves a SHARED LOCAL AXIS -- the world-space direction and
	 * scale that every participant's own local Axis maps to -- so Unified Local can preserve
	 * translation between instances (see ResolveAxisCoordinate) while still tracking the meshes'
	 * shared orientation rather than the world's fixed XYZ.
	 *
	 * Compatibility is checked on the ACTUAL TRANSFORMED AXIS VECTOR
	 * (ParticipantTransforms[i].TransformVector(UnitAxis)) -- not on the whole Rotation/Scale
	 * generically -- so a mismatch on an axis nobody cares about (e.g. differing X scale) never
	 * blocks Unified Local on a different, actually-compatible axis (e.g. Z). Comparing the vectors
	 * directly also inherently rejects "approximately parallel but flipped" cases (a vector and its
	 * negation differ by 2x their length, always outside any reasonable tolerance), satisfying the
	 * "não aceite eixos... com sentido invertido incompatível" requirement without special-casing it.
	 *
	 * Deterministic regardless of selection order: every participant is compared against
	 * participant[0] purely as a symmetric equivalence check (if all equal the first, all are
	 * mutually equal by transitivity, so the SAME shared vector -- within tolerance -- results
	 * regardless of which participant happened to be first).
	 */
	static bool ResolveSharedLocalAxis(
		const TArray<FTransform>& ParticipantTransforms,
		const EVertexMaskForgeBoundsAxis Axis,
		const TCHAR* AxisName,
		FVector& OutSharedDirection,
		double& OutSharedScale,
		FText& OutErrorText)
	{
		check(ParticipantTransforms.Num() > 0);

		const FVector UnitAxis = GetLocalAxisUnitVector(Axis);
		const FVector ReferenceVector = ParticipantTransforms[0].TransformVector(UnitAxis);
		const double ReferenceLength = ReferenceVector.Size();

		if (!FMath::IsFinite(ReferenceLength) || ReferenceLength <= UE_DOUBLE_SMALL_NUMBER)
		{
			OutErrorText = FText::Format(
				LOCTEXT("UnifiedBoundsLocalDegenerateAxisFormat",
					"Unified Bounds: Local {0} axis has zero scale on one or more selected instances."),
				FText::FromString(AxisName));
			return false;
		}

		// Relative tolerance (proportional to the reference vector's own length) covers both
		// direction and magnitude (scale) in one comparison -- a vector differing in orientation OR
		// in scale from the reference both fail Equals() at this tolerance.
		constexpr double RelativeTolerance = 1e-3;
		const double AbsoluteTolerance = RelativeTolerance * ReferenceLength;

		for (int32 i = 1; i < ParticipantTransforms.Num(); ++i)
		{
			const FVector Vector = ParticipantTransforms[i].TransformVector(UnitAxis);
			if (!Vector.Equals(ReferenceVector, AbsoluteTolerance))
			{
				OutErrorText = FText::Format(
					LOCTEXT("UnifiedBoundsLocalIncompatibleFormat",
						"Unified Bounds requires compatible Local {0} axes. Enable World Space for {0}."),
					FText::FromString(AxisName));
				return false;
			}
		}

		OutSharedDirection = ReferenceVector.GetSafeNormal();
		OutSharedScale = ReferenceLength;
		return true;
	}

	/**
	 * THE single shared coordinate resolver, used identically by Phase A
	 * (ComputeCollectiveAxisBounds, collecting CollectiveMin/Max) and Phase B
	 * (GenerateBoundingBoxMask, evaluating every render vertex) -- never two separate
	 * implementations, per the explicit requirement that Phase A and Phase B must never operate in
	 * different spaces.
	 *
	 * AUDITED (root cause of Unified Local ignoring instance translation): previously, an enabled
	 * Local-space axis always read LocalPosition[Axis] directly, in BOTH Individual and Unified
	 * modes. LocalPosition is the Static Mesh ASSET's own object-space coordinate -- it has no
	 * notion of "where this component is placed" at all, so two instances of the SAME asset always
	 * produced the exact same raw local coordinate range regardless of how far apart they actually
	 * are in the level. Collecting/evaluating that way inevitably collapsed every instance onto the
	 * same [0, AssetExtent] range instead of composing their real relative placement.
	 *
	 * Four cases now:
	 *   - Individual Local (bWorldSpace=false, bUseUnifiedBounds=false): LocalPosition[Axis]
	 *     unchanged -- exactly preserves the already-validated single-mesh behavior; translation
	 *     between components never participates (there is no "between components" in this mode).
	 *   - Individual World / Unified World (bWorldSpace=true): WorldPosition[Axis], where
	 *     WorldPosition = ComponentTransform.TransformPosition(LocalPosition) -- the full affine
	 *     transform (translation+rotation+scale), unchanged from before.
	 *   - Unified Local (bWorldSpace=false, bUseUnifiedBounds=true): WorldPosition is still computed
	 *     (so translation between instances DOES participate), then projected onto the SHARED local
	 *     axis direction (SharedLocalAxisDirection, resolved once by ResolveSharedLocalAxis and
	 *     validated compatible across every participant) and divided by SharedLocalAxisScale. This
	 *     is what lets Local orientation stay meaningfully different from World orientation (a
	 *     rotated selection's "Local Z" still follows the meshes' own shared up-axis, not the
	 *     world's), while still composing translation between instances correctly.
	 * The absolute origin used for the dot product is arbitrary and does not need to be subtracted
	 * out: T = (Coord - Min) / (Max - Min) is invariant to any additive offset applied uniformly to
	 * every Coord, Min, and Max alike, so introducing an arbitrary pivot would add complexity without
	 * changing the result.
	 */
	static double ResolveAxisCoordinate(
		const FVector3f& LocalPosition,
		const FTransform& ComponentTransform,
		const EVertexMaskForgeBoundsAxis Axis,
		const bool bWorldSpace,
		const bool bUseUnifiedBounds,
		const FVector& SharedLocalAxisDirection,
		const double SharedLocalAxisScale)
	{
		if (!bWorldSpace && !bUseUnifiedBounds)
		{
			return static_cast<double>(GetAxisCoordinate(LocalPosition, Axis));
		}

		const FVector WorldPosition = ComponentTransform.TransformPosition(FVector(LocalPosition));

		if (bWorldSpace)
		{
			return static_cast<double>(GetAxisCoordinate(FVector3f(WorldPosition), Axis));
		}

		// Unified Local.
		return FVector::DotProduct(WorldPosition, SharedLocalAxisDirection) / SharedLocalAxisScale;
	}

	/**
	 * Generates the Bounding Box Mask directly in RENDER VERTEX order for one Static Mesh's LOD 0,
	 * evaluating up to 3 independent axes (AxisParams, indexed by EVertexMaskForgeBoundsAxis) and
	 * combining every ENABLED axis by maximum.
	 *
	 * AUDITED ARCHITECTURAL CORRECTION (render-vertex order): this mask is computed directly from
	 * LOD0.VertexBuffers.PositionVertexBuffer -- ONE value per RenderVertexIndex, guaranteeing
	 * Mask.Values.Num() == PositionVertexBuffer.GetNumVertices() exactly (enforced by construction
	 * below). Render vertices that share a position (a seam) each still get their own array slot,
	 * but since the mask value is a pure function of position, they necessarily compute to the same
	 * value -- consistent with baseline colors remaining independent per render vertex. No
	 * FDynamicMesh3, no position matching (BuildPositionBuckets/FindMatchingVertexID remain unused,
	 * reserved for a future topology-dependent generator).
	 *
	 * LOCAL vs WORLD SPACE (per axis, audited): for an axis with bWorldSpace == false, the
	 * evaluation position is LocalPosition (LOD0's own render-vertex position) unchanged. For
	 * bWorldSpace == true, EvaluationPosition = ComponentTransform.TransformPosition(LocalPosition)
	 * -- i.e. the FULL affine transform (translation, rotation, and uniform or non-uniform scale),
	 * never just a direction/vector transform, so translation is never dropped. ComponentTransform
	 * is the SPECIFIC previewed instance's transform passed in by the caller (FTransform::Identity
	 * for the entry-level Local-only reference evaluation used for gating/display -- see the audit
	 * note on FVertexMaskForgeWorkingMesh::BoundingBoxMask). Position/bounds are ALWAYS computed in
	 * the SAME axis's chosen space -- never local position against world bounds or vice versa.
	 *
	 * WORLD BOUNDS (audited): computed by transforming EVERY relevant render vertex and taking
	 * min/max of the transformed coordinate -- NEVER by transforming just the 8 corners of the local
	 * bounding box, which would be wrong for a rotated component (a rotated box's world-space AABB
	 * is not simply the transform of its local AABB corners' min/max in the general case here because
	 * we need the exact per-axis extent of the actual geometry, not an AABB-of-an-AABB approximation).
	 * The bounds pass and the value pass therefore both iterate all NumRenderVerts render vertices,
	 * per enabled axis.
	 *
	 * MIRROR / INVERT ORDER (audited, exact contract from the checkpoint spec): for axis A,
	 *   T = (Coordinate - BoundsMin) / (BoundsMax - BoundsMin)
	 *   BaseGradient = EvaluateAxisBaseGradient(T, Position, SafeTransitionWidth)
	 *   AxisMask = bMirror ? max(BaseGradient, EvaluateAxisBaseGradient(1-T, Position, SafeTransitionWidth)) : BaseGradient
	 *   if (bInvert) AxisMask = 1 - AxisMask
	 *   AxisMask = clamp(AxisMask, 0, 1)
	 * Invert is applied to the COMBINED (post-Mirror) result, never to BaseGradient and the mirrored
	 * gradient individually before the maximum -- inverting each side separately would change the
	 * operation mathematically (max(1-a,1-b) != 1-max(a,b) in general) and is explicitly prohibited.
	 *
	 * AXIS COMBINATION (audited): CombinedMask = max over every ENABLED axis's own AxisMask (0.0 if
	 * no axis is enabled, but callers must check bAnyAxisEnabled themselves BEFORE calling this --
	 * see the "Enable at least one Bounding Box axis" contract in OnGenerateBoundingBoxMaskClicked /
	 * RunAutoUpdatePreview -- this function still safely returns Unavailable if called with none).
	 *
	 * Never touches the Primary Color Overlay, MeshDescription, FDynamicMesh3, RenderData, the
	 * source asset, or ComponentTransform's owning component/actor -- only reads FPositionVertexBuffer
	 * positions (read-only) and ComponentTransform (read-only, by value) and writes into the
	 * returned FVertexMaskForgeScalarMask. Render-vertex order, one entry per render vertex, and
	 * seam independence are all preserved exactly as in the single-axis Local Z version.
	 */
	static FVertexMaskForgeScalarMask GenerateBoundingBoxMask(
		const FStaticMeshLODResources& LOD0,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const FTransform& ComponentTransform,
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBounds = nullptr)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::BoundingBox;
		Mask.UsedAxisParams = AxisParams;
		Mask.bUnifiedBounds = (CollectiveBounds != nullptr);

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		bool bAnyAxisEnabled = false;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled)
			{
				bAnyAxisEnabled = true;
				break;
			}
		}
		if (!bAnyAxisEnabled)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

#if !UE_BUILD_SHIPPING
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisParams[AxisIndex].bMirror)
			{
				VerifyMirrorSymmetryOnce();
				break;
			}
		}
#endif

		// Pass 1 (per enabled axis, in that axis's own chosen space): bounds over EVERY render
		// vertex -- the global bounding box of the whole LOD in that space, not per-piece, and never
		// approximated from just the local AABB corners for World Space (see the function doc).
		//
		// AUDITED (Unified Bounds): if CollectiveBounds is supplied (non-null), it was already fully
		// computed and validated by ComputeCollectiveAxisBounds() across every participating
		// component BEFORE this call -- skip this mesh's own individual bounds pass entirely and use
		// the shared collective domain instead. This is the ONLY difference between Individual and
		// Unified Bounds: everything from here on (normalization, Mirror, Invert, clamp, axis
		// combination, composition) is the exact same code path regardless of which bounds were used.
		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> IndividualBounds;
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>& AxisBounds = CollectiveBounds ? *CollectiveBounds : IndividualBounds;

		constexpr double MinExtent = 1e-5;

		if (!CollectiveBounds)
		{
			for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				if (!AxisParams[AxisIndex].bEnabled)
				{
					continue;
				}
				const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
				const bool bWorldSpace = AxisParams[AxisIndex].bWorldSpace;
				FVertexMaskForgeAxisBoundsResult& BoundsResult = IndividualBounds[AxisIndex];
				BoundsResult.MinCoord = TNumericLimits<double>::Max();
				BoundsResult.MaxCoord = TNumericLimits<double>::Lowest();

				for (int32 i = 0; i < NumRenderVerts; ++i)
				{
					const FVector3f LocalPosition = RenderPositions.VertexPosition(i);
					// bUseUnifiedBounds=false: Individual bounds -- Local reads LocalPosition[Axis]
					// directly (unchanged, single-mesh behavior), World reads WorldPosition[Axis].
					// SharedLocalAxisDirection/Scale are unused in this branch (Individual mode never
					// has a "shared" axis -- there is only one component).
					const double Coord = ResolveAxisCoordinate(
						LocalPosition, ComponentTransform, Axis, bWorldSpace, /*bUseUnifiedBounds=*/false,
						FVector::ZeroVector, 1.0);
					BoundsResult.MinCoord = FMath::Min(BoundsResult.MinCoord, Coord);
					BoundsResult.MaxCoord = FMath::Max(BoundsResult.MaxCoord, Coord);
				}

				const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
				if (!FMath::IsFinite(Extent) || Extent <= MinExtent)
				{
					BoundsResult.bDegenerate = true;
				}
			}
		}

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisBounds[AxisIndex].bDegenerate)
			{
				Mask.State = EVertexMaskForgeScalarMaskState::DegenerateBounds;
				return Mask;
			}
		}

		// Dense by construction: render vertex indices are already compact (0..NumRenderVerts-1), so
		// every slot is written below.
		Mask.Values.SetNumZeroed(NumRenderVerts);
		Mask.bHasValue.Init(true, NumRenderVerts);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;
		bool bAllFinite = true;
		bool bAllInRange = true;

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const FVector3f LocalPosition = RenderPositions.VertexPosition(i);

			// Combine every enabled axis's own AxisMask by maximum -- each axis fully completes its
			// own Local/World selection, bounds, normalization, Mirror, Invert, and clamp BEFORE
			// contributing to CombinedMask (see the function doc for the exact order).
			float CombinedMask = 0.f;
			for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				const FVertexMaskForgeAxisMaskParams& Params = AxisParams[AxisIndex];
				if (!Params.bEnabled)
				{
					continue;
				}
				const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
				const FVertexMaskForgeAxisBoundsResult& BoundsResult = AxisBounds[AxisIndex];

				// AUDITED (Unified Bounds + Local Space fix): SAME resolver as Phase A
				// (ComputeCollectiveAxisBounds) and as Phase 1 just above -- bUseUnifiedBounds mirrors
				// whether CollectiveBounds was supplied to this call at all (Unified Local needs the
				// SharedLocalAxisDirection/Scale that Phase A already resolved and stored on
				// BoundsResult; Individual Local/either World branch ignore them).
				const double Coord = ResolveAxisCoordinate(
					LocalPosition, ComponentTransform, Axis, Params.bWorldSpace, /*bUseUnifiedBounds=*/CollectiveBounds != nullptr,
					BoundsResult.SharedLocalAxisDirection, BoundsResult.SharedLocalAxisScale);

				const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
				const float T = static_cast<float>((Coord - BoundsResult.MinCoord) / Extent);

				// Epsilon guard against a zero (or near-zero) Transition Width, per the checkpoint spec.
				const float SafeTransitionWidth = FMath::Max(Params.TransitionWidth, 1e-4f);

				// AUDITED (Mirror normalization fix): the original formula --
				// max(EvaluateAxisBaseGradient(T,...), EvaluateAxisBaseGradient(1-T,...)) -- evaluates
				// EvaluateAxisBaseGradient() TWICE, each still across the FULL [0,1] domain, then takes
				// their maximum. Since EvaluateAxisBaseGradient() is monotonically non-decreasing in
				// its first argument, for any T the larger of {T, 1-T} is always >= 0.5, so the
				// maximum's own MINIMUM (at T=0.5, where both arguments equal 0.5) is
				// EvaluateAxisBaseGradient(0.5, Position, Width) -- 0.5 with the default Position=0.5/
				// Width=1.0 -- and it never goes lower. The whole result is therefore compressed into
				// [EvaluateAxisBaseGradient(0.5,...), 1], never reaching the low end of 0-1 the way the
				// non-Mirror gradient does.
				//
				// Fix: remap T itself into a "tent" domain that ALREADY spans the full 0-1 range
				// within EACH half, then evaluate EvaluateAxisBaseGradient() exactly ONCE on that
				// remapped coordinate (not twice, not maxed) -- MirroredT = 1 - |2T - 1|. MirroredT is
				// 0 at T=0 and T=1 (the two edges), rises to 1 at T=0.5 (the center), and is exactly
				// symmetric about T=0.5 by construction, so:
				//   - Mask(0) == Mask(1) (both use MirroredT=0);
				//   - Mask(0.25) == Mask(0.75) (both use MirroredT=0.5);
				//   - each half traverses the FULL 0-1 range of EvaluateAxisBaseGradient(), matching
				//     the non-Mirror gradient's own amplitude exactly, just folded at the center;
				//   - continuous at the center (MirroredT peaks smoothly at T=0.5, no value jump).
				// Position/TransitionWidth keep their existing meaning: they still shape a single
				// EvaluateAxisBaseGradient() curve, now over the tent-shaped domain instead of T
				// directly -- Position shifts where the transition sits between center and edge,
				// TransitionWidth still controls how sharp that transition is.
				const float EvaluationT = Params.bMirror ? (1.f - FMath::Abs(2.f * T - 1.f)) : T;
				float AxisMask = EvaluateAxisBaseGradient(EvaluationT, Params.Position, SafeTransitionWidth);

				if (Params.bInvert)
				{
					AxisMask = 1.f - AxisMask;
				}
				AxisMask = FMath::Clamp(AxisMask, 0.f, 1.f);

				CombinedMask = FMath::Max(CombinedMask, AxisMask);
			}

			if (!FMath::IsFinite(CombinedMask))
			{
				bAllFinite = false;
			}
			if (CombinedMask < 0.f || CombinedMask > 1.f)
			{
				bAllInRange = false;
			}

			Mask.Values[i] = CombinedMask;

			Sum += CombinedMask;
			MinValue = FMath::Min(MinValue, CombinedMask);
			MaxValue = FMath::Max(MaxValue, CombinedMask);

			if (FMath::IsNearlyZero(CombinedMask, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(CombinedMask, 1.f, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearOne;
			}
		}

		Mask.NumValidValues = NumRenderVerts;
		Mask.MinValue = MinValue;
		Mask.MaxValue = MaxValue;
		Mask.MeanValue = static_cast<float>(Sum / NumRenderVerts);
		Mask.NumNearZero = NumNearZero;
		Mask.NumNearOne = NumNearOne;

		// Integrity checks: never silently hide inconsistent output. The mandatory invariant
		// (Mask.Values.Num() == PositionVertexBuffer.GetNumVertices()) is enforced by construction
		// above (dense SetNumZeroed(NumRenderVerts)); NumValidValues == RenderVertexCount is checked
		// explicitly here as well so a future edit that reintroduces sparsity is caught immediately.
		if (!bAllFinite || !bAllInRange || Mask.NumValidValues != NumRenderVerts || Mask.Values.Num() != NumRenderVerts)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Invalid;
			return Mask;
		}

		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}

	/**
	 * Phase A of the Unified Bounds two-phase pipeline: computes the collective per-axis domain
	 * across every PARTICIPATING component -- every FVertexMaskForgePreviewComponentState with a
	 * live SourceComponent, belonging to a participating SelectedMeshes entry. Fully validates
	 * (compatibility, finiteness, degeneracy) BEFORE returning true -- callers must not touch any
	 * Preview until this returns true, per the two-phase contract. Never approximates: bounds are
	 * accumulated from the SAME real render-vertex positions (LOD0.VertexBuffers.PositionVertexBuffer)
	 * that the evaluation phase and the Accept path both read -- no ComponentBounds, no pivot, no
	 * Actor origin, no position matching.
	 *
	 * bForGeneration selects which entries count as "participating", since this function is shared
	 * by two different moments in the pipeline:
	 *   - true (OnGenerateBoundingBoxMaskClicked / RunAutoUpdatePreview, BEFORE this click/tick's
	 *     regeneration has written anything): an entry participates if its WorkingMesh itself is
	 *     Ready -- its CURRENT BoundingBoxMask may still be NotGenerated/stale, since generation is
	 *     what is about to (re)populate it.
	 *   - false (UpdateAllPreviews / BuildAcceptTargets, AFTER generation already ran): an entry
	 *     participates only if it is actually showing a Ready, Source == BoundingBox mask -- Content-
	 *     Browser-only or Fill-sourced entries never participate here.
	 */
	static bool ComputeCollectiveAxisBounds(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const bool bForGeneration,
		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>& OutBounds,
		FText& OutErrorText)
	{
		OutBounds = TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>();

		struct FParticipant
		{
			const FStaticMeshLODResources* LOD0 = nullptr;
			FTransform Transform;
		};
		TArray<FParticipant> Participants;

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			const bool bParticipates = bForGeneration
				? (Entry->WorkingMesh.State == EVertexMaskForgeWorkingMeshState::Ready)
				: (Entry->WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::BoundingBox
					&& Entry->WorkingMesh.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready);
			if (!bParticipates)
			{
				continue;
			}

			const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
			{
				continue;
			}
			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				continue;
			}

			for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
			{
				const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}

				FParticipant P;
				P.LOD0 = &RenderData->LODResources[0];
				P.Transform = SourceComponent->GetComponentTransform();
				Participants.Add(P);
			}
		}

		if (Participants.IsEmpty())
		{
			OutErrorText = LOCTEXT("UnifiedBoundsNoParticipants", "Unified Bounds: no eligible components to collect a collective domain from.");
			return false;
		}

		static const TCHAR* AxisNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
		constexpr double MinExtent = 1e-5;

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (!AxisParams[AxisIndex].bEnabled)
			{
				continue;
			}
			const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
			const bool bWorldSpace = AxisParams[AxisIndex].bWorldSpace;

			FVertexMaskForgeAxisBoundsResult& BoundsResult = OutBounds[AxisIndex];
			BoundsResult.MinCoord = TNumericLimits<double>::Max();
			BoundsResult.MaxCoord = TNumericLimits<double>::Lowest();

			// AUDITED (Unified Bounds + Local Space fix): resolve the SHARED local axis (direction +
			// scale) ONCE per axis here in Phase A, store it on BoundsResult, and use it verbatim in
			// Phase B (GenerateBoundingBoxMask) via the same ResolveAxisCoordinate() call -- Phase A
			// and Phase B must never operate in different spaces.
			if (!bWorldSpace)
			{
				TArray<FTransform> ParticipantTransforms;
				ParticipantTransforms.Reserve(Participants.Num());
				for (const FParticipant& P : Participants)
				{
					ParticipantTransforms.Add(P.Transform);
				}
				if (!ResolveSharedLocalAxis(
					ParticipantTransforms, Axis, AxisNames[AxisIndex],
					BoundsResult.SharedLocalAxisDirection, BoundsResult.SharedLocalAxisScale, OutErrorText))
				{
					return false;
				}
			}

			for (const FParticipant& P : Participants)
			{
				const FPositionVertexBuffer& RenderPositions = P.LOD0->VertexBuffers.PositionVertexBuffer;
				const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
				for (int32 i = 0; i < NumRenderVerts; ++i)
				{
					const FVector3f LocalPosition = RenderPositions.VertexPosition(i);
					const double Coord = ResolveAxisCoordinate(
						LocalPosition, P.Transform, Axis, bWorldSpace, /*bUseUnifiedBounds=*/true,
						BoundsResult.SharedLocalAxisDirection, BoundsResult.SharedLocalAxisScale);
					BoundsResult.MinCoord = FMath::Min(BoundsResult.MinCoord, Coord);
					BoundsResult.MaxCoord = FMath::Max(BoundsResult.MaxCoord, Coord);
				}
			}

			const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
			if (!FMath::IsFinite(Extent) || Extent <= MinExtent)
			{
				BoundsResult.bDegenerate = true;
			}
		}

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && OutBounds[AxisIndex].bDegenerate)
			{
				OutErrorText = FText::Format(
					LOCTEXT("UnifiedBoundsDegenerateFormat",
						"Unified Bounds: the collective {0} extent across the selection is too small to normalize safely."),
					FText::FromString(AxisNames[AxisIndex]));
				return false;
			}
		}

		return true;
	}

	/**
	 * Generates a dense, constant-valued mask directly in RENDER VERTEX order for one Static Mesh's
	 * LOD 0 -- the Fill White / Fill Black utility (Source distinguishes which for UI labeling).
	 * Same domain and the same mandatory invariant as GenerateBoundingBoxMask (Values.Num() ==
	 * bHasValue.Num() == PositionVertexBuffer.GetNumVertices(), every slot written, dense by
	 * construction) -- but every value is simply ConstantValue: no per-vertex computation, no
	 * FDynamicMesh3, no position matching, no ComponentTransform dependency (a constant is the same
	 * in every space), so it feeds the exact same downstream composition (UpdateWorkingColors) and
	 * Accept path (BuildAcceptTargets/WriteAcceptTargets) as the Bounding Box Mask, with no parallel
	 * code path.
	 */
	static FVertexMaskForgeScalarMask GenerateConstantMask(
		const FStaticMeshLODResources& LOD0,
		const float ConstantValue,
		const EVertexMaskForgeScalarMaskSource Source)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = Source;

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		Mask.Values.Init(ConstantValue, NumRenderVerts);
		Mask.bHasValue.Init(true, NumRenderVerts);

		Mask.NumValidValues = NumRenderVerts;
		Mask.MinValue = ConstantValue;
		Mask.MaxValue = ConstantValue;
		Mask.MeanValue = ConstantValue;
		Mask.NumNearZero = FMath::IsNearlyZero(ConstantValue, FVertexMaskForgeScalarMask::Tolerance) ? NumRenderVerts : 0;
		Mask.NumNearOne = FMath::IsNearlyEqual(ConstantValue, 1.f, FVertexMaskForgeScalarMask::Tolerance) ? NumRenderVerts : 0;

		// Invariant, enforced by construction: Values.Num() == bHasValue.Num() == NumRenderVerts.
		Mask.State = (Mask.Values.Num() == NumRenderVerts && Mask.bHasValue.Num() == NumRenderVerts)
			? EVertexMaskForgeScalarMaskState::Ready
			: EVertexMaskForgeScalarMaskState::Invalid;
		return Mask;
	}

	/** Sibling of GenerateConstantMask for Source-Topology entries -- indexed by CORNER INDEX (see
	 *  UpdateWorkingColorsSourceTopology's own doc comment for the domain), constant everywhere, so
	 *  NumCorners is simply 3 * WorkingMesh.Mesh->TriangleCount(). */
	static FVertexMaskForgeScalarMask GenerateConstantMaskForCornerDomain(
		const int32 NumCorners,
		const float ConstantValue,
		const EVertexMaskForgeScalarMaskSource Source)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = Source;
		Mask.RenderVertexCount = NumCorners;

		if (NumCorners <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		Mask.Values.Init(ConstantValue, NumCorners);
		Mask.bHasValue.Init(true, NumCorners);

		Mask.NumValidValues = NumCorners;
		Mask.MinValue = ConstantValue;
		Mask.MaxValue = ConstantValue;
		Mask.MeanValue = ConstantValue;
		Mask.NumNearZero = FMath::IsNearlyZero(ConstantValue, FVertexMaskForgeScalarMask::Tolerance) ? NumCorners : 0;
		Mask.NumNearOne = FMath::IsNearlyEqual(ConstantValue, 1.f, FVertexMaskForgeScalarMask::Tolerance) ? NumCorners : 0;

		Mask.State = (Mask.Values.Num() == NumCorners && Mask.bHasValue.Num() == NumCorners)
			? EVertexMaskForgeScalarMaskState::Ready
			: EVertexMaskForgeScalarMaskState::Invalid;
		return Mask;
	}

	// --- Ambient Occlusion Mask ---------------------------------------------------------------

	/** Auto Update Preview never raycasts at full Samples -- see GenerateAmbientOcclusionMask's own
	 *  doc comment (EFFECTIVE SAMPLES). An explicit Generate Mask click always uses the full,
	 *  user-chosen Samples value; only the interactive/live-preview path is capped. */
	static constexpr int32 AOAutoUpdateMaxSamples = 16;

	/**
	 * Cheap, geometry-cache-FREE structural validation for the Ambient Occlusion slot's ENTRY-LEVEL
	 * gating (see FVertexMaskForgeWorkingMesh::AmbientOcclusionMask's own doc comment) -- mirrors
	 * GenerateAmbientOcclusionMask's own early-exit checks (NumRenderVerts>0, at least one triangle,
	 * normal buffer count matches) WITHOUT touching FVertexMaskForgeAOCache, building a WorldMesh/Tree,
	 * or running a single raycast. AUDITED (double-AO-execution fix): this is what lets
	 * OnGenerateBoundingBoxMaskClicked/RunAutoUpdatePreview determine Ready/Unavailable for the AO slot
	 * WITHOUT ever calling GenerateAmbientOcclusionMask at entry level -- the ONLY call site that ever
	 * performs the real (expensive) computation is ApplyPreviewToEntry, exactly once per component, per
	 * update. See ApplyPreviewToEntry's own doc comment for the full audit of the bug this replaced.
	 */
	static bool IsAmbientOcclusionInputValid(const FStaticMeshLODResources& LOD0)
	{
		const int32 NumRenderVerts = static_cast<int32>(LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices());
		return NumRenderVerts > 0
			&& LOD0.IndexBuffer.GetNumIndices() >= 3
			&& static_cast<int32>(LOD0.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices()) == NumRenderVerts;
	}

	/** Sibling of IsAmbientOcclusionInputValid for Source-Topology (Nanite) entries -- validates
	 *  WorkingMesh.Mesh (the SOURCE-topology FDynamicMesh3) instead of LOD0 render buffers. AUDITED
	 *  (index-safety correction): never requires compactness (see
	 *  GenerateAmbientOcclusionMaskFromDynamicMesh's own INDEX SAFETY note); checks for a usable Normal
	 *  Overlay instead, since that is what AO in this domain actually reads (EnsureNormalOverlay
	 *  guarantees one exists for any Ready working mesh, so this only fails for a genuinely empty mesh). */
	static bool IsAmbientOcclusionInputValidForDynamicMesh(const UE::Geometry::FDynamicMesh3* Mesh)
	{
		return Mesh != nullptr
			&& Mesh->VertexCount() > 0 && Mesh->TriangleCount() > 0
			&& Mesh->HasAttributes() && Mesh->Attributes()->PrimaryNormals() != nullptr
			&& Mesh->Attributes()->PrimaryNormals()->ElementCount() > 0;
	}

	/**
	 * Deterministic, COSINE-WEIGHTED hemisphere sample directions (Z-up local tangent frame: X/Y span
	 * the hemisphere's base, Z is the pole/normal direction), via Malley's method (uniform disk sample
	 * -> project to hemisphere) combined with a golden-angle Fibonacci spiral for the disk sample
	 * itself -- no random number generator, no seed, no dependency on evaluation order, so
	 * regenerating with the same Samples count always reproduces the EXACT same directions (Test D's
	 * "regenerate with equal parameters produces identical results" requirement).
	 *
	 * AUDITED (sampling-quality fix, re-examined per explicit correction): previously UNIFORM over the
	 * hemisphere (Z = 1 - T); now COSINE-WEIGHTED (R = sqrt(T), Z = sqrt(1 - T)), which concentrates
	 * more samples near the normal direction and fewer at grazing angles -- physically appropriate for
	 * AO (a grazing-angle occluder contributes less to perceived occlusion than one near the normal),
	 * and reduces the visible noise/banding a uniform distribution produced at low Samples counts.
	 * Computed ONCE per GenerateAmbientOcclusionMask call (not once per render vertex); reoriented AND
	 * rotated per-vertex by the caller -- see ComputeDeterministicScrambleAngle and the caller's own
	 * SAMPLING doc note for why a per-vertex azimuthal rotation is applied on top of this fixed set.
	 */
	static TArray<FVector> BuildHemisphereSampleDirections(const int32 NumSamples)
	{
		TArray<FVector> Directions;
		Directions.Reserve(NumSamples);

		const double GoldenAngle = PI * (3.0 - FMath::Sqrt(5.0));
		for (int32 i = 0; i < NumSamples; ++i)
		{
			const double U1 = (static_cast<double>(i) + 0.5) / static_cast<double>(NumSamples);
			const double R = FMath::Sqrt(U1);
			const double Theta = GoldenAngle * static_cast<double>(i);
			const double Z = FMath::Sqrt(FMath::Max(0.0, 1.0 - U1));
			Directions.Add(FVector(R * FMath::Cos(Theta), R * FMath::Sin(Theta), Z));
		}
		return Directions;
	}

	/**
	 * Deterministic azimuthal scramble angle (radians, [0, 2*PI)) for one render vertex's sample set,
	 * derived from a QUANTIZED WORLD POSITION key -- the same "quantize to a grid, hash the cell"
	 * idiom BuildPositionBuckets already uses elsewhere in this file for stable geometric grouping,
	 * applied here for a different purpose (a per-position pseudo-random-but-deterministic rotation,
	 * not vertex matching).
	 *
	 * AUDITED (sampling-quality fix): deliberately keyed by POSITION, never by render vertex INDEX --
	 * FVector::FindBestAxisVectors picks a tangent basis that can discontinuously "flip" between
	 * neighboring vertices with only slightly different normals, which combined with every vertex
	 * reusing the IDENTICAL, unrotated sample set previously produced visible coherent banding /
	 * structured patterns across the surface (the artifact reported in manual testing). Rotating each
	 * vertex's samples by a position-derived angle breaks that coherence. Keying by POSITION (not
	 * index) is what keeps hard-edge/UV-seam-coincident render vertices (same position, different
	 * render vertex IDs) receiving the SAME scramble, so a seam never introduces an artificial
	 * discontinuity in the noise pattern that isn't actually present in the underlying geometry --
	 * exactly the requirement that seam-splitting must never change AO behavior (see the function's
	 * SELF-HIT doc note for the same principle applied to occlusion itself).
	 */
	static float ComputeDeterministicScrambleAngle(const FVector& WorldPos)
	{
		constexpr double QuantizeScale = 1000.0; // 1/1000 Unreal unit grid.
		const FIntVector Key(
			FMath::RoundToInt(WorldPos.X * QuantizeScale),
			FMath::RoundToInt(WorldPos.Y * QuantizeScale),
			FMath::RoundToInt(WorldPos.Z * QuantizeScale));
		const uint32 Hash = HashCombine(HashCombine(GetTypeHash(Key.X), GetTypeHash(Key.Y)), GetTypeHash(Key.Z));
		return (static_cast<float>(Hash) / static_cast<float>(MAX_uint32)) * 2.0f * UE_PI;
	}

	/**
	 * AUDITED (AO Levels + vanilla inversion checkpoint): the ONE shared, pure post-processing step
	 * both GenerateAmbientOcclusionMask (render-vertex) and GenerateAmbientOcclusionMaskFromDynamicMesh
	 * (Source-Topology) call to turn a single cached RawAO sample into the final composed value --
	 * never duplicated, never diverges between the two domains. Does no geometry, no raycasts, no cache
	 * access; a pure scalar transform, safe to call every recomposition regardless of Auto Update
	 * Preview, cache hit/miss, or which domain called it.
	 *
	 * PIPELINE (confirmed against the existing composition order before writing this; see the
	 * checkpoint report for the full confirmation):
	 *   1. RawAO: the raw hemisphere-occlusion fraction from AOCache.RawValues (or
	 *      FVertexMaskForgeSourceTopologyAOCache.RawValues) -- convention unchanged: 0 = exposed
	 *      (no occluders), 1 = fully occluded/cavity.
	 *   2. BaseAO = 1 - RawAO -- the NEW vanilla inversion (checkpoint requirement): baked
	 *      unconditionally into the AO layer's own interpretation, independent of the user-facing
	 *      Invert checkbox, which keeps its default (false/unchecked) meaning and serialized value.
	 *      With Invert left OFF, BaseAO is now exactly what previously required Invert ON to see.
	 *   3. LevelsMin/LevelsMax: saturate((BaseAO - LevelsMin) / max(LevelsMax - LevelsMin, Epsilon)) --
	 *      a standard black/white-point remap over BaseAO. Defaults (Min=0, Max=1) make this an exact
	 *      no-op (Denom=1, numerator unchanged, saturate is a no-op since BaseAO is already in [0,1]),
	 *      so existing sessions/serialized state that predate this field (defaulting to 0/1) are
	 *      visually unaffected -- see the checkpoint report.
	 *   4. User Invert (bInvert): applied LAST, over the ALREADY-leveled result -- FinalAO = bInvert ?
	 *      (1 - LeveledAO) : LeveledAO. This is what makes Invert "invert the new vanilla result" rather
	 *      than "toggle between old and new vanilla" -- the vanilla flip in step 2 is unconditional and
	 *      structural, not something Invert ever cancels back out to the OLD pre-checkpoint behavior.
	 *
	 * DIVIDE-BY-ZERO / NaN SAFETY: LevelsMax <= LevelsMin (including exactly equal) is handled by
	 * clamping the denominator to a small Epsilon rather than rejecting the input or clamping
	 * LevelsMin/LevelsMax themselves -- the UI keeps showing exactly what the user set (never silently
	 * snapped), and the composed result becomes a hard step (everything at or above LevelsMin reads as
	 * white) instead of NaN/Inf/undefined -- a deterministic, artist-legible degenerate case rather than
	 * a crash or a silently-wrong value.
	 */
	static float ApplyAOLevelsAndInvert(const float RawAO, const float LevelsMin, const float LevelsMax, const bool bInvert)
	{
		constexpr float Epsilon = 1e-4f;

		const float BaseAO = 1.0f - RawAO;

		const float Denom = FMath::Max(LevelsMax - LevelsMin, Epsilon);
		const float LeveledAO = FMath::Clamp((BaseAO - LevelsMin) / Denom, 0.0f, 1.0f);

		return bInvert ? (1.0f - LeveledAO) : LeveledAO;
	}

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
	 * EFFECTIVE SAMPLES (AUDITED, interactive-performance fix): the caller (OnGenerateBoundingBoxMaskClicked
	 * / RunAutoUpdatePreview) is responsible for capping Params.Samples to AOAutoUpdateMaxSamples for
	 * an AUTO UPDATE-triggered call, and passing the full, user-chosen Samples value for an explicit
	 * GENERATE MASK click -- this function itself always raycasts at whatever Params.Samples it is
	 * given (it has no notion of "interactive vs final" on its own) and reports the ACTUAL value used
	 * in Mask.UsedAOParams.Samples (diagnostic-accurate, matching UsedAxisParams' own contract). This
	 * is what makes Auto Update responsive without ever silently overwriting the user's chosen Samples
	 * UI value -- AOSamples itself is never mutated by Auto Update, only what gets PASSED for that one
	 * call. A switch between interactive and final quality is a genuine parameter change from this
	 * function's point of view, so the RawValues cache correctly treats it as one (Layer 2 rebuilds).
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
	static FVertexMaskForgeScalarMask GenerateAmbientOcclusionMask(
		TUniquePtr<FVertexMaskForgeAOCache>& CachePtr,
		const UStaticMesh* Mesh,
		const FStaticMeshLODResources& LOD0,
		const FTransform& ComponentTransform,
		const FVertexMaskForgeAOParams& RawParams)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::AmbientOcclusion;

		// Never trust the UI clamp alone (same defensive posture as every other generator's inputs).
		FVertexMaskForgeAOParams Params = RawParams;
		Params.Samples = FMath::Clamp(Params.Samples, 8, 256);
		Params.MaxDistance = FMath::Clamp(Params.MaxDistance, 0.01f, 10000.0f);
		Params.Bias = FMath::Clamp(Params.Bias, 0.001f, 10.0f);
		// AUDITED (AO Levels): clamped independently to [0,1] each -- LevelsMax <= LevelsMin is a valid,
		// deterministic degenerate case (see ApplyAOLevelsAndInvert's own doc comment), not something to
		// reorder or reject here.
		Params.LevelsMin = FMath::Clamp(Params.LevelsMin, 0.0f, 1.0f);
		Params.LevelsMax = FMath::Clamp(Params.LevelsMax, 0.0f, 1.0f);
		Mask.UsedAOParams = Params;

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& RenderTangents = LOD0.VertexBuffers.StaticMeshVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0
			|| LOD0.IndexBuffer.GetNumIndices() < 3
			|| static_cast<int32>(RenderTangents.GetNumVertices()) != NumRenderVerts)
		{
			// No geometry, no triangles, or a normal buffer that doesn't match the render vertex
			// count (partial/invalid, same treatment as a mismatched Color Vertex Buffer elsewhere in
			// this file) -- never guessed or index-clamped.
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		if (!CachePtr.IsValid())
		{
			CachePtr = MakeUnique<FVertexMaskForgeAOCache>();
		}
		FVertexMaskForgeAOCache& Cache = *CachePtr;

		const double GenerationStartSeconds = FPlatformTime::Seconds();

		// Layer 1: occluder geometry + spatial index. See the function's own CACHE doc comment for
		// exactly what each comparison catches.
		//
		// AUDITED (DerivedDataKey false-mismatch fix, per explicit root-cause confirmation): the key
		// comparison is now a PLAIN equality (FString::operator== already treats "both empty" as
		// equal, which is the mathematically correct answer -- two unknown/unavailable keys are not
		// evidence of a change). The PREVIOUS policy additionally required the CURRENT key to be
		// non-empty, which meant any asset whose LOD0.DerivedDataKey is never populated (confirmed by
		// the checkpoint's own diagnostic logs: oldEmpty=true, newEmpty=true, equal=true, logged as a
		// "mismatch" every single call) could NEVER hit the Tree cache -- a permanent, silent full
		// rebuild on every call, not a real geometry change. The corrected policy, exactly as
		// specified:
		//   - old empty + new empty            -> compatible (this is the case that was broken)
		//   - old non-empty + new non-empty, == -> compatible
		//   - one empty, other non-empty        -> incompatible (a key appeared or disappeared)
		//   - both non-empty, different         -> incompatible (real content change)
		// DerivedDataKey is therefore an OPTIONAL, best-effort field within the cache key -- never an
		// absolute veto. Safety against reimport/rebuild does NOT depend on this field alone even when
		// it is unavailable: Mesh identity, NumRenderVerts, NumIndices, and Transform are independent,
		// already-audited fields in the SAME AND-chain (see below) -- a reimport that changes vertex/
		// triangle counts is still caught by bVertCountMatches/bIndexCountMatches regardless of
		// DerivedDataKey's availability. A reimport that preserves both counts AND leaves
		// DerivedDataKey empty is a known, accepted residual gap for that specific (rare) asset
		// category -- not solvable by any stable identifier already available on this code path
		// without inventing a new one, which was explicitly out of scope for this fix.
		const int32 NumIndices = LOD0.IndexBuffer.GetNumIndices();
		const bool bMeshMatches = Cache.CachedMesh.Get() == Mesh;
		const bool bKeyMatches = Cache.CachedDerivedDataKey == LOD0.DerivedDataKey;
		const bool bVertCountMatches = Cache.CachedNumRenderVerts == NumRenderVerts;
		const bool bIndexCountMatches = Cache.CachedNumIndices == NumIndices;
		const bool bTransformMatches = Cache.CachedTransform.Equals(ComponentTransform, 1e-5);
		const bool bTreeStillValid = Cache.bTreeValid
			&& bMeshMatches
			&& bKeyMatches
			&& bVertCountMatches
			&& bIndexCountMatches
			&& bTransformMatches;

		// Fires ONLY on a genuine miss -- Cache.bTreeValid was already true (a Tree existed from a
		// PREVIOUS call) but at least one field no longer matches. Never fires on the legitimate first
		// build of a session. One compact Verbose line, no addresses/hashes/full transform dumps.
		if (Cache.bTreeValid && !bTreeStillValid)
		{
			TArray<FString> Reasons;
			if (!bMeshMatches) { Reasons.Add(TEXT("Mesh changed")); }
			if (!bKeyMatches) { Reasons.Add(TEXT("DerivedDataKey changed")); }
			if (!bVertCountMatches || !bIndexCountMatches) { Reasons.Add(TEXT("Vertex/index count changed")); }
			if (!bTransformMatches) { Reasons.Add(TEXT("Transform changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO Tree cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}

		if (!bTreeStillValid)
		{
			Cache.WorldMesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
			Cache.WorldMesh->EnableTriangleGroups();

			for (int32 i = 0; i < NumRenderVerts; ++i)
			{
				const FVector WorldPos = ComponentTransform.TransformPosition(FVector(RenderPositions.VertexPosition(i)));
				Cache.WorldMesh->AppendVertex(WorldPos);
			}

			const int32 NumTriangles = NumIndices / 3;
			for (int32 TriIndex = 0; TriIndex < NumTriangles; ++TriIndex)
			{
				const int32 I0 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 0));
				const int32 I1 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 1));
				const int32 I2 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 2));
				if (I0 == I1 || I1 == I2 || I0 == I2)
				{
					continue; // Degenerate triangle (zero area): never fed to the occluder tree.
				}
				Cache.WorldMesh->AppendTriangle(I0, I1, I2);
			}

			Cache.Tree = MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(Cache.WorldMesh.Get());
			Cache.CachedMesh = Mesh;
			Cache.CachedDerivedDataKey = LOD0.DerivedDataKey;
			Cache.CachedTransform = ComponentTransform;
			Cache.CachedNumRenderVerts = NumRenderVerts;
			Cache.CachedNumIndices = NumIndices;
			Cache.bTreeValid = true;
			Cache.bValuesValid = false; // Geometry changed: any previously-cached raw values are stale.
		}

		// Layer 2: raw occlusion fraction per render vertex. Rebuilt only if Samples/MaxDistance/Bias
		// actually changed (or Layer 1 was just rebuilt above).
		const bool bSamplesMatch = Cache.CachedSamples == Params.Samples;
		const bool bMaxDistanceMatches = FMath::IsNearlyEqual(Cache.CachedMaxDistance, Params.MaxDistance, 1e-4f);
		const bool bBiasMatches = FMath::IsNearlyEqual(Cache.CachedBias, Params.Bias, 1e-6f);
		const bool bValuesStillValid = Cache.bValuesValid && bSamplesMatch && bMaxDistanceMatches && bBiasMatches;

		// Fires only on a genuine miss where the Tree survived (if the Tree itself was just rebuilt
		// above, RawValues are correctly and unconditionally invalidated too -- not a separate,
		// unexplained miss, so it is not logged again here).
		if (bTreeStillValid && Cache.bValuesValid && !bValuesStillValid)
		{
			TArray<FString> Reasons;
			if (!bSamplesMatch) { Reasons.Add(FString::Printf(TEXT("Samples %d -> %d"), Cache.CachedSamples, Params.Samples)); }
			if (!bMaxDistanceMatches) { Reasons.Add(TEXT("MaxDistance changed")); }
			if (!bBiasMatches) { Reasons.Add(TEXT("Bias changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO RawValues cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}
		if (!bValuesStillValid)
		{
			const TArray<FVector> LocalSampleDirs = BuildHemisphereSampleDirections(Params.Samples);
			const UE::Geometry::FDynamicMesh3& WorldMesh = *Cache.WorldMesh;
			const UE::Geometry::FDynamicMeshAABBTree3& Tree = *Cache.Tree;
			const FMatrix NormalMatrix = ComponentTransform.ToMatrixWithScale().Inverse().GetTransposed();

			Cache.RawValues.SetNumUninitialized(NumRenderVerts);

			UE::Geometry::IMeshSpatial::FQueryOptions Options;
			Options.MaxDistance = Params.MaxDistance;

			// AUDITED (parallelization): see the function's own PARALLELIZATION doc comment for the
			// source-level confirmation this is safe. Tree/WorldMesh/RenderTangents/NormalMatrix/
			// LocalSampleDirs/Options are read-only; each worker writes only Cache.RawValues[i].
			ParallelFor(NumRenderVerts, [&](const int32 i)
			{
				const FVector WorldPos = WorldMesh.GetVertex(i);

				const FVector4f LocalNormal4 = RenderTangents.VertexTangentZ(i);
				FVector WorldNormal = NormalMatrix.TransformVector(FVector(LocalNormal4));
				if (!WorldNormal.Normalize())
				{
					// Degenerate render normal (pathological content only): fall back to this
					// vertex's own first incident triangle's geometric normal, never a guess.
					WorldNormal = FVector::UpVector;
					for (const int32 TriID : WorldMesh.VtxTrianglesItr(i))
					{
						WorldNormal = WorldMesh.GetTriNormal(TriID);
						break;
					}
				}

				FVector TangentX, TangentY;
				WorldNormal.FindBestAxisVectors(TangentX, TangentY);

				const FVector Origin = WorldPos + WorldNormal * Params.Bias;

				// AUDITED (sampling-quality fix): position-keyed deterministic scramble -- see
				// ComputeDeterministicScrambleAngle's own doc comment.
				const float ScrambleAngle = ComputeDeterministicScrambleAngle(WorldPos);
				const float CosS = FMath::Cos(ScrambleAngle);
				const float SinS = FMath::Sin(ScrambleAngle);

				int32 NumOccluded = 0;
				int32 NumValidSamples = 0;
				for (const FVector& LocalDir : LocalSampleDirs)
				{
					const float RotatedX = LocalDir.X * CosS - LocalDir.Y * SinS;
					const float RotatedY = LocalDir.X * SinS + LocalDir.Y * CosS;
					const FVector WorldDir = TangentX * RotatedX + TangentY * RotatedY + WorldNormal * LocalDir.Z;
					const FVector NormalizedDir = WorldDir.GetSafeNormal();
					if (NormalizedDir.IsNearlyZero())
					{
						continue;
					}
					++NumValidSamples;

					// AUDITED (self-hit fix, re-examined): any-hit, trusting Bias alone -- see the
					// function's own SELF-HIT doc comment for why FindNearestHitTriangle+epsilon was
					// reverted.
					const FRay3d Ray(FVector3d(Origin), FVector3d(NormalizedDir), /*bDirectionIsNormalized=*/true);
					if (Tree.TestAnyHitTriangle(Ray, Options))
					{
						++NumOccluded;
					}
				}

				const float AO = (NumValidSamples > 0)
					? (static_cast<float>(NumOccluded) / static_cast<float>(NumValidSamples))
					: 0.0f;
				Cache.RawValues[i] = FMath::Clamp(AO, 0.0f, 1.0f);
			});

			Cache.CachedSamples = Params.Samples;
			Cache.CachedMaxDistance = Params.MaxDistance;
			Cache.CachedBias = Params.Bias;
			Cache.bValuesValid = true;

			// Genuine anomaly only (never routine): a raycast pass that produced a NaN or
			// out-of-[0,1] value indicates bad mesh/normal data, not a normal cache event.
			int32 NumNaNOrOutOfRange = 0;
			for (const float RawValue : Cache.RawValues)
			{
				if (!FMath::IsFinite(RawValue) || RawValue < 0.0f || RawValue > 1.0f)
				{
					++NumNaNOrOutOfRange;
				}
			}
			if (NumNaNOrOutOfRange > 0)
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("Vertex Mask Forge: AO raycast produced %d NaN/out-of-range value(s) out of %d render vert(s) -- check mesh/normal data."),
					NumNaNOrOutOfRange, NumRenderVerts);
			}

			// Explicit operation summary: fires only when a real raycast pass actually ran (never on
			// a RawValues cache hit).
			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO generated: %d vertices, %d samples, %.1f ms"),
				NumRenderVerts, Params.Samples, (FPlatformTime::Seconds() - GenerationStartSeconds) * 1000.0);
		}

		// Populate Mask.Values from the cached raw values, applying Invert here (display/compose
		// contract only -- never touches Cache.RawValues, the tree, or triggers a rebuild of either).
		Mask.Values.SetNumUninitialized(NumRenderVerts);
		Mask.bHasValue.Init(true, NumRenderVerts);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const float Raw = Cache.RawValues[i];
			const float Value = ApplyAOLevelsAndInvert(Raw, Params.LevelsMin, Params.LevelsMax, Params.bInvert);
			Mask.Values[i] = Value;

			Sum += Value;
			MinValue = FMath::Min(MinValue, Value);
			MaxValue = FMath::Max(MaxValue, Value);
			if (FMath::IsNearlyZero(Value, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(Value, 1.f, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearOne;
			}
		}

		Mask.NumValidValues = NumRenderVerts;
		Mask.MinValue = MinValue;
		Mask.MaxValue = MaxValue;
		Mask.MeanValue = static_cast<float>(Sum / NumRenderVerts);
		Mask.NumNearZero = NumNearZero;
		Mask.NumNearOne = NumNearOne;
		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}

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
	static FVertexMaskForgeScalarMask GenerateAmbientOcclusionMaskFromDynamicMesh(
		TUniquePtr<FVertexMaskForgeSourceTopologyAOCache>& CachePtr,
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const uint32 SourceGeometryFingerprint,
		const FTransform& ComponentTransform,
		const FVertexMaskForgeAOParams& RawParams)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::AmbientOcclusion;

		FVertexMaskForgeAOParams Params = RawParams;
		Params.Samples = FMath::Clamp(Params.Samples, 8, 256);
		Params.MaxDistance = FMath::Clamp(Params.MaxDistance, 0.01f, 10000.0f);
		Params.Bias = FMath::Clamp(Params.Bias, 0.001f, 10.0f);
		// AUDITED (AO Levels): clamped independently to [0,1] each -- LevelsMax <= LevelsMin is a valid,
		// deterministic degenerate case (see ApplyAOLevelsAndInvert's own doc comment), not something to
		// reorder or reject here.
		Params.LevelsMin = FMath::Clamp(Params.LevelsMin, 0.0f, 1.0f);
		Params.LevelsMax = FMath::Clamp(Params.LevelsMax, 0.0f, 1.0f);
		Mask.UsedAOParams = Params;

		const FDynamicMeshNormalOverlay* NormalOverlay =
			(SourceMesh.HasAttributes() && SourceMesh.Attributes()->PrimaryNormals() != nullptr)
			? SourceMesh.Attributes()->PrimaryNormals() : nullptr;
		if (!NormalOverlay || NormalOverlay->ElementCount() <= 0
			|| SourceMesh.VertexCount() <= 0 || SourceMesh.TriangleCount() <= 0)
		{
			// EnsureNormalOverlay (called once at working-mesh build time) guarantees a Normal Overlay
			// exists for any Ready working mesh -- reaching here means the mesh itself has no geometry.
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}
		const int32 NumElements = NormalOverlay->MaxElementID();
		Mask.RenderVertexCount = NormalOverlay->ElementCount(); // Domain note: Normal Overlay element count -- see doc comment.

		if (!CachePtr.IsValid())
		{
			CachePtr = MakeUnique<FVertexMaskForgeSourceTopologyAOCache>();
		}
		FVertexMaskForgeSourceTopologyAOCache& Cache = *CachePtr;

		const double GenerationStartSeconds = FPlatformTime::Seconds();

		const bool bMeshMatches = Cache.CachedSourceMesh == &SourceMesh;
		const bool bFingerprintMatches = Cache.CachedGeometryFingerprint == SourceGeometryFingerprint;
		const bool bTransformMatches = Cache.CachedTransform.Equals(ComponentTransform, 1e-5);
		const bool bTreeStillValid = Cache.bTreeValid
			&& bMeshMatches && bFingerprintMatches && bTransformMatches;

		if (Cache.bTreeValid && !bTreeStillValid)
		{
			TArray<FString> Reasons;
			if (!bMeshMatches) { Reasons.Add(TEXT("Source mesh changed")); }
			if (bMeshMatches && !bFingerprintMatches) { Reasons.Add(TEXT("Geometry/normals changed")); }
			if (!bTransformMatches) { Reasons.Add(TEXT("Transform changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO (Source Topology) Tree cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}

		if (!bTreeStillValid)
		{
			Cache.WorldMesh = MakeUnique<FDynamicMesh3>();
			Cache.WorldMesh->EnableTriangleGroups();

			// Explicit VertexID -> dense WorldMesh index remap -- never assumes SourceMesh's own
			// VertexIDs are already dense/compact (see the function's own INDEX SAFETY doc note).
			TArray<int32> VertexIdToWorldIndex;
			VertexIdToWorldIndex.Init(INDEX_NONE, SourceMesh.MaxVertexID());
			for (const int32 VertexID : SourceMesh.VertexIndicesItr())
			{
				const FVector WorldPos = ComponentTransform.TransformPosition(FVector(SourceMesh.GetVertex(VertexID)));
				VertexIdToWorldIndex[VertexID] = Cache.WorldMesh->AppendVertex(WorldPos);
			}
			for (const int32 TriangleID : SourceMesh.TriangleIndicesItr())
			{
				const FIndex3i Tri = SourceMesh.GetTriangle(TriangleID);
				Cache.WorldMesh->AppendTriangle(
					VertexIdToWorldIndex[Tri.A], VertexIdToWorldIndex[Tri.B], VertexIdToWorldIndex[Tri.C]);
			}

			Cache.Tree = MakeUnique<FDynamicMeshAABBTree3>(Cache.WorldMesh.Get());
			Cache.CachedSourceMesh = &SourceMesh;
			Cache.CachedGeometryFingerprint = SourceGeometryFingerprint;
			Cache.CachedTransform = ComponentTransform;
			Cache.bTreeValid = true;
			Cache.bValuesValid = false;
		}

		const bool bSamplesMatch = Cache.CachedSamples == Params.Samples;
		const bool bMaxDistanceMatches = FMath::IsNearlyEqual(Cache.CachedMaxDistance, Params.MaxDistance, 1e-4f);
		const bool bBiasMatches = FMath::IsNearlyEqual(Cache.CachedBias, Params.Bias, 1e-6f);
		const bool bValuesStillValid = Cache.bValuesValid && bSamplesMatch && bMaxDistanceMatches && bBiasMatches;

		if (bTreeStillValid && Cache.bValuesValid && !bValuesStillValid)
		{
			TArray<FString> Reasons;
			if (!bSamplesMatch) { Reasons.Add(FString::Printf(TEXT("Samples %d -> %d"), Cache.CachedSamples, Params.Samples)); }
			if (!bMaxDistanceMatches) { Reasons.Add(TEXT("MaxDistance changed")); }
			if (!bBiasMatches) { Reasons.Add(TEXT("Bias changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO (Source Topology) RawValues cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}

		if (!bValuesStillValid)
		{
			const TArray<FVector> LocalSampleDirs = BuildHemisphereSampleDirections(Params.Samples);
			const FDynamicMesh3& WorldMesh = *Cache.WorldMesh;
			const FDynamicMeshAABBTree3& Tree = *Cache.Tree;
			// Same non-uniform-scale-correct normal transform GenerateAmbientOcclusionMask itself uses.
			const FMatrix NormalMatrix = ComponentTransform.ToMatrixWithScale().Inverse().GetTransposed();

			// Explicit VertexID -> dense WorldMesh index remap, rebuilt here too (independent of the
			// Tree-rebuild branch above, since a RawValues-only miss -- e.g. Bias changed -- must not
			// require rebuilding WorldMesh/Tree, but still needs this same lookup for Origin positions).
			// AUDITED: WorldMesh's own vertex order was assigned by AppendVertex during the (possibly
			// earlier) Tree-build pass above, in SourceMesh.VertexIndicesItr() order -- rebuilding the
			// same mapping by re-walking that same iterator is deterministic and exact, since
			// AppendVertex returns sequential indices 0,1,2,... in call order.
			TArray<int32> VertexIdToWorldIndex;
			VertexIdToWorldIndex.Init(INDEX_NONE, SourceMesh.MaxVertexID());
			{
				int32 NextWorldIndex = 0;
				for (const int32 VertexID : SourceMesh.VertexIndicesItr())
				{
					VertexIdToWorldIndex[VertexID] = NextWorldIndex++;
				}
			}

			Cache.RawValues.SetNumUninitialized(NumElements);

			IMeshSpatial::FQueryOptions Options;
			Options.MaxDistance = Params.MaxDistance;

			// AUDITED (parallelization): identical safety argument as GenerateAmbientOcclusionMask's
			// own PARALLELIZATION doc comment -- Tree/WorldMesh/LocalSampleDirs/Options/NormalOverlay/
			// NormalMatrix/VertexIdToWorldIndex are read-only; each worker writes only Cache.RawValues[e].
			ParallelFor(NumElements, [&](const int32 ElementID)
			{
				if (!NormalOverlay->IsElement(ElementID))
				{
					// Sparse slot: never written, never read (see TryGetValue/bHasValue on the returned
					// Mask -- this array itself has no "has value" bit, so an unused slot is simply
					// never touched; downstream code only ever reads indices proven IsElement() true).
					return;
				}

				const int32 ParentVertexID = NormalOverlay->GetParentVertex(ElementID);
				const int32 WorldIndex = VertexIdToWorldIndex.IsValidIndex(ParentVertexID) ? VertexIdToWorldIndex[ParentVertexID] : INDEX_NONE;
				if (WorldIndex == INDEX_NONE)
				{
					Cache.RawValues[ElementID] = 0.0f;
					return;
				}

				const FVector WorldPos = WorldMesh.GetVertex(WorldIndex);

				const FVector3f LocalNormal = NormalOverlay->GetElement(ElementID);
				FVector WorldNormal = NormalMatrix.TransformVector(FVector(LocalNormal));
				if (!WorldNormal.Normalize())
				{
					// Degenerate normal (pathological content only): fall back to this vertex's own
					// first incident triangle's geometric normal, never a guess.
					WorldNormal = FVector::UpVector;
					for (const int32 TriID : WorldMesh.VtxTrianglesItr(WorldIndex))
					{
						WorldNormal = WorldMesh.GetTriNormal(TriID);
						break;
					}
				}

				FVector TangentX, TangentY;
				WorldNormal.FindBestAxisVectors(TangentX, TangentY);

				const FVector Origin = WorldPos + WorldNormal * Params.Bias;

				const float ScrambleAngle = ComputeDeterministicScrambleAngle(WorldPos);
				const float CosS = FMath::Cos(ScrambleAngle);
				const float SinS = FMath::Sin(ScrambleAngle);

				int32 NumOccluded = 0;
				int32 NumValidSamples = 0;
				for (const FVector& LocalDir : LocalSampleDirs)
				{
					const float RotatedX = LocalDir.X * CosS - LocalDir.Y * SinS;
					const float RotatedY = LocalDir.X * SinS + LocalDir.Y * CosS;
					const FVector WorldDir = TangentX * RotatedX + TangentY * RotatedY + WorldNormal * LocalDir.Z;
					const FVector NormalizedDir = WorldDir.GetSafeNormal();
					if (NormalizedDir.IsNearlyZero())
					{
						continue;
					}
					++NumValidSamples;

					const FRay3d Ray(FVector3d(Origin), FVector3d(NormalizedDir), /*bDirectionIsNormalized=*/true);
					if (Tree.TestAnyHitTriangle(Ray, Options))
					{
						++NumOccluded;
					}
				}

				const float AO = (NumValidSamples > 0)
					? (static_cast<float>(NumOccluded) / static_cast<float>(NumValidSamples))
					: 0.0f;
				Cache.RawValues[ElementID] = FMath::Clamp(AO, 0.0f, 1.0f);
			});

			Cache.CachedSamples = Params.Samples;
			Cache.CachedMaxDistance = Params.MaxDistance;
			Cache.CachedBias = Params.Bias;
			Cache.bValuesValid = true;

			int32 NumNaNOrOutOfRange = 0;
			for (const int32 ElementID : NormalOverlay->ElementIndicesItr())
			{
				const float RawValue = Cache.RawValues[ElementID];
				if (!FMath::IsFinite(RawValue) || RawValue < 0.0f || RawValue > 1.0f)
				{
					++NumNaNOrOutOfRange;
				}
			}
			if (NumNaNOrOutOfRange > 0)
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("Vertex Mask Forge: AO (Source Topology) raycast produced %d NaN/out-of-range value(s) out of %d element(s) -- check mesh/normal data."),
					NumNaNOrOutOfRange, NormalOverlay->ElementCount());
			}

			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO (Source Topology) generated: %d normal element(s), %d samples, %.1f ms"),
				NormalOverlay->ElementCount(), Params.Samples, (FPlatformTime::Seconds() - GenerationStartSeconds) * 1000.0);
		}

		Mask.Values.SetNumZeroed(NumElements);
		Mask.bHasValue.Init(false, NumElements);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;
		int32 NumValid = 0;

		for (const int32 ElementID : NormalOverlay->ElementIndicesItr())
		{
			const float Raw = Cache.RawValues[ElementID];
			const float Value = ApplyAOLevelsAndInvert(Raw, Params.LevelsMin, Params.LevelsMax, Params.bInvert);
			Mask.Values[ElementID] = Value;
			Mask.bHasValue[ElementID] = true;
			++NumValid;

			Sum += Value;
			MinValue = FMath::Min(MinValue, Value);
			MaxValue = FMath::Max(MaxValue, Value);
			if (FMath::IsNearlyZero(Value, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(Value, 1.f, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearOne;
			}
		}

		Mask.NumValidValues = NumValid;
		Mask.MinValue = MinValue;
		Mask.MaxValue = MaxValue;
		Mask.MeanValue = NumValid > 0 ? static_cast<float>(Sum / NumValid) : 0.f;
		Mask.NumNearZero = NumNearZero;
		Mask.NumNearOne = NumNearOne;
		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}

	/**
	 * AUDITED (Nanite source-topology support): sibling of GenerateBoundingBoxMask, used ONLY for
	 * entries in Source-Topology mode. INDIVIDUAL BOUNDS ONLY -- Unified Bounds is not supported for
	 * this domain in this checkpoint (an explicit, scoped-down decision, not an oversight: combining a
	 * render-vertex-domain bounds pass with a Dynamic-Mesh-vertex-domain one in the same collective
	 * bounds computation would require its own design, and the common case is one Nanite mesh edited at
	 * a time). CollectiveBounds is therefore never accepted here; ComponentTransform is always this
	 * specific instance's own transform (never a shared/identity reference the way the render-vertex
	 * path's entry-level evaluation uses).
	 *
	 * Otherwise identical math to GenerateBoundingBoxMask (same ResolveAxisCoordinate/
	 * EvaluateAxisBaseGradient/Mirror/Invert/axis-combination-by-maximum contract, unchanged and not
	 * repeated here) -- the only difference is the vertex source: SourceMesh.GetVertex(VertexID)
	 * (world-transformed) instead of LOD0's PositionVertexBuffer.
	 *
	 * INDEX SAFETY (corrected): indexed by Dynamic Mesh Vertex ID, but NEVER assumes SourceMesh is
	 * compact -- Mask.Values/bHasValue are sized by SourceMesh.MaxVertexID() (not VertexCount()) and
	 * written only at indices actually yielded by VertexIndicesItr() (which never yields an invalid
	 * ID); a caller must use TryGetValue(), never index Values directly, exactly per
	 * FVertexMaskForgeScalarMask's own struct-level contract for a sparse domain.
	 */
	static FVertexMaskForgeScalarMask GenerateBoundingBoxMaskFromDynamicMesh(
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const FTransform& ComponentTransform)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::BoundingBox;
		Mask.UsedAxisParams = AxisParams;
		Mask.bUnifiedBounds = false; // Never Unified in this domain -- see the function's own doc comment.

		if (SourceMesh.VertexCount() <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}
		const int32 NumVerts = SourceMesh.VertexCount();
		const int32 ArraySize = SourceMesh.MaxVertexID();
		Mask.RenderVertexCount = NumVerts; // Domain note: Dynamic Mesh vertex count -- see doc comment.

		bool bAnyAxisEnabled = false;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled)
			{
				bAnyAxisEnabled = true;
				break;
			}
		}
		if (!bAnyAxisEnabled)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

#if !UE_BUILD_SHIPPING
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisParams[AxisIndex].bMirror)
			{
				VerifyMirrorSymmetryOnce();
				break;
			}
		}
#endif

		constexpr double MinExtent = 1e-5;

		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> AxisBounds;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (!AxisParams[AxisIndex].bEnabled)
			{
				continue;
			}
			const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
			const bool bWorldSpace = AxisParams[AxisIndex].bWorldSpace;
			FVertexMaskForgeAxisBoundsResult& BoundsResult = AxisBounds[AxisIndex];
			BoundsResult.MinCoord = TNumericLimits<double>::Max();
			BoundsResult.MaxCoord = TNumericLimits<double>::Lowest();

			for (const int32 VertexID : SourceMesh.VertexIndicesItr())
			{
				const FVector3f LocalPosition(SourceMesh.GetVertex(VertexID));
				// Individual bounds only in this domain (never Unified) -- bUseUnifiedBounds is always
				// false, matching Individual mode's own contract in GenerateBoundingBoxMask.
				const double Coord = ResolveAxisCoordinate(
					LocalPosition, ComponentTransform, Axis, bWorldSpace, /*bUseUnifiedBounds=*/false,
					FVector::ZeroVector, 1.0);
				BoundsResult.MinCoord = FMath::Min(BoundsResult.MinCoord, Coord);
				BoundsResult.MaxCoord = FMath::Max(BoundsResult.MaxCoord, Coord);
			}

			const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
			if (!FMath::IsFinite(Extent) || Extent <= MinExtent)
			{
				BoundsResult.bDegenerate = true;
			}
		}

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisBounds[AxisIndex].bDegenerate)
			{
				Mask.State = EVertexMaskForgeScalarMaskState::DegenerateBounds;
				return Mask;
			}
		}

		Mask.Values.SetNumZeroed(ArraySize);
		Mask.bHasValue.Init(false, ArraySize);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;
		bool bAllFinite = true;
		bool bAllInRange = true;

		for (const int32 VertexID : SourceMesh.VertexIndicesItr())
		{
			const FVector3f LocalPosition(SourceMesh.GetVertex(VertexID));

			float CombinedMask = 0.f;
			for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				const FVertexMaskForgeAxisMaskParams& Params = AxisParams[AxisIndex];
				if (!Params.bEnabled)
				{
					continue;
				}
				const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
				const FVertexMaskForgeAxisBoundsResult& BoundsResult = AxisBounds[AxisIndex];

				const double Coord = ResolveAxisCoordinate(
					LocalPosition, ComponentTransform, Axis, Params.bWorldSpace, /*bUseUnifiedBounds=*/false,
					FVector::ZeroVector, 1.0);

				const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
				const float T = static_cast<float>((Coord - BoundsResult.MinCoord) / Extent);

				const float SafeTransitionWidth = FMath::Max(Params.TransitionWidth, 1e-4f);
				const float EvaluationT = Params.bMirror ? (1.f - FMath::Abs(2.f * T - 1.f)) : T;
				float AxisMask = EvaluateAxisBaseGradient(EvaluationT, Params.Position, SafeTransitionWidth);

				if (Params.bInvert)
				{
					AxisMask = 1.f - AxisMask;
				}
				AxisMask = FMath::Clamp(AxisMask, 0.f, 1.f);

				CombinedMask = FMath::Max(CombinedMask, AxisMask);
			}

			if (!FMath::IsFinite(CombinedMask))
			{
				bAllFinite = false;
			}
			if (CombinedMask < 0.f || CombinedMask > 1.f)
			{
				bAllInRange = false;
			}

			Mask.Values[VertexID] = CombinedMask;
			Mask.bHasValue[VertexID] = true;

			Sum += CombinedMask;
			MinValue = FMath::Min(MinValue, CombinedMask);
			MaxValue = FMath::Max(MaxValue, CombinedMask);

			if (FMath::IsNearlyZero(CombinedMask, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(CombinedMask, 1.f, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearOne;
			}
		}

		Mask.NumValidValues = NumVerts;
		Mask.MinValue = MinValue;
		Mask.MaxValue = MaxValue;
		Mask.MeanValue = static_cast<float>(Sum / NumVerts);
		Mask.NumNearZero = NumNearZero;
		Mask.NumNearOne = NumNearOne;

		if (!bAllFinite || !bAllInRange || Mask.NumValidValues != NumVerts || Mask.Values.Num() != ArraySize)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Invalid;
			return Mask;
		}

		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}

	// --- Preview: Preview Mode / Channel Filter ----------------------------------------------

	static FText GetPreviewModeLabel(const EVertexMaskForgePreviewMode Mode)
	{
		switch (Mode)
		{
		case EVertexMaskForgePreviewMode::OriginalMaterial:
			return LOCTEXT("PreviewModeOriginal", "Original Material");
		case EVertexMaskForgePreviewMode::RGBVertexColor:
			return LOCTEXT("PreviewModeRGB", "RGB Vertex Color");
		case EVertexMaskForgePreviewMode::RedChannel:
			return LOCTEXT("PreviewModeRed", "Red Channel");
		case EVertexMaskForgePreviewMode::GreenChannel:
			return LOCTEXT("PreviewModeGreen", "Green Channel");
		case EVertexMaskForgePreviewMode::BlueChannel:
			return LOCTEXT("PreviewModeBlue", "Blue Channel");
		case EVertexMaskForgePreviewMode::AlphaChannel:
			return LOCTEXT("PreviewModeAlpha", "Alpha Channel");
		default:
			return FText::GetEmpty();
		}
	}

	static FText GetBlendModeLabel(const EVertexMaskForgeBlendMode Mode)
	{
		switch (Mode)
		{
		case EVertexMaskForgeBlendMode::Copy:
			return LOCTEXT("BlendModeCopy", "Copy");
		case EVertexMaskForgeBlendMode::Add:
			return LOCTEXT("BlendModeAdd", "Add");
		case EVertexMaskForgeBlendMode::Subtract:
			return LOCTEXT("BlendModeSubtract", "Subtract");
		case EVertexMaskForgeBlendMode::Multiply:
			return LOCTEXT("BlendModeMultiply", "Multiply");
		case EVertexMaskForgeBlendMode::Overlay:
			return LOCTEXT("BlendModeOverlay", "Overlay");
		case EVertexMaskForgeBlendMode::Screen:
			return LOCTEXT("BlendModeScreen", "Screen");
		case EVertexMaskForgeBlendMode::Linear:
			return LOCTEXT("BlendModeLinear", "Linear");
		default:
			return FText::GetEmpty();
		}
	}

	// --- Blend Mode math (see EVertexMaskForgeBlendMode) -------------------------------------

	/**
	 * The raw, un-opacitized blend-mode formula for one channel, operating on already-normalized
	 * [0,1] Base ("B")/Mask ("M") values. Pure function -- mutates nothing, never clamps. Linear is
	 * deliberately NOT an alias of Copy, even though lerp(B, M, M) at M=1 equals M exactly like
	 * Copy does -- for M < 1 the two diverge (Linear also factors in B), which is the whole point of
	 * offering it as a distinct mode.
	 */
	static float ApplyMaskBlendMode(const float Base, const float Mask, const EVertexMaskForgeBlendMode Mode)
	{
		switch (Mode)
		{
		case EVertexMaskForgeBlendMode::Copy:
			return Mask;
		case EVertexMaskForgeBlendMode::Add:
			return Base + Mask;
		case EVertexMaskForgeBlendMode::Subtract:
			return Base - Mask;
		case EVertexMaskForgeBlendMode::Multiply:
			return Base * Mask;
		case EVertexMaskForgeBlendMode::Overlay:
			return (Base < 0.5f)
				? (2.0f * Base * Mask)
				: (1.0f - 2.0f * (1.0f - Base) * (1.0f - Mask));
		case EVertexMaskForgeBlendMode::Screen:
			return 1.0f - (1.0f - Base) * (1.0f - Mask);
		case EVertexMaskForgeBlendMode::Linear:
			return FMath::Lerp(Base, Mask, Mask);
		default:
			return Mask;
		}
	}

	/**
	 * AUDITED (peer-mask composition checkpoint): the Opacity lerp, WITHOUT any clamp. Renamed from
	 * BlendMaskValue (which used to clamp every call) -- clamping every single-layer step is exactly
	 * what breaks the associativity/commutativity proofs the multi-mask composition below relies on
	 * (demonstrated numerically in the checkpoint report: Add-then-Subtract vs Subtract-then-Add only
	 * agree if intermediate saturation is never clamped away mid-fold). Clamping now happens ONLY at
	 * the two audited boundaries inside ComposeMaskStack -- see that function's own doc comment.
	 */
	static float BlendMaskValueUnclamped(const float Base, const float Mask, const EVertexMaskForgeBlendMode Mode, const float Opacity)
	{
		const float BlendResult = ApplyMaskBlendMode(Base, Mask, Mode);
		return FMath::Lerp(Base, BlendResult, Opacity);
	}

	/**
	 * ONE mask generator's contribution to the composition -- Bounding Box, Ambient Occlusion, and any
	 * future generator (Curvature, Thickness, ...) are STRUCTURALLY IDENTICAL peers here: this struct
	 * carries no notion of "spatial" vs "content", no fixed role, and no UI-derived position. Already
	 * resolved to the PER-COMPONENT effective mask for this call (World Space Bounding Box axes and
	 * Ambient Occlusion are both re-evaluated per component by the caller -- see ApplyPreviewToEntry --
	 * before being wrapped here). Mask is a non-owning pointer into a caller-owned
	 * FVertexMaskForgeScalarMask that must outlive the UpdateWorkingColors call it is passed to.
	 * Mask->Source doubles as the fixed, stable "generator identifier" used to break ties between two
	 * masks sharing the same Blend Mode (see ComposeMaskStack) -- never reused for anything else, never
	 * exposed in the UI, never configurable.
	 */
	struct FVertexMaskForgeMaskLayerParams
	{
		const FVertexMaskForgeScalarMask* Mask = nullptr;
		EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;
		float Opacity = 1.0f;

		/**
		 * AUDITED (Nanite source-topology support): when >= 0, ComposeMaskStack looks up THIS value
		 * instead of its own shared VertexIndex parameter for this one layer -- needed because Source-
		 * Topology masks are not all indexed by the same domain (Bounding Box by Dynamic Mesh Vertex ID;
		 * Ambient Occlusion by Normal Overlay Element ID, to preserve hard-edge AO correctness -- see
		 * GenerateAmbientOcclusionMaskFromDynamicMesh's own doc comment). -1 (default) preserves the
		 * original, single-shared-index behavior every existing (render-vertex domain) caller already
		 * relies on -- unchanged, since every layer there is indexed identically by render vertex index.
		 */
		int32 IndexOverride = -1;
	};

	/**
	 * AUDITED (peer-mask composition checkpoint -- supersedes the previous, UI-position-ordered fold
	 * this same function used to implement). Combines an UNORDERED set of mask generators (Bounding
	 * Box, Ambient Occlusion, future Curvature/Thickness/...) into ONE result per channel, starting
	 * from BaselineColor's own channel value ("Base") -- never a neutral 1.0/0.0 seed, and Base is
	 * NEVER itself a peer entry in Layers (no generator, no Enable, no Invert, no Blend Mode, no
	 * Opacity of its own) -- it is simply the accumulator the canonical-order operations below act on.
	 * Proof this must be Base (not a neutral seed): a single Multiply-mode mask at Opacity 0 must
	 * leave the channel completely unchanged ("Opacity 0 = no influence", the pre-existing, still-
	 * approved contract) -- lerp(Base, Base*Mask, 0) = Base only holds when the fold's own base
	 * already equals Base; lerp(1, 1*Mask, 0) = 1 would incorrectly turn the channel white instead.
	 *
	 * CANONICAL ORDER (approved convention -- internal, mathematical, never a UI/artistic stack; see
	 * the checkpoint report for the full derivation and proofs): every enabled mask is processed in
	 * FIXED STAGES, by Blend Mode, in this exact sequence -- Copy, {Add+Subtract combined}, Multiply,
	 * Overlay, Screen, Linear (the EVertexMaskForgeBlendMode enum's own declaration order, with
	 * Add/Subtract merged into one stage). This ordering is NEVER configurable, NEVER derived from
	 * section position in the UI, and NEVER derived from the order masks were enabled in -- two masks
	 * sharing the exact same Blend Mode are tie-broken by Mask->Source (a fixed, stable generator
	 * identifier, itself never configurable). No generator (Bounding Box, Ambient Occlusion, or any
	 * future one) has any special role -- ALL of them go through the exact same per-stage logic below,
	 * selected purely by which Blend Mode each one's own panel setting currently uses.
	 *
	 * Per stage:
	 *   - Copy: sequential fold (BlendMaskValueUnclamped), Mask->Source order -- proven NON-
	 *     commutative even among only Copy-mode masks (two different Opacity<1 Copy layers do not
	 *     commute), so this is the one stage with no closed-form reduction.
	 *   - Add+Subtract: closed form, order-irrelevant -- Result = R + Sum(AddValue*AddOpacity) -
	 *     Sum(SubValue*SubOpacity). Proven: BlendMaskValueUnclamped(R,M,Add,Op) = R + M*Op and
	 *     (R,M,Subtract,Op) = R - M*Op are both purely additive per-term, so summing every Add/Subtract
	 *     mask's own term, in ANY order, reproduces the exact same total. No clamp within this stage
	 *     (per the checkpoint's explicit instruction) -- the running total may legitimately leave
	 *     [0,1] here.
	 *   - Multiply: closed form, order-irrelevant -- Result = R * Product(lerp(1,MaskValue,Opacity)).
	 *     Proven algebraically associative/commutative among Multiply-mode masks regardless of R's own
	 *     magnitude (the proof needs no assumption that R is normalized), so it safely consumes
	 *     whatever the Add/Subtract stage produced, in or out of [0,1], with no clamp beforehand.
	 *   - CLAMP BOUNDARY: R is clamped to [0,1] here, and ONLY here (plus the final defensive clamp) --
	 *     this is the one clamp the checkpoint's audit proved is actually required: Overlay's branch
	 *     (R < 0.5), and Screen/Linear's (1-R)-based formulas, are only proven to map [0,1] back into
	 *     [0,1] when their OWN input is already in [0,1] (see the checkpoint report's worked
	 *     Add/Subtract-then-Overlay example, which shows what happens without this clamp: Overlay
	 *     receiving R=1.4 produces 1.56, compounding out-of-domain error instead of correcting it).
	 *   - Overlay: sequential fold (BlendMaskValueUnclamped), Mask->Source order -- proven NON-
	 *     commutative (worked counter-example in the checkpoint report: swapping two Overlay masks
	 *     changes the result from 0.84 to 0.36 for the same inputs).
	 *   - Screen: closed form, order-irrelevant -- (1-Result) = (1-R) * Product(1-MaskValue*Opacity).
	 *     Proven algebraically (De Morgan dual of Multiply). Input is already in [0,1] from the clamp
	 *     boundary above (and Overlay's own output stays in [0,1] given [0,1] input -- proven), so no
	 *     further clamp is needed before this stage.
	 *   - Linear: sequential fold (BlendMaskValueUnclamped), Mask->Source order -- proven NON-
	 *     commutative (f(B,M) != f(M,B) whenever B != M and B+M != 1). Proven to stay in [0,1] given
	 *     [0,1] input (B(1-M)+M^2 in [0,1] for B,M in [0,1]), so still no clamp needed entering this
	 *     stage; a final defensive clamp still closes out the whole channel computation.
	 *
	 * AUDITED (non-accumulation preserved): Base is re-read from BaselineColor fresh on EVERY single
	 * UpdateWorkingColors call (never from a previous WorkingColors/Accumulator value -- see that
	 * function's own doc comment), so regenerating the SAME set of masks with the SAME parameters
	 * always reproduces the EXACT same final result, never drifting across repeated recompositions.
	 *
	 * A "Fill/Constant" layer (Mask->Source == ConstantWhite/ConstantBlack) is handled entirely by the
	 * CALLER (ApplyPreviewToEntry) as a single Copy@1.0 layer, never combined with any other generator
	 * in the same pass -- this function has no special knowledge of Fill/Constant sources at all; it
	 * just sees one Copy-mode layer in that case, same as any other generator would look if configured
	 * that way.
	 *
	 * Channels NOT enabled in the Channel Filter (bFilterR/G/B false) are read verbatim from
	 * CommittedColor -- untouched by any layer or stage. Alpha is always BaselineColor.W,
	 * unconditionally. bOutAnyLayerContributed is true iff at least one layer had a value for this
	 * vertex -- the caller uses it to decide whether to write this vertex's R/G/B into WorkingColors at
	 * all, or leave it exactly as the CommittedColors copy left it.
	 */
	static FVector4f ComposeMaskStack(
		const FVector4f& BaselineColor,
		const FVector4f& CommittedColor,
		const int32 VertexIndex,
		TArrayView<const FVertexMaskForgeMaskLayerParams> SortedLayers,
		const bool bFilterR, const bool bFilterG, const bool bFilterB,
		bool& bOutAnyLayerContributed)
	{
		bOutAnyLayerContributed = false;

		// Resolved ONCE per vertex -- MaskValue does not vary per channel, so every channel below
		// reuses the exact same set of (MaskValue, Mode, Opacity) contributions. SortedLayers is
		// already ordered by Mask->Source (the caller sorts once, outside the per-vertex loop) --
		// preserved here, which is what gives the Copy/Overlay/Linear stages their deterministic,
		// generator-ID tie-break order.
		struct FResolvedContribution
		{
			float MaskValue;
			EVertexMaskForgeBlendMode Mode;
			float Opacity;
		};
		TArray<FResolvedContribution, TInlineAllocator<8>> Contributions;
		for (const FVertexMaskForgeMaskLayerParams& Layer : SortedLayers)
		{
			float MaskValue = 0.f;
			const int32 LookupIndex = (Layer.IndexOverride >= 0) ? Layer.IndexOverride : VertexIndex;
			if (!Layer.Mask || !Layer.Mask->TryGetValue(LookupIndex, MaskValue))
			{
				continue;
			}
			bOutAnyLayerContributed = true;
			Contributions.Add({ MaskValue, Layer.BlendMode, Layer.Opacity });
		}

		if (!bOutAnyLayerContributed)
		{
			return FVector4f(CommittedColor.X, CommittedColor.Y, CommittedColor.Z, BaselineColor.W);
		}

		auto ComposeChannel = [&Contributions](const float Base) -> float
		{
			float R = Base;

			// Stage 1: Copy -- sequential fold, Mask->Source order (non-commutative, no closed form).
			for (const FResolvedContribution& C : Contributions)
			{
				if (C.Mode == EVertexMaskForgeBlendMode::Copy)
				{
					R = BlendMaskValueUnclamped(R, C.MaskValue, EVertexMaskForgeBlendMode::Copy, C.Opacity);
				}
			}

			// Stage 2: Add + Subtract -- closed form, order-irrelevant. No clamp within this stage.
			for (const FResolvedContribution& C : Contributions)
			{
				if (C.Mode == EVertexMaskForgeBlendMode::Add)
				{
					R += C.MaskValue * C.Opacity;
				}
				else if (C.Mode == EVertexMaskForgeBlendMode::Subtract)
				{
					R -= C.MaskValue * C.Opacity;
				}
			}

			// Stage 3: Multiply -- closed form, order-irrelevant. Safely consumes an out-of-[0,1] R
			// from Stage 2 (the algebraic proof needs no [0,1] assumption on R).
			for (const FResolvedContribution& C : Contributions)
			{
				if (C.Mode == EVertexMaskForgeBlendMode::Multiply)
				{
					R *= FMath::Lerp(1.0f, C.MaskValue, C.Opacity);
				}
			}

			// CLAMP BOUNDARY (audited, required): Overlay/Screen/Linear's own formulas are only
			// proven to stay in [0,1] when their input already is -- see this function's own doc
			// comment for the worked counter-example without this clamp.
			R = FMath::Clamp(R, 0.0f, 1.0f);

			// Stage 4: Overlay -- sequential fold, Mask->Source order (non-commutative, no closed form).
			for (const FResolvedContribution& C : Contributions)
			{
				if (C.Mode == EVertexMaskForgeBlendMode::Overlay)
				{
					R = BlendMaskValueUnclamped(R, C.MaskValue, EVertexMaskForgeBlendMode::Overlay, C.Opacity);
				}
			}

			// Stage 5: Screen -- closed form (De Morgan dual of Multiply), order-irrelevant. No
			// additional clamp needed: R is already in [0,1] from the boundary above, and Overlay's
			// own output stays in [0,1] given [0,1] input (proven).
			for (const FResolvedContribution& C : Contributions)
			{
				if (C.Mode == EVertexMaskForgeBlendMode::Screen)
				{
					R = 1.0f - (1.0f - R) * (1.0f - C.MaskValue * C.Opacity);
				}
			}

			// Stage 6: Linear -- sequential fold, Mask->Source order (non-commutative, no closed form).
			for (const FResolvedContribution& C : Contributions)
			{
				if (C.Mode == EVertexMaskForgeBlendMode::Linear)
				{
					R = BlendMaskValueUnclamped(R, C.MaskValue, EVertexMaskForgeBlendMode::Linear, C.Opacity);
				}
			}

			// Final defensive clamp (float precision only -- every stage above is already proven to
			// leave R in [0,1] by this point under normal inputs).
			return FMath::Clamp(R, 0.0f, 1.0f);
		};

		return FVector4f(
			bFilterR ? ComposeChannel(BaselineColor.X) : CommittedColor.X,
			bFilterG ? ComposeChannel(BaselineColor.Y) : CommittedColor.Y,
			bFilterB ? ComposeChannel(BaselineColor.Z) : CommittedColor.Z,
			BaselineColor.W);
	}

	/**
	 * Reduces a composed RGBA color to what the given Preview Mode should actually display.
	 *
	 * AUDITED (Alpha checkpoint): AlphaChannel follows the EXACT same pattern already established
	 * for Red/Green/Blue -- Composite.W (the real Alpha, ALWAYS just the baseline's own Alpha since
	 * the Blend Modes checkpoint -- see ComposeMaskLayer/UpdateWorkingColors) is read out as grayscale (A=0 -> black,
	 * A=1 -> white, values in between -> proportional gray), exactly mirroring how the other three
	 * channel-isolation modes already work. This reduction is a DISPLAY-ONLY view of Composite --
	 * Composite itself (the real RGBA the Preview holds up to this point) is never mutated by this
	 * function; only the returned copy is reduced to grayscale.
	 */
	static FVector4f ApplyPreviewModeDisplay(const FVector4f& Composite, const EVertexMaskForgePreviewMode Mode)
	{
		switch (Mode)
		{
		case EVertexMaskForgePreviewMode::RedChannel:
			return FVector4f(Composite.X, Composite.X, Composite.X, 1.f);
		case EVertexMaskForgePreviewMode::GreenChannel:
			return FVector4f(Composite.Y, Composite.Y, Composite.Y, 1.f);
		case EVertexMaskForgePreviewMode::BlueChannel:
			return FVector4f(Composite.Z, Composite.Z, Composite.Z, 1.f);
		case EVertexMaskForgePreviewMode::AlphaChannel:
			return FVector4f(Composite.W, Composite.W, Composite.W, 1.f);
		case EVertexMaskForgePreviewMode::RGBVertexColor:
		default:
			return Composite;
		}
	}

	static FColor ToDisplayFColor(const FVector4f& Color)
	{
		auto ClampChannel = [](const float V) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(V * 255.f), 0, 255)); };
		return FColor(ClampChannel(Color.X), ClampChannel(Color.Y), ClampChannel(Color.Z), ClampChannel(Color.W));
	}

	// --- Render-vertex <-> Dynamic-Mesh-vertex position correspondence -----------------------
	// AUDITED: no longer used by the Bounding Box Z mask (see GenerateBoundingBoxMask and
	// UpdateWorkingColors), since a purely spatial, Local-Z-only mask has no need for
	// Dynamic Mesh topology and can be computed directly in render-vertex order. Left here,
	// intentionally unused for now, only for a FUTURE generator that genuinely needs Dynamic Mesh
	// topology/source-vertex correspondence (e.g. per-triangle or connectivity-dependent data) --
	// such a generator must re-derive whether this position-based approximation is still safe for
	// its case before reusing it; it must NOT be reintroduced for spatial-only masks.

	/** Result of trying to find the one Dynamic Mesh vertex that corresponds to a render vertex's position. */
	enum class EPositionMatchResult : uint8 { Matched, Unmatched, Ambiguous };

	/**
	 * Buckets every Dynamic Mesh vertex by quantized local-space position (grid cell = 1 / QuantizeScale).
	 * Multiple vertices may land in the same bucket; all are kept (no collapsing here -- collapsing
	 * happens, if at all, only after a real distance check in FindMatchingVertexID()).
	 */
	static TMap<FIntVector, TArray<int32>> BuildPositionBuckets(const UE::Geometry::FDynamicMesh3& Mesh, const double QuantizeScale)
	{
		TMap<FIntVector, TArray<int32>> Buckets;
		Buckets.Reserve(Mesh.VertexCount());

		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			const FVector3d P = Mesh.GetVertex(VertexID);
			const FIntVector Key(
				FMath::RoundToInt(P.X * QuantizeScale),
				FMath::RoundToInt(P.Y * QuantizeScale),
				FMath::RoundToInt(P.Z * QuantizeScale));
			Buckets.FindOrAdd(Key).Add(VertexID);
		}

		return Buckets;
	}

	/**
	 * Finds the Dynamic Mesh vertex within Tolerance of QueryPosition, searching the query's bucket
	 * and its 26 neighbors (so a position near a bucket boundary is not missed) and verifying every
	 * candidate with a real squared-distance check -- the bucket lookup is only ever an acceleration
	 * structure, never treated as the match itself. If more than one distinct vertex is within
	 * Tolerance (coincident/duplicate/overlapping geometry), the match is deliberately rejected as
	 * Ambiguous rather than guessing via nearest-of-several.
	 */
	static EPositionMatchResult FindMatchingVertexID(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TMap<FIntVector, TArray<int32>>& Buckets,
		const FVector3d& QueryPosition,
		const double QuantizeScale,
		const double ToleranceSq,
		int32& OutVertexID)
	{
		OutVertexID = INDEX_NONE;

		const int32 CenterX = FMath::RoundToInt(QueryPosition.X * QuantizeScale);
		const int32 CenterY = FMath::RoundToInt(QueryPosition.Y * QuantizeScale);
		const int32 CenterZ = FMath::RoundToInt(QueryPosition.Z * QuantizeScale);

		int32 MatchVertexID = INDEX_NONE;
		int32 NumWithinTolerance = 0;

		for (int32 dx = -1; dx <= 1; ++dx)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dz = -1; dz <= 1; ++dz)
				{
					const TArray<int32>* Candidates = Buckets.Find(FIntVector(CenterX + dx, CenterY + dy, CenterZ + dz));
					if (!Candidates)
					{
						continue;
					}

					for (const int32 CandidateVertexID : *Candidates)
					{
						const double DistSq = FVector3d::DistSquared(QueryPosition, Mesh.GetVertex(CandidateVertexID));
						if (DistSq <= ToleranceSq)
						{
							++NumWithinTolerance;
							MatchVertexID = CandidateVertexID;
						}
					}
				}
			}
		}

		if (NumWithinTolerance == 0)
		{
			return EPositionMatchResult::Unmatched;
		}
		if (NumWithinTolerance > 1)
		{
			return EPositionMatchResult::Ambiguous;
		}

		OutVertexID = MatchVertexID;
		return EPositionMatchResult::Matched;
	}

	/**
	 * Updates BaselineColors/CommittedColors/WorkingColors (see FVertexMaskForgePreviewComponentState's
	 * own doc comments for the full architectural contract) IN PLACE for one component. This is the
	 * ONLY function that ever writes to any of the three arrays, and both Preview and Accept consume
	 * WorkingColors as this function leaves it, so both always see the exact same result.
	 *
	 * AUDITED (baseline-snapshot fix): BaselineColors is captured ONCE per component per session --
	 * exactly when it is empty or stale-sized (first composition of a new session, or the render
	 * vertex count changed since, e.g. the mesh was rebuilt) -- and is NEVER touched again after
	 * that until RestoreComponentOriginal resets all three arrays together (session end, or this one
	 * component's per-instance mask falling back to degenerate -- see its own doc comment).
	 * InstanceOverrideColors/AssetRenderColors (the LIVE, real, read-only sources) are consulted
	 * ONLY during that one initial-capture branch. CommittedColors is seeded from BaselineColors
	 * verbatim at that same moment. Priority used for the ONE-TIME capture (audited, Problem 3):
	 *   1. InstanceOverrideColors (SourceComponent's own, PRE-EXISTING per-instance
	 *      FStaticMeshComponentLODInfo::OverrideVertexColors, e.g. from a prior Mesh Paint session or
	 *      an earlier Accept as Instance Override on this placed instance) IF non-null and its vertex
	 *      count matches LOD0's -- this is what the artist actually sees in the level, and it is
	 *      read-only here: never written to.
	 *   2. Otherwise, the asset's own LOD0 ColorVertexBuffer (RenderData), if its count matches.
	 *   3. Otherwise, white -- consistent with the rest of the panel's "no original colors" fallback.
	 * A buffer present but with a mismatched vertex count (partial/invalid) is treated exactly like
	 * "absent" and safely falls through to the next priority; it is never partially applied or
	 * index-clamped.
	 *
	 * AUDITED (Channel Filter toggle fix): on EVERY call (not just the first), WorkingColors is
	 * rebuilt FRESH from CommittedColors -- `WorkingColors = CommittedColors;` -- BEFORE any channel
	 * is composed, never carried forward from WorkingColors' own previous value. Each channel
	 * currently enabled in the Channel Filter (see ComposeMaskStack) is then recomputed from
	 * BaselineColors.Channel through the WHOLE layer stack -- ALWAYS starting from BaselineColors,
	 * never from CommittedColors or WorkingColors' prior value, which is what prevents that channel
	 * from ever accumulating ACROSS repeated recompositions (Auto Update Preview re-running, toggling
	 * Opacity/Blend Mode/any axis parameter, Generate Mask) -- see ComposeMaskStack's own doc comment
	 * for why building on the PREVIOUS LAYER's result WITHIN one pass is a different thing and fully
	 * intended. A channel NOT currently enabled is simply whatever CommittedColors already holds -- so
	 * unchecking a channel that was never consolidated immediately, visibly reverts it to
	 * BaselineColors; one that WAS consolidated (by an earlier Generate Mask or Fill) reverts to that
	 * consolidated result, never to a leftover transient value. OutNumComposed reports how many
	 * vertices had AT LEAST ONE layer contribute a value this call; a vertex where every layer skipped
	 * it (Mask.TryGetValue failed for all of them) keeps WorkingColors[i] exactly as the
	 * CommittedColors copy left it (R/G/B), with Alpha still refreshed from BaselineColors.
	 *
	 * bCommit: if true, CommittedColors is promoted to WorkingColors' just-composed result at the end
	 * of this call -- ONLY an explicit Generate Mask click or a Fill White/Black action passes true
	 * (see UpdateAllPreviews' own doc comment for the exhaustive list of callers and their bCommit
	 * value); Auto Update Preview and Channel Filter toggles always pass false, so a transient edit
	 * is never silently consolidated.
	 *
	 * AUDITED (peer-mask composition checkpoint): Layers is an UNORDERED set of every enabled+Ready
	 * mask generator for this component, resolved entirely by the caller (ApplyPreviewToEntry) BEFORE
	 * this call -- Bounding Box and Ambient Occlusion are STRUCTURAL PEERS here, neither one has a
	 * fixed position; each carries its own per-component-evaluated FVertexMaskForgeScalarMask plus its
	 * own BlendMode/Opacity. (A single Fill/Constant layer, when that is the active result for this
	 * pass, is just one more Layers entry the caller already set to Copy@1.0 -- see ApplyPreviewToEntry
	 * -- nothing here treats it specially.) This function sorts Layers ONCE by Mask->Source (a fixed,
	 * stable generator identifier -- never UI position, never enable order) before the per-vertex loop,
	 * then hands the same sorted view to ComposeMaskStack for every vertex -- see that function's own
	 * doc comment for the full canonical-order/stage-grouping contract this sort enables. This function
	 * itself has NO knowledge of Bounding Box or Ambient Occlusion specifically -- it only ever sees an
	 * opaque set of (Mask, BlendMode, Opacity) layers, which is exactly what lets a future generator
	 * (Curvature, Thickness, ...) participate in this SAME pipeline by simply appearing as one more
	 * Layers entry, with no special-cased combination code anywhere in this function.
	 */
	static void UpdateWorkingColors(
		TArray<FColor>& BaselineColors,
		TArray<FColor>& CommittedColors,
		TArray<FColor>& WorkingColors,
		TArrayView<const FVertexMaskForgeMaskLayerParams> Layers,
		const FStaticMeshLODResources& LOD0,
		const FColorVertexBuffer* InstanceOverrideColors,
		const bool bFilterR, const bool bFilterG, const bool bFilterB,
		const bool bCommit,
		int32& OutNumComposed)
	{
		OutNumComposed = 0;

		// AUDITED (peer-mask composition checkpoint): sorted ONCE here, by Mask->Source (the fixed
		// generator identifier), never per-vertex -- the canonical order ComposeMaskStack relies on is
		// the SAME for every render vertex in this component (only each mask's per-vertex VALUE
		// changes, never the set of generators or their Blend Modes/Opacities), so sorting once here
		// and reusing the sorted view for every vertex is both correct and the efficient choice.
		TArray<FVertexMaskForgeMaskLayerParams, TInlineAllocator<8>> SortedLayers(Layers.GetData(), Layers.Num());
		SortedLayers.Sort([](const FVertexMaskForgeMaskLayerParams& A, const FVertexMaskForgeMaskLayerParams& B)
		{
			const uint8 SourceA = A.Mask ? static_cast<uint8>(A.Mask->Source) : 0;
			const uint8 SourceB = B.Mask ? static_cast<uint8>(B.Mask->Source) : 0;
			return SourceA < SourceB;
		});

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const uint32 NumRenderVerts = RenderPositions.GetNumVertices();

		// ONE-TIME capture: empty (session's first composition for this component) or stale-sized
		// (render vertex count changed since, e.g. mesh rebuilt -- a genuine reconstruction case, not
		// an ordinary parameter change). InstanceOverrideColors/AssetRenderColors are read HERE ONLY;
		// every other line in this function reads BaselineColors/CommittedColors exclusively.
		if (BaselineColors.Num() != static_cast<int32>(NumRenderVerts))
		{
			const FColorVertexBuffer& AssetRenderColors = LOD0.VertexBuffers.ColorVertexBuffer;
			const bool bHasInstanceOverride =
				InstanceOverrideColors != nullptr && InstanceOverrideColors->GetNumVertices() == NumRenderVerts;
			const bool bHasAssetColors = !bHasInstanceOverride && AssetRenderColors.GetNumVertices() == NumRenderVerts;

			BaselineColors.SetNumUninitialized(NumRenderVerts);
			for (uint32 i = 0; i < NumRenderVerts; ++i)
			{
				// This render vertex's OWN effective original color -- never a value borrowed from a
				// different render vertex (preserves seams).
				BaselineColors[i] = bHasInstanceOverride ? InstanceOverrideColors->VertexColor(i)
					: bHasAssetColors ? AssetRenderColors.VertexColor(i)
					: FColor::White;
			}

			// "CommittedColors starts as BaselineColors" -- seeded verbatim, exactly once, right here.
			CommittedColors = BaselineColors;
		}

		// WorkingColors is rebuilt FRESH from CommittedColors every single call -- never carried
		// forward from WorkingColors' own previous value. This is what makes unchecking a channel in
		// the Channel Filter immediately, visibly revert it to its last CONSOLIDATED state.
		WorkingColors = CommittedColors;

		for (uint32 i = 0; i < NumRenderVerts; ++i)
		{
			const FColor& BaselineRenderColor = BaselineColors[i];

			// Alpha always tracks the baseline unconditionally, whether or not any layer has a value
			// for this vertex this call.
			WorkingColors[i].A = BaselineRenderColor.A;

			const FVector4f BaselineColorF(
				BaselineRenderColor.R / 255.f, BaselineRenderColor.G / 255.f,
				BaselineRenderColor.B / 255.f, BaselineRenderColor.A / 255.f);
			const FColor& CommittedRenderColor = CommittedColors[i];
			const FVector4f CommittedColorF(
				CommittedRenderColor.R / 255.f, CommittedRenderColor.G / 255.f,
				CommittedRenderColor.B / 255.f, CommittedRenderColor.A / 255.f);

			bool bAnyLayerContributed = false;
			const FVector4f Composite = ComposeMaskStack(
				BaselineColorF, CommittedColorF, static_cast<int32>(i), SortedLayers,
				bFilterR, bFilterG, bFilterB, bAnyLayerContributed);
			if (!bAnyLayerContributed)
			{
				// No layer had a value for this vertex -- R/G/B already carry CommittedColors[i]
				// from the copy above; nothing new to compose.
				continue;
			}
			++OutNumComposed;
			WorkingColors[i] = ToDisplayFColor(Composite);
		}

		// Consolidate: ONLY an explicit Generate Mask click or a Fill action requests this (bCommit
		// == true) -- Auto Update Preview and Channel Filter toggles never do.
		if (bCommit)
		{
			CommittedColors = WorkingColors;
		}
	}

	/**
	 * AUDITED (Nanite source-topology support): sibling of UpdateWorkingColors for Source-Topology
	 * entries. Operates in TRIANGLE-CORNER domain (one slot per (TriangleID, corner) pair, enumerated
	 * by iterating Mesh.TriangleIndicesItr() in a fixed, deterministic order, corners 0/1/2 in that
	 * same order every call) instead of render vertex index -- this is the EXACT granularity that gets
	 * committed (MeshDescription VertexInstanceColors, one slot per triangle corner -- see
	 * WriteSourceTopologyAcceptTargets), so two corners sharing a vertex position (a UV seam or hard
	 * edge) never collapse onto one slot and never lose an independently-authored baseline color.
	 *
	 * Per corner:
	 *   - Baseline color: read from Mesh's own Primary Color Overlay (this corner's own authored
	 *     color), or white if the source has no color overlay at all -- same "own effective original
	 *     color, never borrowed from a different corner" contract UpdateWorkingColors already
	 *     guarantees for render vertices, just at corner granularity here. No per-instance
	 *     OverrideVertexColors priority in this domain (unlike UpdateWorkingColors): Nanite's renderer
	 *     never reads per-instance overrides at all, so there is no per-instance baseline to
	 *     prioritize -- baseline always comes from the asset's own source color overlay.
	 *   - Bounding Box layer's value: looked up by DYNAMIC MESH VERTEX ID (Mesh.GetTriangle(tid)[corner])
	 *     -- BBox is a pure function of position, so corner-level granularity is not needed for the
	 *     VALUE itself, only for where it gets written.
	 *   - Ambient Occlusion layer's value: looked up by NORMAL OVERLAY ELEMENT ID
	 *     (NormalOverlay->GetTriangle(tid)[corner]) -- preserves hard-edge AO correctness, see
	 *     GenerateAmbientOcclusionMaskFromDynamicMesh's own doc comment.
	 *   - A Fill/Constant layer's value: looked up by CORNER INDEX itself (GenerateConstantMaskForCornerDomain
	 *     is built in this exact domain already).
	 * Every lookup is wired through FVertexMaskForgeMaskLayerParams::IndexOverride (see its own doc
	 * comment), set per corner just before each ComposeMaskStack call -- the composition math itself
	 * (ComposeMaskStack) is completely unmodified/unaware of any of this; it just sees whatever index
	 * each layer asks it to look up. Layers is sorted ONCE, by Mask->Source, exactly like
	 * UpdateWorkingColors -- IndexOverride is mutated per corner on that SAME sorted array afterward,
	 * which never changes sort order (sort key is Source, never IndexOverride).
	 */
	static void UpdateWorkingColorsSourceTopology(
		TArray<FColor>& BaselineColors,
		TArray<FColor>& CommittedColors,
		TArray<FColor>& WorkingColors,
		TArrayView<const FVertexMaskForgeMaskLayerParams> Layers,
		const UE::Geometry::FDynamicMesh3& Mesh,
		const bool bFilterR, const bool bFilterG, const bool bFilterB,
		const bool bCommit,
		int32& OutNumComposed)
	{
		using namespace UE::Geometry;

		OutNumComposed = 0;

		TArray<FVertexMaskForgeMaskLayerParams, TInlineAllocator<8>> SortedLayers(Layers.GetData(), Layers.Num());
		SortedLayers.Sort([](const FVertexMaskForgeMaskLayerParams& A, const FVertexMaskForgeMaskLayerParams& B)
		{
			const uint8 SourceA = A.Mask ? static_cast<uint8>(A.Mask->Source) : 0;
			const uint8 SourceB = B.Mask ? static_cast<uint8>(B.Mask->Source) : 0;
			return SourceA < SourceB;
		});

		const FDynamicMeshColorOverlay* ColorOverlay = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryColors() : nullptr;
		const FDynamicMeshNormalOverlay* NormalOverlay = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryNormals() : nullptr;

		const int32 NumCorners = Mesh.TriangleCount() * 3;

		// ONE-TIME capture (session's first composition, or a genuine reconstruction -- corner count
		// changed since, e.g. Refresh Selection rebuilt WorkingMesh). ColorOverlay is read HERE ONLY;
		// every other line below reads BaselineColors/CommittedColors exclusively.
		if (BaselineColors.Num() != NumCorners)
		{
			BaselineColors.SetNumUninitialized(NumCorners);
			int32 SeedCornerIndex = 0;
			for (const int32 TriangleID : Mesh.TriangleIndicesItr())
			{
				const FIndex3i ColorTri = ColorOverlay ? ColorOverlay->GetTriangle(TriangleID) : FIndex3i(INDEX_NONE, INDEX_NONE, INDEX_NONE);
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					FColor Color = FColor::White;
					const int32 ElementID = ColorTri[Corner];
					if (ColorOverlay && ElementID != INDEX_NONE && ColorOverlay->IsElement(ElementID))
					{
						Color = ToDisplayFColor(ColorOverlay->GetElement(ElementID));
					}
					BaselineColors[SeedCornerIndex] = Color;
					++SeedCornerIndex;
				}
			}
			CommittedColors = BaselineColors;
		}

		WorkingColors = CommittedColors;

		int32 CornerIndex = 0;
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			const FIndex3i VertTri = Mesh.GetTriangle(TriangleID);
			const FIndex3i NormalTri = NormalOverlay ? NormalOverlay->GetTriangle(TriangleID) : FIndex3i(INDEX_NONE, INDEX_NONE, INDEX_NONE);

			for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
			{
				for (FVertexMaskForgeMaskLayerParams& Layer : SortedLayers)
				{
					if (!Layer.Mask)
					{
						continue;
					}
					switch (Layer.Mask->Source)
					{
					case EVertexMaskForgeScalarMaskSource::BoundingBox:
						Layer.IndexOverride = VertTri[Corner];
						break;
					case EVertexMaskForgeScalarMaskSource::AmbientOcclusion:
						Layer.IndexOverride = NormalTri[Corner];
						break;
					default: // ConstantWhite / ConstantBlack (Fill) -- corner-domain mask, see
						// GenerateConstantMaskForCornerDomain.
						Layer.IndexOverride = CornerIndex;
						break;
					}
				}

				const FColor& BaselineRenderColor = BaselineColors[CornerIndex];
				WorkingColors[CornerIndex].A = BaselineRenderColor.A;

				const FVector4f BaselineColorF(
					BaselineRenderColor.R / 255.f, BaselineRenderColor.G / 255.f,
					BaselineRenderColor.B / 255.f, BaselineRenderColor.A / 255.f);
				const FColor& CommittedRenderColor = CommittedColors[CornerIndex];
				const FVector4f CommittedColorF(
					CommittedRenderColor.R / 255.f, CommittedRenderColor.G / 255.f,
					CommittedRenderColor.B / 255.f, CommittedRenderColor.A / 255.f);

				bool bAnyLayerContributed = false;
				const FVector4f Composite = ComposeMaskStack(
					BaselineColorF, CommittedColorF, CornerIndex, SortedLayers,
					bFilterR, bFilterG, bFilterB, bAnyLayerContributed);
				if (!bAnyLayerContributed)
				{
					continue;
				}
				++OutNumComposed;
				WorkingColors[CornerIndex] = ToDisplayFColor(Composite);
			}
		}

		if (bCommit)
		{
			CommittedColors = WorkingColors;
		}
	}

	/**
	 * Derives the render-order color buffer actually pushed to FStaticMeshComponentLODInfo::
	 * OverrideVertexColors for the LIVE PREVIEW ONLY, by reducing WorkingColors' RAW composited RGBA
	 * through the current Preview Mode (see ApplyPreviewModeDisplay) -- e.g. "Red Channel" reduces to
	 * a Red-only grayscale.
	 *
	 * AUDITED (Preview-Mode-cannot-affect-Accept fix): DISPLAY-ONLY. Called exclusively from
	 * ApplyPreviewToEntry, for the transient PreviewComponent shown in the viewport. Accept
	 * (BuildAcceptTargets) NEVER calls this function -- it persists State.WorkingColors verbatim, so
	 * the real multi-channel RGB is written regardless of which Preview Mode happens to be selected at
	 * the moment of Accept. Read-only: never mutates WorkingColors.
	 */
	static TArray<FColor> DeriveDisplayColors(const TArray<FColor>& WorkingColors, const EVertexMaskForgePreviewMode Mode)
	{
		TArray<FColor> Result;
		Result.SetNumUninitialized(WorkingColors.Num());

		for (int32 i = 0; i < WorkingColors.Num(); ++i)
		{
			const FColor& Working = WorkingColors[i];
			const FVector4f WorkingF(Working.R / 255.f, Working.G / 255.f, Working.B / 255.f, Working.A / 255.f);
			Result[i] = ToDisplayFColor(ApplyPreviewModeDisplay(WorkingF, Mode));
		}

		return Result;
	}

	// --- Accept: permanent write to Static Mesh asset(s) -------------------------------------

	/** One Static Mesh asset targeted by an Accept operation, with its final, ready-to-write colors. */
	struct FVertexMaskForgeAcceptTarget
	{
		TWeakObjectPtr<UStaticMesh> Mesh;
		FString AssetName;
		/** Render-vertex-order (LOD0), exactly as shown in Preview -- the data actually written. */
		TArray<FColor> FinalColors;
	};

	/**
	 * Validates every SelectedMeshes entry eligible for Accept and, only if ALL of them pass,
	 * returns the exact set of {Static Mesh, final render-order colors} to write. Nothing is written
	 * here -- this function is pure validation + composition, so the caller can guarantee Accept is
	 * all-or-nothing (validate every destination before starting any write).
	 *
	 * An entry is eligible when it has a Ready mask AND at least one live PreviewComponent (Content-
	 * Browser-only entries, with no viewport component, are never eligible -- there is no baseline to
	 * compose from). For an eligible entry:
	 *   - composes render-order colors independently PER SourceComponent, using the same baseline
	 *     priority as the live Preview (per-instance OverrideVertexColors, then asset colors, then
	 *     white -- see UpdateWorkingColors);
	 *   - entries are already 1-per-asset by construction (AddOrUpdateSelectedMesh keys InOutMeshes
	 *     by asset path), so no separate cross-entry dedup by UStaticMesh identity is needed here --
	 *     but if two or more COMPONENTS of that one entry's asset produce DIFFERENT composed results
	 *     (because their per-instance baselines differ), the write target is AMBIGUOUS: Accept is
	 *     blocked for the whole operation (not silently narrowed to one component), the Preview is
	 *     left untouched, and OutErrorText names the asset;
	 *   - otherwise, validates that FStaticMeshLODResources::WedgeMap (the engine's own deterministic
	 *     wedge->render-vertex mapping, exactly as used by UMeshPaintingSubsystem::
	 *     PropagateColorsToRawMesh) is present and matches the current MeshDescription's vertex
	 *     instance count. If it does not, this function refuses -- it NEVER falls back to an
	 *     approximate position-based remap, per the explicit architectural requirement (a stale/
	 *     missing WedgeMap most commonly means the asset needs a Build first).
	 *
	 * AUDITED (recompute-at-Accept fix): this function NEVER calls UpdateWorkingColors, NEVER
	 * re-evaluates GenerateBoundingBoxMask, and NEVER reads BoundingBoxBlendMode/BoundingBoxOpacity/
	 * the Channel Filter -- it only READS each component's State.WorkingColors, exactly as
	 * Auto Update Preview or Generate Mask last left it (see UpdateWorkingColors' own doc comment;
	 * ApplyPreviewToEntry is the only other caller, and it is the SAME array). This is what
	 * structurally guarantees Accept can never silently apply a parameter change that was never
	 * actually generated (e.g. Blend Mode/Opacity/an axis parameter changed with Auto Update Preview
	 * off, without clicking Generate Mask afterward): every one of those parameters either
	 * invalidates the mask (forcing OperationState back to Idle, which disables Accept entirely
	 * until a fresh Generate Mask/Auto Update regenerates WorkingColors) or itself synchronously
	 * updates WorkingColors before Accept can ever see it (Channel Filter). A component whose
	 * WorkingColors is empty (never composed yet, or reset by RestoreComponentOriginal because its
	 * own per-instance World Space mask evaluation was degenerate) is skipped -- the exact same
	 * outcome the old per-component mask re-evaluation produced for that case, without needing to
	 * redo the evaluation here.
	 *
	 * AUDITED (Preview-Mode-cannot-affect-Accept fix): takes NO EVertexMaskForgePreviewMode parameter
	 * at all, and reads State.WorkingColors VERBATIM -- never through DeriveDisplayColors -- so the
	 * persisted result is the same real multi-channel RGB regardless of which Preview Mode (Original
	 * Material, Full RGB, Red/Green/Blue Channel) happens to be selected when Accept runs.
	 * RecomputeOperationState() computes PendingChanges identically regardless of CurrentPreviewMode
	 * (see its own doc comment), so this function needs no Preview-Mode check of its own either.
	 */
	static bool BuildAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		TArray<FVertexMaskForgeAcceptTarget>& OutTargets,
		FText& OutErrorText)
	{
		OutTargets.Reset();

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			// AUDITED (composition-stack checkpoint): eligible if EITHER slot is Ready -- this gate is
			// only a coarse pre-filter anyway; the actual data persisted is always State.WorkingColors
			// (read verbatim below), never re-derived from either mask here.
			//
			// AUDITED (Nanite source-topology support): a Source-Topology entry (every Nanite-enabled
			// mesh -- see FVertexMaskForgeSelectedMesh::bUseSourceTopology) is handled EXCLUSIVELY by
			// BuildSourceTopologyAcceptTargets/WriteSourceTopologyAcceptTargets, never by this function
			// -- skip it here unconditionally, rather than letting it fall through to the WedgeMap
			// check below (which would incorrectly reject a Nanite mesh that happens to have a valid
			// WedgeMap too, since Source-Topology mode is chosen unconditionally for Nanite, not just
			// when WedgeMap is invalid).
			if (!Entry.IsValid() || Entry->bUseSourceTopology || Entry->PreviewComponents.IsEmpty()
				|| (Entry->WorkingMesh.BoundingBoxMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.AmbientOcclusionMask.State != EVertexMaskForgeScalarMaskState::Ready))
			{
				continue;
			}

			UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptInvalidMeshFormat", "'{0}': Static Mesh could not be resolved or has no valid LOD 0 render data."),
					FText::FromString(Entry->AssetName));
				return false;
			}

			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptNoRenderDataFormat", "'{0}': no LOD 0 render data available."),
					FText::FromString(Entry->AssetName));
				return false;
			}
			const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];

			// Compose independently per component and require agreement -- see the divergent-
			// baseline case in the doc comment above.
			TArray<FColor> ReferenceColors;
			bool bHaveReference = false;
			for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
			{
				const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}

				// AUDITED (recompute-at-Accept fix): read-only -- see this function's own doc comment.
				// Empty WorkingColors means this component currently has no valid composed result
				// (mirrors the old "per-instance mask not Ready" skip).
				if (State.WorkingColors.IsEmpty())
				{
					continue;
				}
				// AUDITED (Preview-Mode-cannot-affect-Accept fix): the raw multi-channel result,
				// verbatim -- never DeriveDisplayColors, never CurrentPreviewMode.
				TArray<FColor> ComponentColors = State.WorkingColors;

				if (!bHaveReference)
				{
					ReferenceColors = MoveTemp(ComponentColors);
					bHaveReference = true;
				}
				else if (ComponentColors != ReferenceColors)
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptDivergentBaselineFormat",
							"'{0}': different instances of this asset produced different Preview results (their per-instance Vertex Color overrides differ), so writing to the shared Static Mesh asset would be ambiguous. Accept is blocked for this operation; make the instances' overrides consistent, or Cancel."),
						FText::FromString(Entry->AssetName));
					return false;
				}
			}

			if (!bHaveReference)
			{
				// Every tracked component became invalid since the Preview was last updated; nothing
				// to accept for this entry -- not an error, just nothing eligible here.
				continue;
			}

			// Deterministic wedge->render-vertex mapping check (audited): never approximate by
			// position. Read-only check here (element count only); the mutable pass happens in
			// WriteAcceptTargets.
			//
			// AUDITED (Nanite): confirmed against the actual UE 5.8 Nanite build source
			// (StaticMeshBuilder.cpp's BuildNanite()) that for a Nanite-enabled asset WITHOUT an
			// explicit HiRes Source Model (the common/default case), LODResources[0] is NOT a
			// full-fidelity copy of the source mesh -- it is Nanite's own decimated/re-clustered
			// fallback proxy (FallbackRelativeError defaults to 1.0, i.e. reduction IS applied), and
			// FStaticMeshLODResources::WedgeMap for LOD 0 is never populated on that path at all (the
			// Nanite build explicitly passes bNeedWedgeMap=false -- "mainly used by non-Nanite mesh
			// painting"). So this same WedgeMap check that already protects every other asset ALSO
			// correctly refuses Nanite assets on that default path -- it must NOT be bypassed or
			// "fixed" with a position-based remap, which would silently paint the wrong (reduced)
			// vertex set. Only the message differs, so the user is told the true, actionable reason
			// instead of the generic "try rebuilding" text (rebuilding will not fix this for Nanite --
			// the fallback is decimated by design).
			const FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			const bool bWedgeMapValid = MeshDescription
				&& LOD0.WedgeMap.Num() != 0
				&& LOD0.WedgeMap.Num() == MeshDescription->VertexInstances().Num()
				&& ReferenceColors.Num() == static_cast<int32>(LOD0.GetNumVertices());
			if (!bWedgeMapValid)
			{
				if (Mesh->IsNaniteEnabled())
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptNaniteReducedFallbackFormat",
							"'{0}': this Static Mesh has Nanite enabled and its LOD 0 fallback is a reduced/re-clustered proxy (not 1:1 with the source mesh), so Vertex Mask Forge cannot currently write results back into it safely. Assigning an explicit High Res Source Model to this asset (Nanite Settings) restores a full-fidelity LOD 0 and may allow Accept to succeed; otherwise, source-topology write support for Nanite is not yet implemented."),
						FText::FromString(Entry->AssetName));
				}
				else
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptNoWedgeMapFormat",
							"'{0}': no deterministic wedge-to-render-vertex mapping is available (FStaticMeshLODResources::WedgeMap missing or stale for LOD 0). Refusing to write -- an approximate position-based remap could paint seams incorrectly. Try rebuilding this Static Mesh (Build) and Accept again."),
						FText::FromString(Entry->AssetName));
				}
				return false;
			}

			FVertexMaskForgeAcceptTarget Target;
			Target.Mesh = Mesh;
			Target.AssetName = Entry->AssetName;
			Target.FinalColors = MoveTemp(ReferenceColors);
			OutTargets.Add(MoveTemp(Target));
		}

		// AUDITED (BUG FIX -- Nanite Accept root cause): this function used to treat an empty
		// OutTargets as a hard failure on its own ("AcceptNothingEligible"). That was correct back when
		// this was the ONLY Accept target builder, but since the Source-Topology split, a selection
		// that is ENTIRELY Nanite (bUseSourceTopology, skipped above unconditionally) legitimately
		// produces zero render-vertex targets while still having real, valid Source-Topology targets
		// waiting in BuildSourceTopologyAcceptTargets. Returning false HERE made AcceptPendingChanges
		// bail out immediately -- before BuildSourceTopologyAcceptTargets was ever called -- so a
		// pure-Nanite Accept always failed with "No eligible pending changes to accept.", even though
		// nothing had actually been validated yet, let alone written. "Nothing eligible in EITHER
		// domain" is now decided ONCE, by the caller, after combining both builders' results -- see
		// AcceptPendingChanges. An empty result here is not, by itself, an error.
		return true;
	}

	/**
	 * Writes every target's FinalColors into its Static Mesh asset's LOD 0 MeshDescription (via the
	 * SAME WedgeMap-based approach as UMeshPaintingSubsystem::PropagateColorsToRawMesh -- see the
	 * audit note on BuildAcceptTargets). Re-validates every target BEFORE modifying anything -- once
	 * the write loop starts, every step is expected to succeed by construction (validated moments
	 * earlier, synchronously, nothing else runs in between), so there is no mid-loop rollback path:
	 * if this function ever returns false, NOTHING has been modified.
	 *
	 * AUDITED (Undo/Redo fix): does NOT open its own FScopedTransaction or call Mesh->Build() -- the
	 * caller (AcceptPendingChanges) owns ONE outer transaction spanning this function and its
	 * Source-Topology sibling together, and calls BuildModifiedMeshes() for both domains' results only
	 * AFTER that transaction closes (see BuildModifiedMeshes' own doc comment for why the rebuild must
	 * stay outside it). Every successfully-written Mesh is appended to OutModifiedMeshes for that later
	 * pass.
	 *
	 * AUDITED (Undo/Redo fix, root cause): Mesh->Modify() ALONE only snapshots UStaticMesh's own
	 * UPROPERTY fields -- the TObjectPtr<UStaticMeshDescriptionBulkData> pointer VALUE inside
	 * FStaticMeshSourceModel, which does not change (same sub-object instance before and after Accept).
	 * The actual serialized MeshDescription bytes live inside that SEPARATE UObject
	 * (UStaticMeshDescriptionBulkData, via UMeshDescriptionBaseBulkData::Serialize -- see
	 * MeshDescriptionBaseBulkData.h/.cpp), which the Editor transaction system only captures if Modify()
	 * is ALSO called on THAT object -- exactly what UStaticMesh::ModifyMeshDescription(LodIndex) does
	 * (StaticMesh.cpp: `SourceModel.StaticMeshDescriptionBulkData->Modify(bAlwaysMarkDirty)`). Without
	 * it, Mesh->Modify() alone records an empty/no-op change for this asset, so Undo never restores the
	 * previous colors (this was the actual bug -- not a refresh/notification problem). Confirmed against
	 * the native reference for this exact commit sequence, Modeling Tools'
	 * UStaticMeshToolTarget::CommitMeshDescription (StaticMeshToolTarget.cpp): SetFlags(RF_Transactional)
	 * -> Modify() -> ModifyMeshDescription(LodIndex) -> mutate -> CommitMeshDescription(LodIndex) ->
	 * PostEditChange() (done here via BuildModifiedMeshes/Build(), the derived-data equivalent). On
	 * Undo, UStaticMesh::PostEditUndo() -> Super::PostEditUndo() -> PostEditChangeProperty() -> Build()
	 * automatically regenerates RenderData (and Nanite data) from the restored MeshDescription -- no
	 * extra Undo/Redo handling is needed here for that part.
	 */
	static bool WriteAcceptTargets(const TArray<FVertexMaskForgeAcceptTarget>& Targets, TArray<UStaticMesh*>& OutModifiedMeshes, FText& OutErrorText)
	{
		for (const FVertexMaskForgeAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			const FMeshDescription* MeshDescription = IsValid(Mesh) ? Mesh->GetMeshDescription(0) : nullptr;
			const FStaticMeshRenderData* RenderData = IsValid(Mesh) ? Mesh->GetRenderData() : nullptr;
			const bool bLODValid = RenderData && RenderData->LODResources.IsValidIndex(0);

			if (!MeshDescription || !bLODValid
				|| RenderData->LODResources[0].WedgeMap.Num() != MeshDescription->VertexInstances().Num()
				|| Target.FinalColors.Num() != static_cast<int32>(RenderData->LODResources[0].GetNumVertices()))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptWriteRevalidationFailedFormat", "'{0}' failed re-validation immediately before writing; aborting Accept (nothing was modified)."),
					FText::FromString(Target.AssetName));
				return false;
			}
		}

		for (const FVertexMaskForgeAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];

			// AUDITED (Undo/Redo fix): SetFlags is defensive (assets already loaded in the Editor are
			// transactional in practice) and matches the native reference exactly; Modify() captures
			// UStaticMesh's own properties, ModifyMeshDescription(0) captures the source data that
			// actually changes -- see this function's own doc comment for why both are required.
			Mesh->SetFlags(RF_Transactional);
			Mesh->Modify();
			Mesh->ModifyMeshDescription(0);

			FStaticMeshAttributes Attributes(*MeshDescription);
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

			int32 VertexInstanceIndex = 0;
			for (const FVertexInstanceID VertexInstanceID : MeshDescription->VertexInstances().GetElementIDs())
			{
				const int32 RenderIndex = LOD0.WedgeMap[VertexInstanceIndex];
				if (RenderIndex != INDEX_NONE && Target.FinalColors.IsValidIndex(RenderIndex))
				{
					Colors[VertexInstanceID] = FLinearColor(Target.FinalColors[RenderIndex]);
				}
				++VertexInstanceIndex;
			}

			Mesh->CommitMeshDescription(0);
			OutModifiedMeshes.Add(Mesh);
		}

		return true;
	}

	// --- Accept (Source-Topology / Nanite): permanent write to the SOURCE MeshDescription -----

	/**
	 * AUDITED (Nanite source-topology support): sibling of FVertexMaskForgeAcceptTarget/
	 * BuildAcceptTargets/WriteAcceptTargets for Source-Topology entries (every Nanite-enabled mesh --
	 * see FVertexMaskForgeSelectedMesh::bUseSourceTopology). Same validate-then-write, all-or-nothing
	 * contract; same divergent-per-instance-baseline blocking rule; same shared-asset dedup (one
	 * target per entry, entries are already 1-per-asset by construction). The only structural
	 * difference: colors are in CORNER domain (see UpdateWorkingColorsSourceTopology), and the commit
	 * itself writes via the TriangleID+corner correspondence (Entry->WorkingMesh.TriIDMap) instead of
	 * FStaticMeshLODResources::WedgeMap -- exactly the route the native UE Paint Vertex Colors tool
	 * uses (FDynamicMeshToMeshDescription::UpdateVertexColors), proven correct for Nanite by the
	 * native-tool audit. Entry is kept alive (TSharedPtr) so WorkingMesh.Mesh/TriIDMap remain valid
	 * from preflight through the write pass.
	 */
	struct FVertexMaskForgeSourceTopologyAcceptTarget
	{
		TWeakObjectPtr<UStaticMesh> Mesh;
		FString AssetName;
		/** Corner-domain colors (Entry->WorkingMesh.Mesh's own TriangleIndicesItr()+corner order),
		 *  exactly as shown in Preview -- the data actually written. */
		TArray<FColor> FinalColors;
		TSharedPtr<FVertexMaskForgeSelectedMesh> Entry;
	};

	/**
	 * AUDITED (Nanite source-topology support, commit preflight correction): full formal validation of
	 * the TriangleID -> source FTriangleID -> VertexInstanceID correspondence a Source-Topology Accept
	 * is about to rely on, BEFORE any write. Checks:
	 *   - TriIDMap has a VALID entry for every triangle actually enumerated by WorkingMesh's own
	 *     TriangleIndicesItr() (not just "non-empty") -- a partially-populated map is refused outright,
	 *     never silently skipped mid-write the way the write loop's own defensive guards do (those
	 *     exist as a last-resort safety net, not as the intended detection point).
	 *   - Each mapped FTriangleID still exists in the LIVE MeshDescription
	 *     (MeshDescription.IsTriangleValid) -- catches a stale TriIDMap if the asset's MeshDescription
	 *     was ever rebuilt/edited since WorkingMesh was built, without WorkingMesh itself being rebuilt.
	 *   - No two WorkingMesh triangles map to the SAME destination FTriangleID (TSet-based uniqueness) --
	 *     two different corners silently writing into the same three VertexInstanceIDs would corrupt the
	 *     write (last-write-wins on a shared slot) without ever raising an error otherwise.
	 *   - Each destination triangle provides EXACTLY three VertexInstanceIDs, each itself still valid in
	 *     the live MeshDescription (MeshDescription.IsVertexInstanceValid).
	 * The corner ORDER itself is not a runtime check here -- it is a structural invariant: both
	 * composition (UpdateWorkingColorsSourceTopology) and the write (WriteSourceTopologyAcceptTargets)
	 * iterate the EXACT SAME Mesh.TriangleIndicesItr() + corner 0..2 sequence, so corner i of a given
	 * TriangleID always means the same thing in both places by construction, not by a value that could
	 * silently drift between them.
	 */
	static bool ValidateSourceTopologyCorrespondence(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TArray<FTriangleID>& TriIDMap,
		const FMeshDescription& MeshDescription,
		const FString& AssetName,
		FText& OutErrorText)
	{
		TSet<int32> SeenDestinationTriangles;
		SeenDestinationTriangles.Reserve(Mesh.TriangleCount());

		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			if (!TriIDMap.IsValidIndex(TriangleID))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyMissingTriIDMapEntryFormat",
						"'{0}': the triangle/corner correspondence is missing an entry for one or more triangles. Try Refresh Selection and Generate Mask again."),
					FText::FromString(AssetName));
				return false;
			}

			const FTriangleID SourceTriangleID = TriIDMap[TriangleID];
			if (!MeshDescription.IsTriangleValid(SourceTriangleID))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyStaleTriangleFormat",
						"'{0}': the correspondence points to a triangle that no longer exists in the Static Mesh's source data (it may have been rebuilt or reimported since Generate Mask ran). Try Refresh Selection and Generate Mask again."),
					FText::FromString(AssetName));
				return false;
			}

			bool bAlreadySeen = false;
			SeenDestinationTriangles.Add(SourceTriangleID.GetValue(), &bAlreadySeen);
			if (bAlreadySeen)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyDuplicateTriangleFormat",
						"'{0}': two different triangles map to the same source triangle -- refusing to write an ambiguous correspondence. Try Refresh Selection and Generate Mask again."),
					FText::FromString(AssetName));
				return false;
			}

			const TArrayView<const FVertexInstanceID> SourceInstances = MeshDescription.GetTriangleVertexInstances(SourceTriangleID);
			if (SourceInstances.Num() != 3)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyBadInstanceCountFormat",
						"'{0}': a destination triangle does not have exactly three Vertex Instances. Try Refresh Selection and Generate Mask again."),
					FText::FromString(AssetName));
				return false;
			}
			for (const FVertexInstanceID InstanceID : SourceInstances)
			{
				if (!MeshDescription.IsVertexInstanceValid(InstanceID))
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptSourceTopologyInvalidInstanceFormat",
							"'{0}': the correspondence points to a Vertex Instance that no longer exists. Try Refresh Selection and Generate Mask again."),
						FText::FromString(AssetName));
					return false;
				}
			}
		}

		return true;
	}

	/**
	 * Sibling of BuildAcceptTargets for Source-Topology entries. Only ever produces targets for
	 * entries with bUseSourceTopology == true; BuildAcceptTargets skips those entries entirely (see
	 * its own doc comment), so the two functions' outputs never overlap for the same asset.
	 */
	static bool BuildSourceTopologyAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		TArray<FVertexMaskForgeSourceTopologyAcceptTarget>& OutTargets,
		FText& OutErrorText)
	{
		OutTargets.Reset();

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid() || !Entry->bUseSourceTopology || Entry->PreviewComponents.IsEmpty()
				|| (Entry->WorkingMesh.BoundingBoxMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.AmbientOcclusionMask.State != EVertexMaskForgeScalarMaskState::Ready))
			{
				continue;
			}

			UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Entry->WorkingMesh.Mesh.IsValid())
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyInvalidMeshFormat", "'{0}': Static Mesh or its working topology could not be resolved."),
					FText::FromString(Entry->AssetName));
				return false;
			}

			// Compose independently per component and require agreement -- same divergent-baseline
			// rule as BuildAcceptTargets' own doc comment (no per-instance override in this domain, but
			// World Space Bounding Box axes and Ambient Occlusion are both still per-instance-transform-
			// dependent, so two components CAN legitimately disagree).
			TArray<FColor> ReferenceColors;
			bool bHaveReference = false;
			for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
			{
				const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}
				if (State.SourceTopologyWorkingColors.IsEmpty())
				{
					continue;
				}
				TArray<FColor> ComponentColors = State.SourceTopologyWorkingColors;

				if (!bHaveReference)
				{
					ReferenceColors = MoveTemp(ComponentColors);
					bHaveReference = true;
				}
				else if (ComponentColors != ReferenceColors)
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptSourceTopologyDivergentBaselineFormat",
							"'{0}': different instances of this asset produced different Preview results (World Space Bounding Box and/or Ambient Occlusion depend on each instance's own transform), so writing to the shared Static Mesh asset would be ambiguous. Accept is blocked for this operation; make the instances' transforms/results consistent, or Cancel."),
						FText::FromString(Entry->AssetName));
					return false;
				}
			}

			if (!bHaveReference)
			{
				continue;
			}

			// Correspondence check: FinalColors must match the CURRENT corner count exactly -- never
			// approximate -- and the full TriIDMap -> FTriangleID -> VertexInstanceID chain must be
			// formally valid against the LIVE MeshDescription (see ValidateSourceTopologyCorrespondence
			// for the complete list of what this proves).
			const int32 NumCorners = Entry->WorkingMesh.Mesh->TriangleCount() * 3;
			const FMeshDescription* LiveMeshDescription = Mesh->GetMeshDescription(0);
			if (!LiveMeshDescription || ReferenceColors.Num() != NumCorners)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyCorrespondenceFormat",
						"'{0}': the triangle/corner correspondence needed to write vertex colors is unavailable or stale. Try Refresh Selection and Generate Mask again."),
					FText::FromString(Entry->AssetName));
				return false;
			}
			if (!ValidateSourceTopologyCorrespondence(
				*Entry->WorkingMesh.Mesh, Entry->WorkingMesh.TriIDMap, *LiveMeshDescription, Entry->AssetName, OutErrorText))
			{
				return false;
			}

			FVertexMaskForgeSourceTopologyAcceptTarget Target;
			Target.Mesh = Mesh;
			Target.AssetName = Entry->AssetName;
			Target.FinalColors = MoveTemp(ReferenceColors);
			Target.Entry = Entry;
			OutTargets.Add(MoveTemp(Target));
		}

		return true;
	}

	/**
	 * Sibling of WriteAcceptTargets for Source-Topology entries. Writes ONLY VertexInstanceColors on
	 * the SOURCE MeshDescription (Mesh->GetMeshDescription(0)) -- never positions, topology, normals,
	 * UVs, polygon groups, or any other attribute -- via the TriangleID+corner correspondence
	 * (Entry->WorkingMesh.TriIDMap), reproducing exactly what
	 * UE::Geometry::FDynamicMeshToMeshDescription::UpdateVertexColors does for the native Paint Vertex
	 * Colors tool's own commit (see the native-tool audit), without re-running a full mesh conversion
	 * (which could risk touching other attributes). Same two-pass (re-validate everything, THEN write)
	 * discipline as WriteAcceptTargets -- if this returns false, nothing was modified.
	 *
	 * AUDITED (Undo/Redo fix): like its render-vertex sibling, does NOT open its own FScopedTransaction
	 * or call Mesh->Build() -- see WriteAcceptTargets' own doc comment for why (single outer
	 * transaction owned by AcceptPendingChanges, rebuild deferred to BuildModifiedMeshes() after that
	 * transaction closes) and for the ModifyMeshDescription() root-cause explanation, which applies
	 * identically here.
	 */
	static bool WriteSourceTopologyAcceptTargets(const TArray<FVertexMaskForgeSourceTopologyAcceptTarget>& Targets, TArray<UStaticMesh*>& OutModifiedMeshes, FText& OutErrorText)
	{
		using namespace UE::Geometry;

		for (const FVertexMaskForgeSourceTopologyAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			const bool bEntryValid = Target.Entry.IsValid() && Target.Entry->WorkingMesh.Mesh.IsValid()
				&& !Target.Entry->WorkingMesh.TriIDMap.IsEmpty();
			const FMeshDescription* MeshDescription = IsValid(Mesh) ? Mesh->GetMeshDescription(0) : nullptr;
			const int32 NumCorners = bEntryValid ? Target.Entry->WorkingMesh.Mesh->TriangleCount() * 3 : 0;

			if (!MeshDescription || !bEntryValid || Target.FinalColors.Num() != NumCorners)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyWriteRevalidationFailedFormat", "'{0}' failed re-validation immediately before writing; aborting Accept (nothing was modified)."),
					FText::FromString(Target.AssetName));
				return false;
			}
			// AUDITED (commit preflight correction): full correspondence re-check, same as
			// BuildSourceTopologyAcceptTargets' own preflight -- nothing else can have touched these
			// assets between preflight and here (synchronous, same call), but re-proving it immediately
			// before the first Modify() matches WriteAcceptTargets' own re-validation discipline exactly.
			if (!ValidateSourceTopologyCorrespondence(
				*Target.Entry->WorkingMesh.Mesh, Target.Entry->WorkingMesh.TriIDMap, *MeshDescription, Target.AssetName, OutErrorText))
			{
				return false;
			}
		}

		for (const FVertexMaskForgeSourceTopologyAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			const FDynamicMesh3& WorkingDynamicMesh = *Target.Entry->WorkingMesh.Mesh;
			const TArray<FTriangleID>& TriIDMap = Target.Entry->WorkingMesh.TriIDMap;

			// AUDITED (Undo/Redo fix): see WriteAcceptTargets' own doc comment -- ModifyMeshDescription
			// is the call that actually makes the source data participate in the Transaction Buffer.
			Mesh->SetFlags(RF_Transactional);
			Mesh->Modify();
			Mesh->ModifyMeshDescription(0);

			FStaticMeshAttributes Attributes(*MeshDescription);
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

			int32 CornerIndex = 0;
			for (const int32 TriangleID : WorkingDynamicMesh.TriangleIndicesItr())
			{
				if (!TriIDMap.IsValidIndex(TriangleID))
				{
					// Re-validated above; never reachable in practice, but never crash or misalign the
					// remaining corners if it somehow were.
					CornerIndex += 3;
					continue;
				}
				const FTriangleID SourceTriangleID = TriIDMap[TriangleID];
				const TArrayView<const FVertexInstanceID> SourceInstances = MeshDescription->GetTriangleVertexInstances(SourceTriangleID);
				if (SourceInstances.Num() != 3)
				{
					CornerIndex += 3;
					continue;
				}
				for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
				{
					if (Target.FinalColors.IsValidIndex(CornerIndex))
					{
						Colors[SourceInstances[Corner]] = FLinearColor(Target.FinalColors[CornerIndex]);
					}
				}
			}

			Mesh->CommitMeshDescription(0);

			// AUDITED (BUG FIX round -- persistence verification, per explicit requirement): re-read
			// VertexInstanceColors from the LIVE MeshDescription (re-fetched, not the stale local
			// pointer/attributes-ref from before Commit) and compare against what was just written, via
			// the EXACT SAME TriangleID+corner walk -- never trust the preview's appearance alone as
			// proof the asset was actually updated. Aborts BEFORE Build()/notifying anything if the
			// write did not actually stick.
			{
				const FMeshDescription* VerifyMeshDescription = Mesh->GetMeshDescription(0);
				const FStaticMeshConstAttributes VerifyAttributes(*VerifyMeshDescription);
				const TVertexInstanceAttributesConstRef<FVector4f> VerifyColors = VerifyAttributes.GetVertexInstanceColors();

				int32 VerifyCornerIndex = 0;
				int32 NumMismatched = 0;
				for (const int32 TriangleID : WorkingDynamicMesh.TriangleIndicesItr())
				{
					if (!TriIDMap.IsValidIndex(TriangleID)) { VerifyCornerIndex += 3; continue; }
					const FTriangleID VerifySourceTriangleID = TriIDMap[TriangleID];
					const TArrayView<const FVertexInstanceID> VerifySourceInstances = VerifyMeshDescription->GetTriangleVertexInstances(VerifySourceTriangleID);
					if (VerifySourceInstances.Num() != 3) { VerifyCornerIndex += 3; continue; }
					for (int32 Corner = 0; Corner < 3; ++Corner, ++VerifyCornerIndex)
					{
						if (!Target.FinalColors.IsValidIndex(VerifyCornerIndex)) { continue; }
						const FVector4f Expected(FLinearColor(Target.FinalColors[VerifyCornerIndex]));
						const FVector4f Actual = VerifyColors.Get(VerifySourceInstances[Corner]);
						if (!Expected.Equals(Actual, 1.0f / 512.0f))
						{
							++NumMismatched;
						}
					}
				}

				if (NumMismatched > 0)
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptSourceTopologyPersistenceVerificationFailedFormat",
							"'{0}': {1} Vertex Instance color(s) did not match what was written immediately after CommitMeshDescription; aborting Accept before Build/notify (the write did not persist as expected)."),
						FText::FromString(Target.AssetName), FText::AsNumber(NumMismatched));
					UE_LOG(LogVertexMaskForge, Error,
						TEXT("Vertex Mask Forge: Accept (Source Topology) persistence verification FAILED for '%s' -- %d mismatched Vertex Instance color(s)."),
						*Target.AssetName, NumMismatched);
					return false;
				}
			}

			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: Accept (Source Topology) -- '%s': colors written, CommitMeshDescription succeeded, persistence verified."),
				*Target.AssetName);

			OutModifiedMeshes.Add(Mesh);
		}

		return true;
	}

	/**
	 * Rebuilds RenderData (and Nanite cluster data, for Nanite-enabled assets -- same build pass, see
	 * the native Nanite build audit: MeshBuilderModule.BuildMesh() reads VertexInstanceColors directly
	 * from the same source MeshDescription when constructing Nanite's cluster input) for every mesh
	 * whose MeshDescription was just committed by WriteAcceptTargets/WriteSourceTopologyAcceptTargets.
	 *
	 * AUDITED (Undo/Redo fix): called by AcceptPendingChanges AFTER its outer FScopedTransaction has
	 * already closed -- mirrors UE::ToolTarget::Internal::PostEditChangeWithConditionalUndo, the
	 * audited native reference (Modeling Tools' own StaticMeshToolTarget::CommitMeshDescription commit
	 * sequence): RenderData/Nanite data is DERIVED, deterministically regenerated from the (now
	 * transacted) MeshDescription on both Accept and Undo/Redo (UStaticMesh::PostEditUndo() ->
	 * Super::PostEditUndo() -> PostEditChangeProperty() -> Build(), the exact same derivation) -- so it
	 * must never itself be captured inside a transaction. GUndo is suppressed for the duration of each
	 * Build() call, exactly as the native pattern does, so an already-closed outer transaction (or the
	 * complete absence of one) can never be reopened or polluted by whatever Modify() calls Build()
	 * triggers internally (e.g. component re-registration).
	 */
	static bool BuildModifiedMeshes(const TArray<UStaticMesh*>& Meshes, FText& OutErrorText)
	{
		for (UStaticMesh* Mesh : Meshes)
		{
			if (!IsValid(Mesh))
			{
				continue;
			}

			TGuardValue<ITransaction*> SuppressTransaction(GUndo, nullptr);
			TArray<FText> BuildErrors;
			Mesh->Build(/*bInSilent=*/true, &BuildErrors);
			if (!BuildErrors.IsEmpty())
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptBuildFailedFormat", "'{0}': Build failed after committing Vertex Colors: {1}"),
					FText::FromString(Mesh->GetName()), BuildErrors[0]);
				UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept Build FAILED for '%s': %s"),
					*Mesh->GetName(), *BuildErrors[0].ToString());
				return false;
			}
			UE_LOG(LogVertexMaskForge, Log, TEXT("Vertex Mask Forge: Accept -- '%s': Build completed."), *Mesh->GetName());
		}

		return true;
	}

	/** Loads the engine's own built-in Vertex Color debug material. Never creates a new asset. */
	static UMaterialInterface* LoadPreviewDebugMaterial()
	{
		return Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(),
			nullptr,
			TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial")));
	}

	/**
	 * Creates (if not already created) the transient duplicate component used to visualize the
	 * preview. Outer is GetTransientPackage() and it is never added to SourceComponent's owning
	 * Actor's component list (only attached to it for transform propagation), so no Actor/level
	 * serialization path can ever discover it -- see the audit note on FVertexMaskForgePreviewComponentState.
	 * No collision, no shadow, not selectable: purely visual. Attached with an identity relative
	 * transform so it always exactly tracks SourceComponent's world transform, including live edits.
	 *
	 * AUDITED: USceneComponent::AttachParent is an ordinary (non-transient) UPROPERTY, so the new
	 * component's own AttachParent = SourceComponent link would in principle be serializable -- but
	 * only if the new component itself were ever reachable from something a Save actually writes,
	 * which it is not (RF_Transient, Outer = GetTransientPackage(), never added to any Actor's
	 * OwnedComponents/BlueprintCreatedComponents). Conversely, SourceComponent->AttachChildren (which
	 * gains an entry pointing at the new component) IS `UPROPERTY(Transient)` on SceneComponent.h, so
	 * even though SourceComponent itself is a normally-serialized component, this specific back-
	 * reference is skipped by serialization. Neither direction of the attachment relationship can be
	 * persisted by a Save while preview is active.
	 *
	 * Also takes strong ownership (TStrongObjectPtr) immediately, before any further failure point,
	 * so a Collect Garbage cannot reclaim the object out from under a partially-completed setup.
	 */
	static UStaticMeshComponent* EnsurePreviewComponent(FVertexMaskForgePreviewComponentState& State)
	{
		if (UStaticMeshComponent* Existing = State.PreviewComponent.Get())
		{
			return Existing;
		}

		UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
		if (!IsValid(SourceComponent) || !IsValid(SourceComponent->GetWorld()))
		{
			return nullptr;
		}

		UStaticMeshComponent* NewPreviewComponent = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		if (!NewPreviewComponent)
		{
			return nullptr;
		}

		// Strong reference taken up front: from this point on, any early return below must destroy
		// NewPreviewComponent explicitly (letting StrongPreviewComponent go out of scope only drops
		// the reference; it does not detach/unregister/destroy the component).
		TStrongObjectPtr<UStaticMeshComponent> StrongPreviewComponent(NewPreviewComponent);

		NewPreviewComponent->SetStaticMesh(SourceComponent->GetStaticMesh());
		NewPreviewComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		NewPreviewComponent->SetCastShadow(false);
		NewPreviewComponent->bSelectable = false;
		NewPreviewComponent->SetMobility(EComponentMobility::Movable);

		// AUDITED (Nanite preview fix): Nanite's runtime renderer does not read
		// FStaticMeshComponentLODInfo::OverrideVertexColors at all -- only the non-Nanite fallback
		// rendering path does. Forcing ONLY this transient PreviewComponent onto its fallback mesh
		// (same mechanism the Static Mesh Editor's own "Show Nanite Fallback" viewport toggle uses,
		// see SStaticMeshEditorViewport::ToggleShowNaniteFallback -- per-component only, never touches
		// SourceComponent or the asset's own Nanite settings/data) makes the instance-override preview
		// visible again for Nanite-enabled assets. Harmless no-op for non-Nanite assets.
		if (const UStaticMesh* PreviewMesh = NewPreviewComponent->GetStaticMesh())
		{
			if (PreviewMesh->IsNaniteEnabled())
			{
				NewPreviewComponent->SetForceDisableNanite(true);
			}
		}

		NewPreviewComponent->SetupAttachment(SourceComponent);
		NewPreviewComponent->SetRelativeTransform(FTransform::Identity);

		NewPreviewComponent->RegisterComponentWithWorld(SourceComponent->GetWorld());
		if (!NewPreviewComponent->IsRegistered())
		{
			// Partial failure (Problem 5): registration did not take. Undo the attachment and destroy
			// outright rather than leaving an unregistered, attached, strongly-referenced component.
			// Safe to call DetachFromComponent() directly here (unlike RestoreComponentOriginal's use
			// of DetachAndDestroyPreviewComponent()): IsRegistered() just returned false, so
			// SceneComponent.cpp's `!bRegistered || AttachChildren.Contains(this)` ensure is
			// vacuously satisfied regardless of the parent's AttachChildren state.
			NewPreviewComponent->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
			NewPreviewComponent->DestroyComponent();
			return nullptr;
		}

		State.PreviewComponent = MoveTemp(StrongPreviewComponent);
		return NewPreviewComponent;
	}

	/**
	 * Writes render-order preview colors into the transient PreviewComponent's own
	 * OverrideVertexColors and configures its material slots. PreviewComponent is always our own
	 * freshly-created object, so there is no pre-existing state to snapshot or preserve here -- unlike
	 * SourceComponent, which this function never touches (reads its materials only, when
	 * bUseOriginalMaterials).
	 *
	 * AUDITED (Original Textures fix): when bUseOriginalMaterials is true (Preview Mode ==
	 * OriginalMaterial), every slot is set to SourceComponent->GetMaterial(SlotIndex) instead of
	 * DebugMaterial -- same slot count/order as SourceComponent (PreviewComponent->GetNumMaterials()
	 * already mirrors it exactly, since EnsurePreviewComponent's SetStaticMesh gives it the identical
	 * mesh asset/material slot layout, and per-instance overrides are resolved by GetMaterial() itself).
	 * OverrideVertexColors is populated identically regardless of mode -- the transient PreviewComponent
	 * always shows the CURRENT WorkingColors, live; only which material reads/reduces that data differs.
	 */
	static void ApplyPreviewColorsToPreviewComponent(
		UStaticMeshComponent* PreviewComponent,
		const TArray<FColor>& RenderOrderColors,
		UMaterialInterface* DebugMaterial,
		const UStaticMeshComponent* SourceComponent,
		const bool bUseOriginalMaterials)
	{
		if (!IsValid(PreviewComponent) || (!bUseOriginalMaterials && !DebugMaterial))
		{
			return;
		}

		PreviewComponent->SetLODDataCount(1, FMath::Max(PreviewComponent->LODData.Num(), 1));
		if (!PreviewComponent->LODData.IsValidIndex(0))
		{
			return;
		}

		FStaticMeshComponentLODInfo& LODInfo = PreviewComponent->LODData[0];
		LODInfo.ReleaseOverrideVertexColorsAndBlock();
		LODInfo.OverrideVertexColors = new FColorVertexBuffer();
		LODInfo.OverrideVertexColors->InitFromColorArray(RenderOrderColors.GetData(), RenderOrderColors.Num());
		BeginInitResource(LODInfo.OverrideVertexColors);

		const int32 NumSlots = PreviewComponent->GetNumMaterials();
		for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
		{
			UMaterialInterface* SlotMaterial = (bUseOriginalMaterials && IsValid(SourceComponent))
				? SourceComponent->GetMaterial(SlotIndex)
				: DebugMaterial;
			PreviewComponent->SetMaterial(SlotIndex, SlotMaterial);
		}

		UE_LOG(LogVertexMaskForge, Verbose,
			TEXT("Vertex Mask Forge: Preview (Render Vertex) -- component=%s, materials=%d, colors=%d, originalTextures=%s."),
			*PreviewComponent->GetName(), NumSlots, RenderOrderColors.Num(),
			bUseOriginalMaterials ? TEXT("true") : TEXT("false"));

		PreviewComponent->MarkRenderStateDirty();
	}

	/**
	 * AUDITED (Nanite source-topology support): sibling of EnsurePreviewComponent for Source-Topology
	 * entries -- a transient UDynamicMeshComponent instead of UStaticMeshComponent, since that is the
	 * mechanism UE's own Paint Vertex Colors tool uses for its live preview (renders an FDynamicMesh3
	 * directly; never depends on Nanite/OverrideVertexColors at all -- see the native-tool audit).
	 * SourceMesh is COPIED (never moved -- WorkingMesh.Mesh is entry-level, shared by every component of
	 * this entry, and is still needed later for Accept) into the new component's own UDynamicMesh, since
	 * each component's composed colors can legitimately differ (World Space Bounding Box axes and
	 * Ambient Occlusion are both transform-dependent, evaluated per component) even though the
	 * TOPOLOGY/positions are identical across every component of the same entry. Same ownership/
	 * lifetime discipline as EnsurePreviewComponent (TStrongObjectPtr, RF_Transient, attached to
	 * SourceComponent for transform propagation only, never added to any Actor's serialized component
	 * list) -- see that function's own doc comment for the full audit.
	 */
	static UDynamicMeshComponent* EnsureSourceTopologyPreviewComponent(
		FVertexMaskForgePreviewComponentState& State,
		const UE::Geometry::FDynamicMesh3& SourceMesh)
	{
		if (UDynamicMeshComponent* Existing = State.SourceTopologyPreviewComponent.Get())
		{
			return Existing;
		}

		UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
		if (!IsValid(SourceComponent) || !IsValid(SourceComponent->GetWorld()))
		{
			return nullptr;
		}

		UDynamicMeshComponent* NewPreviewComponent = NewObject<UDynamicMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		if (!NewPreviewComponent)
		{
			return nullptr;
		}

		TStrongObjectPtr<UDynamicMeshComponent> StrongPreviewComponent(NewPreviewComponent);

		NewPreviewComponent->SetMesh(UE::Geometry::FDynamicMesh3(SourceMesh));
		// AUDITED (Original Textures fix): ColorOverrideMode::VertexColors (the previous setting) does
		// NOT merely "make vertex colors visible" -- FBaseDynamicMeshSceneProxy::GetViewRelevance/
		// GetDynamicMeshElements force EVERY triangle's material to
		// UBaseDynamicMeshComponent::GetDefaultVertexColorMaterial_RenderThread() (the ENGINE's own
		// global vertex-color-view-mode material, GEngine->VertexColorViewModeMaterial_ColorOnly) --
		// completely ignoring whatever material this component's own ConfigureMaterialSet configured,
		// debug or original. This silently worked for the RGB/Channel debug modes only because that
		// engine material happens to be visually similar to this plugin's own DebugMaterial (loaded
		// from the same "/Engine/EngineDebugMaterials/VertexColorMaterial" asset) -- but it makes
		// "Original Textures" impossible: the real per-slot material, once configured, would still
		// never actually render. ColorOverrideMode::None does not affect vertex color upload at all
		// (MeshRenderBufferSetConverter.bIgnoreVertexColors is only set true for Constant mode) -- the
		// Primary Color Overlay is always converted into the render vertex color buffer regardless of
		// this setting; None only stops the material force-override, letting whichever material
		// ApplySourceTopologyColorsToPreviewComponent actually configures (debug or original) genuinely
		// render and read that vertex color data via its own material graph.
		NewPreviewComponent->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::None);
		NewPreviewComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		NewPreviewComponent->SetCastShadow(false);
		NewPreviewComponent->bSelectable = false;
		NewPreviewComponent->SetMobility(EComponentMobility::Movable);

		NewPreviewComponent->SetupAttachment(SourceComponent);
		NewPreviewComponent->SetRelativeTransform(FTransform::Identity);

		NewPreviewComponent->RegisterComponentWithWorld(SourceComponent->GetWorld());
		if (!NewPreviewComponent->IsRegistered())
		{
			NewPreviewComponent->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
			NewPreviewComponent->DestroyComponent();
			return nullptr;
		}

		State.SourceTopologyPreviewComponent = MoveTemp(StrongPreviewComponent);
		return NewPreviewComponent;
	}

	/**
	 * Writes SourceTopologyWorkingColors into the preview component's own Primary Color Overlay. The
	 * overlay is fully rebuilt every call (cleared, then one AppendElement + SetTriangle per triangle
	 * corner, in the SAME Mesh.TriangleIndicesItr()-then-corner-0..2 order UpdateWorkingColorsSourceTopology
	 * used to build WorkingColors) -- so ElementID == CornerIndex by construction, needing no separate
	 * persisted lookup. Cheap relative to a raycast pass; simpler and less error-prone than maintaining
	 * a stable per-corner element mapping across updates.
	 *
	 * AUDITED (Original Textures fix): the material set configured here is what actually renders now
	 * that EnsureSourceTopologyPreviewComponent leaves ColorOverrideMode at None (see its own doc
	 * comment) -- previously it was silently discarded by the ColorOverrideMode force-override. When
	 * bUseOriginalMaterials is true, one entry per SourceComponent material slot is configured
	 * (SourceComponent->GetMaterial(i), preserving slot count/order/instances/overrides exactly), so
	 * the mesh's own per-triangle MaterialID attribute (populated from the source's polygon groups at
	 * working-mesh build time -- see ValidateWorkingMesh) selects the correct original material per
	 * triangle, exactly matching the source asset's sections. Otherwise (debug modes), DebugMaterial is
	 * applied to every slot via a single-entry material set, same as before.
	 */
	static void ApplySourceTopologyColorsToPreviewComponent(
		UDynamicMeshComponent* PreviewComponent,
		const TArray<FColor>& WorkingColors,
		UMaterialInterface* DebugMaterial,
		const UStaticMeshComponent* SourceComponent,
		const bool bUseOriginalMaterials)
	{
		using namespace UE::Geometry;

		if (!IsValid(PreviewComponent))
		{
			return;
		}

		PreviewComponent->EditMesh([&WorkingColors](FDynamicMesh3& Mesh)
		{
			if (!Mesh.HasAttributes())
			{
				Mesh.EnableAttributes();
			}
			// Rebuild fresh every call -- see the function's own doc comment.
			Mesh.Attributes()->DisablePrimaryColors();
			Mesh.Attributes()->EnablePrimaryColors();
			FDynamicMeshColorOverlay* ColorOverlay = Mesh.Attributes()->PrimaryColors();
			if (!ColorOverlay)
			{
				return;
			}

			int32 CornerIndex = 0;
			for (const int32 TriangleID : Mesh.TriangleIndicesItr())
			{
				FIndex3i ElementTri;
				for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
				{
					const FColor& Color = WorkingColors.IsValidIndex(CornerIndex) ? WorkingColors[CornerIndex] : FColor::White;
					const FVector4f ColorF(Color.R / 255.f, Color.G / 255.f, Color.B / 255.f, Color.A / 255.f);
					ElementTri[Corner] = ColorOverlay->AppendElement(ColorF);
				}
				ColorOverlay->SetTriangle(TriangleID, ElementTri);
			}
		});

		if (bUseOriginalMaterials && IsValid(SourceComponent))
		{
			const int32 NumSlots = SourceComponent->GetNumMaterials();
			TArray<UMaterialInterface*> OriginalMaterials;
			OriginalMaterials.Reserve(FMath::Max(NumSlots, 1));
			for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
			{
				OriginalMaterials.Add(SourceComponent->GetMaterial(SlotIndex));
			}
			if (OriginalMaterials.IsEmpty())
			{
				// No slots at all (pathological content only): configure nothing rather than an empty
				// set ConfigureMaterialSet might not handle gracefully.
			}
			else
			{
				PreviewComponent->ConfigureMaterialSet(OriginalMaterials);
			}
		}
		else if (DebugMaterial)
		{
			PreviewComponent->ConfigureMaterialSet(TArray<UMaterialInterface*>{ DebugMaterial });
		}

		UE_LOG(LogVertexMaskForge, Verbose,
			TEXT("Vertex Mask Forge: Preview (Source Topology) -- component=%s, materials=%d, colors=%d, originalTextures=%s."),
			*PreviewComponent->GetName(), PreviewComponent->GetNumMaterials(), WorkingColors.Num(),
			bUseOriginalMaterials ? TEXT("true") : TEXT("false"));

		PreviewComponent->FastNotifyColorsUpdated();
		PreviewComponent->SetVisibility(true);
	}

	/** Sibling of DetachAndDestroyPreviewComponent for the Source-Topology preview component --
	 *  identical attachment-consistency handling, see that function's own audit note. */
	static void DetachAndDestroySourceTopologyPreviewComponent(UDynamicMeshComponent* PreviewComponentPtr)
	{
		if (!PreviewComponentPtr)
		{
			return;
		}

		USceneComponent* PreviewAttachParent = PreviewComponentPtr->GetAttachParent();
		const bool bConsistentlyAttached =
			PreviewAttachParent && PreviewAttachParent->GetAttachChildren().Contains(PreviewComponentPtr);

		if (PreviewComponentPtr->IsRegistered() && PreviewAttachParent && !bConsistentlyAttached)
		{
			PreviewComponentPtr->UnregisterComponent();
		}
		if (PreviewAttachParent)
		{
			PreviewComponentPtr->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
		}
		PreviewComponentPtr->DestroyComponent();
	}

	/**
	 * Acquires one hide-reference on Owner: hides it (capturing its original
	 * IsTemporarilyHiddenInEditor() value) only on the transition from 0 to 1 active references, via
	 * AActor::SetIsTemporarilyHiddenInEditor() -- the one editor visibility mechanism confirmed to be
	 * UPROPERTY(Transient) (AActor::bHiddenEdTemporary, "used for show/hide/etc functionality w/o
	 * dirtying the actor"; see Actor.h). Safe to call multiple times for the same Actor (e.g. two
	 * previewed components on one Actor): only the first caller's snapshot is kept, so a later
	 * release from any one caller can never un-hide an Actor another active preview still depends on.
	 */
	static void AcquireActorHidden(
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates,
		AActor* Owner)
	{
		if (!IsValid(Owner))
		{
			return;
		}

		FVertexMaskForgeActorHideState& HideState = ActorHideStates.FindOrAdd(TWeakObjectPtr<AActor>(Owner));
		if (HideState.RefCount == 0)
		{
			HideState.bOriginalHiddenInEditor = Owner->IsTemporarilyHiddenInEditor();
			Owner->SetIsTemporarilyHiddenInEditor(true);
		}
		++HideState.RefCount;
	}

	/**
	 * Releases one hide-reference on Owner. Only the release that brings the count to zero actually
	 * restores Owner's original IsTemporarilyHiddenInEditor() value and removes the map entry; a
	 * partially-failed acquire (RefCount already 0, entry missing) safely no-ops rather than
	 * decrementing past zero or restoring a value that was never captured.
	 */
	static void ReleaseActorHidden(
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates,
		AActor* Owner)
	{
		if (!IsValid(Owner))
		{
			return;
		}

		FVertexMaskForgeActorHideState* HideState = ActorHideStates.Find(TWeakObjectPtr<AActor>(Owner));
		if (!HideState)
		{
			return;
		}

		--HideState->RefCount;
		if (HideState->RefCount <= 0)
		{
			Owner->SetIsTemporarilyHiddenInEditor(HideState->bOriginalHiddenInEditor);
			ActorHideStates.Remove(TWeakObjectPtr<AActor>(Owner));
		}
	}

	/**
	 * Activates (or refreshes) the preview for one source component: ensures the transient
	 * duplicate exists and is up to date, then acquires a hide-reference (see AcquireActorHidden) on
	 * SourceComponent's owning Actor. The acquire happens only once per State (bHasAcquiredActorHide
	 * guards it), so repeated updates (e.g. toggling Channel Filter) don't re-acquire.
	 *
	 * Known limitation: hiding is Actor-level, not per-component. If that Actor has other components
	 * not part of the current Vertex Mask Forge selection, they are hidden too while any preview
	 * referencing that Actor is active (and correctly restored once the last one releases). There is
	 * no component-level equivalent of bHiddenEdTemporary in UE 5.8 (SceneComponent::bVisible is not
	 * transient), so this Actor-level granularity is the safest option that cannot be persisted by a
	 * Save.
	 */
	static void ActivatePreviewForComponent(
		FVertexMaskForgePreviewComponentState& State,
		const TArray<FColor>& RenderOrderColors,
		UMaterialInterface* DebugMaterial,
		const bool bUseOriginalMaterials,
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates)
	{
		UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
		if (!IsValid(SourceComponent))
		{
			return;
		}

		UStaticMeshComponent* PreviewComponent = EnsurePreviewComponent(State);
		if (!PreviewComponent)
		{
			return;
		}

		ApplyPreviewColorsToPreviewComponent(PreviewComponent, RenderOrderColors, DebugMaterial, SourceComponent, bUseOriginalMaterials);
		PreviewComponent->SetVisibility(true);

		// Deliberately the LAST step: the hide-reference is only acquired once PreviewComponent
		// exists, is registered, and has been fed colors/material -- i.e. once there is something
		// valid to show in place of the original. There is no failure point after this in the
		// function (AcquireActorHidden itself cannot fail: SetIsTemporarilyHiddenInEditor() is a
		// simple property assignment, not an allocation or engine subsystem call), so no rollback
		// path is needed for "failure after acquiring the hide token".
		if (!State.bHasAcquiredActorHide)
		{
			AActor* Owner = SourceComponent->GetOwner();
			AcquireActorHidden(ActorHideStates, Owner);
			State.HiddenOwner = Owner;
			State.bHasAcquiredActorHide = true;
		}

		State.bOverrideActive = true;
	}

	/**
	 * AUDITED (Nanite source-topology support): sibling of ActivatePreviewForComponent for Source-
	 * Topology entries -- same Actor-hide acquisition contract and same "known limitation" (Actor-level
	 * hide, not per-component; see ActivatePreviewForComponent's own doc comment -- this plugin has no
	 * transient-safe component-level visibility flag available in UE 5.8, so this is not a regression
	 * specific to Nanite, it is the same pre-existing, documented trade-off the render-vertex preview
	 * already has). WorkingColors here is SourceTopologyWorkingColors (corner domain), never
	 * DeriveDisplayColors-reduced -- Preview Mode display reduction (Red/Green/Blue/Alpha Channel) is
	 * intentionally NOT implemented for the Source-Topology preview in this checkpoint (RGB Vertex
	 * Color only); the underlying WorkingColors data Accept reads is unaffected either way, matching the
	 * render-vertex path's own "Preview Mode never affects Accept" guarantee.
	 *
	 * AUDITED (Original Textures fix): bUseOriginalMaterials forwarded verbatim to
	 * ApplySourceTopologyColorsToPreviewComponent -- see that function's own doc comment. This is the
	 * SAME transient PreviewComponent used for every Preview Mode (debug or original); Original
	 * Textures no longer tears it down (see ApplyPreviewToEntry's own doc comment on removing the old
	 * OriginalMaterial early-return).
	 */
	static void ActivateSourceTopologyPreviewForComponent(
		FVertexMaskForgePreviewComponentState& State,
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const TArray<FColor>& WorkingColors,
		UMaterialInterface* DebugMaterial,
		const bool bUseOriginalMaterials,
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates)
	{
		UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
		if (!IsValid(SourceComponent))
		{
			return;
		}

		UDynamicMeshComponent* PreviewComponent = EnsureSourceTopologyPreviewComponent(State, SourceMesh);
		if (!PreviewComponent)
		{
			return;
		}

		ApplySourceTopologyColorsToPreviewComponent(PreviewComponent, WorkingColors, DebugMaterial, SourceComponent, bUseOriginalMaterials);

		if (!State.bHasAcquiredActorHide)
		{
			AActor* Owner = SourceComponent->GetOwner();
			AcquireActorHidden(ActorHideStates, Owner);
			State.HiddenOwner = Owner;
			State.bHasAcquiredActorHide = true;
		}

		State.bOverrideActive = true;
	}

	/**
	 * Detaches and destroys ONE PreviewComponent, tolerant of an inconsistent attachment bookkeeping
	 * state.
	 *
	 * AUDITED (Cancel Ensure fix): USceneComponent::DetachFromComponent() (SceneComponent.cpp:2692)
	 * asserts `!bRegistered || GetAttachParent()->GetAttachChildren().Contains(this)` -- i.e. if the
	 * component is still registered, its AttachParent must actually list it back as a child, or the
	 * ensure fires ("Attempt to detach SceneComponent ... while not attached"). Nothing in this
	 * plugin ever removes PreviewComponent from SourceComponent->AttachChildren directly, but that
	 * bookkeeping is engine-owned and can diverge from PreviewComponent's own AttachParent pointer
	 * for reasons entirely outside this plugin's control (e.g. SourceComponent going through an
	 * external Unregister/Reregister cycle -- triggered by ordinary editor operations such as a
	 * property edit, construction script rerun, or any FComponentReregisterContext -- rebuilds
	 * SourceComponent's OWN AttachChildren from its OWNED components only; a foreign, RF_Transient
	 * preview component attached only via SetupAttachment()/RegisterComponentWithWorld() is never
	 * part of that rebuild, so it silently drops out of the parent's list while its own AttachParent
	 * pointer -- a field local to itself -- is left unchanged).
	 *
	 * The fix: verify BEFORE calling DetachFromComponent() that the parent still actually lists this
	 * component. If it does not, and the component is still registered, calling DetachFromComponent()
	 * directly would retrigger the exact ensure above. UnregisterComponent() has no such attachment
	 * precondition (safe to call unconditionally), and once bRegistered is false, the ensure's own
	 * `!bRegistered || ...` guard is vacuously satisfied -- so unregistering FIRST makes the
	 * subsequent DetachFromComponent() call (which still correctly clears this component's own
	 * AttachParent/AttachSocketName bookkeeping) safe regardless of the parent's list state.
	 */
	static void DetachAndDestroyPreviewComponent(UStaticMeshComponent* PreviewComponentPtr)
	{
		if (!PreviewComponentPtr)
		{
			return;
		}

		USceneComponent* PreviewAttachParent = PreviewComponentPtr->GetAttachParent();
		const bool bConsistentlyAttached =
			PreviewAttachParent && PreviewAttachParent->GetAttachChildren().Contains(PreviewComponentPtr);

		if (PreviewComponentPtr->IsRegistered() && PreviewAttachParent && !bConsistentlyAttached)
		{
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: PreviewComponent '%s' attachment bookkeeping had already diverged from its parent's AttachChildren -- unregistering before detach to avoid SceneComponent.cpp's attachment-consistency ensure."),
				*PreviewComponentPtr->GetName());
			PreviewComponentPtr->UnregisterComponent();
		}

		if (PreviewAttachParent)
		{
			// Safe unconditionally at this point: either it was already consistently attached while
			// registered (the normal path), or it has just been unregistered above (bRegistered is
			// now false, so DetachFromComponent's own ensure guard is vacuously satisfied), or it was
			// never registered in the first place.
			PreviewComponentPtr->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
		}

		PreviewComponentPtr->DestroyComponent();
	}

	/**
	 * Restores one component's preview state, in a fixed, documented order:
	 *   1. Mark not active immediately (so the "not active" observation is atomic with the start of
	 *      cleanup, independent of how far the rest of this function gets).
	 *   2. Copy out the raw pointers this function needs locally, while State's own fields still hold
	 *      them -- each field is only read once, here.
	 *   3. If a PreviewComponent existed: detach and destroy it via DetachAndDestroyPreviewComponent()
	 *      (tolerant of divergent attachment bookkeeping -- see its own audit note), then release the
	 *      strong reference. UStaticMeshComponent::DestroyComponent() (ActorComponent.cpp) already
	 *      calls UnregisterComponent() internally whenever IsRegistered() is true, so no separate
	 *      explicit Unregister call is needed for the ordinary (consistently-attached) case.
	 *   4. Release this State's Actor hide-reference (see ReleaseActorHidden), via the HiddenOwner
	 *      copied in step 2 rather than re-deriving the Actor from SourceComponent, so the release is
	 *      correct even if SourceComponent (or its owning Actor) has since been destroyed.
	 *   5. Clear the remaining fields.
	 * Idempotent by construction: a second call finds PreviewComponent already reset and
	 * bHasAcquiredActorHide already false, so steps 3 and 4 both no-op. Safe to call regardless of
	 * whether SourceComponent/its Actor/its World are still valid -- this function never dereferences
	 * SourceComponent at all.
	 */
	/**
	 * Steps 1-5 ONLY (visual/attachment/actor-hide restore) -- see RestoreComponentOriginal's own doc
	 * comment for the full step list. Deliberately does NOT touch BaselineColors/CommittedColors/
	 * WorkingColors/AOCache.
	 *
	 * AUDITED (raw/composition separation checkpoint, AOCache lifetime fix): this is what
	 * ApplyPreviewToEntry now calls for a MOMENTARY, mid-session fallback (a per-component layer
	 * re-evaluation came back not-Ready, or the resolved Layers list is simply empty because no layer
	 * is currently enabled) -- neither case is a genuine geometric invalidation or a session end, so
	 * destroying AOCache (or the color arrays) there would be wrong: the checkpoint's own audit found
	 * this exact bug -- disabling the only active layer (or a per-component World-Space-degenerate
	 * BBox result while AO was perfectly fine) was destroying AO's geometry cache for no geometric
	 * reason, forcing a full Tree/raycast rebuild the next time that component became eligible again.
	 * A component visually reverted this way keeps its BaselineColors/CommittedColors/WorkingColors/
	 * AOCache exactly as they were, ready to resume the instant it becomes eligible again, with zero
	 * wasted recomputation.
	 */
	static void RestorePreviewVisualOnly(
		FVertexMaskForgePreviewComponentState& State,
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates)
	{
		// Step 1.
		State.bOverrideActive = false;

		// Step 2.
		UStaticMeshComponent* PreviewComponentPtr = State.PreviewComponent.Get();
		UDynamicMeshComponent* SourceTopologyPreviewComponentPtr = State.SourceTopologyPreviewComponent.Get();
		AActor* HiddenOwnerPtr = State.HiddenOwner.Get();
		const bool bHadAcquiredActorHide = State.bHasAcquiredActorHide;

		// Step 3. AUDITED (Nanite source-topology support): a State only ever has ONE of the two
		// preview components active at a time (see ApplyPreviewToEntry's domain branch), but both
		// teardown calls are safe/idempotent no-ops on a null pointer, so calling both unconditionally
		// here is simpler than branching and cannot double-destroy anything.
		DetachAndDestroyPreviewComponent(PreviewComponentPtr);
		State.PreviewComponent.Reset();
		DetachAndDestroySourceTopologyPreviewComponent(SourceTopologyPreviewComponentPtr);
		State.SourceTopologyPreviewComponent.Reset();

		// Step 4.
		if (bHadAcquiredActorHide)
		{
			ReleaseActorHidden(ActorHideStates, HiddenOwnerPtr);
		}

		// Step 5.
		State.bHasAcquiredActorHide = false;
		State.HiddenOwner.Reset();
	}

	/**
	 * Full session-end restore: RestorePreviewVisualOnly's steps 1-5, PLUS resetting BaselineColors/
	 * CommittedColors/WorkingColors/AOCache together (step 6) -- see those fields' own doc comments.
	 * Called ONLY when a whole session genuinely concludes or a genuine geometric invalidation demands
	 * a fresh capture: Cancel, Accept (success), a RefreshSelection about to rebuild, World cleanup
	 * (all via DestroyAllPreviews) -- never for a
	 * momentary mid-session fallback (see RestorePreviewVisualOnly's own doc comment for that case,
	 * used by ApplyPreviewToEntry instead since the raw/composition separation checkpoint).
	 */
	static void RestoreComponentOriginal(
		FVertexMaskForgePreviewComponentState& State,
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates)
	{
		RestorePreviewVisualOnly(State, ActorHideStates);

		// Step 6: the baseline snapshot, the last consolidated result, the transient working result,
		// and the AO geometry cache all belong to the session that just concluded for this component --
		// reset together, never independently. A brand new session always starts from a fresh capture
		// (this component's geometry/transform may have changed since, e.g. Accept just wrote new
		// colors, or the level was edited), never reusing a tree/raycast result computed for a
		// concluded operation.
		State.BaselineColors.Reset();
		State.CommittedColors.Reset();
		State.WorkingColors.Reset();

		// AUDITED (Nanite source-topology support): the corner-domain arrays and the Source-Topology AO
		// cache belong to the same concluded session -- reset together, same rule as the render-vertex
		// arrays above.
		State.SourceTopologyBaselineColors.Reset();
		State.SourceTopologyCommittedColors.Reset();
		State.SourceTopologyWorkingColors.Reset();

		// DIAGNOSTICS (raw/composition separation checkpoint): low-volume -- this function is only
		// ever called at genuine session-end points (Cancel, Accept, RefreshSelection, World cleanup),
		// never per-tick/per-recomposition, so Log level is safe here.
		if (State.AOCache.IsValid())
		{
			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO cache destroyed (session end/component teardown) for '%s'."),
				State.SourceComponent.IsValid() ? *State.SourceComponent->GetName() : TEXT("<invalid component>"));
		}
		State.AOCache.Reset();

		if (State.SourceTopologyAOCache.IsValid())
		{
			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO (Source Topology) cache destroyed (session end/component teardown) for '%s'."),
				State.SourceComponent.IsValid() ? *State.SourceComponent->GetName() : TEXT("<invalid component>"));
		}
		State.SourceTopologyAOCache.Reset();
	}
}

void SVertexMaskForgePanel::OnAxisParamChangedDiscrete()
{
	// AUDITED (raw/composition separation checkpoint): now exclusively used by Bounding Box's OWN
	// discrete geometric controls (Enable/Mirror/World Space checkboxes) -- per-axis Invert has its
	// OWN dedicated handler (OnAxisInvertChanged, see its own doc comment for why), Ambient Occlusion
	// and all purely compositional controls (Blend Mode, Opacity, layer Enable/Disable-when-Ready) no
	// longer call this function at all; see InvalidateBoundingBoxRawMask/InvalidateAODerivedMask/
	// RecomposeWorkingColors.
	InvalidateBoundingBoxRawMask();
	if (bAutoUpdatePreview)
	{
		// A stale, already-armed continuous-slider debounce must never apply after a discrete
		// change -- cancel it and regenerate immediately instead.
		if (GEditor)
		{
			GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
		}
		RunAutoUpdatePreview();
	}
}

void SVertexMaskForgePanel::OnAxisInvertChanged(const int32 AxisIndex, const ECheckBoxState NewState)
{
	BoundingBoxAxisParams[AxisIndex].bInvert = (NewState == ECheckBoxState::Checked);

	// AUDITED (BBox Invert exception, follow-up audit -- see the header's own doc comment for the
	// full rationale): clears ONLY BoundingBoxMask (inlined, not via InvalidateBoundingBoxRawMask, to
	// avoid that function's own conditional UpdateAllPreviews(false) firing redundantly right before
	// the unconditional regeneration below), then ALWAYS regenerates immediately -- unlike every other
	// discrete Bounding Box parameter, this does NOT wait for Auto Update Preview to be on. Ambient
	// Occlusion is never touched (RunAutoUpdatePreview's bIncludeAO=false).
	LastMaskActionStatusText = FText::GetEmpty();
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
		}
	}

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview(/*bIncludeAO=*/false);
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetBlendModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	BoundingBoxBlendMode = *NewSelection;

	// AUDITED (raw/composition separation checkpoint): Blend Mode is PURE composition -- it never
	// affects BoundingBoxMask's own raw Values, only how ComposeMaskStack reads them. Recomposes
	// immediately, exactly like Preview Mode/Channel Filter, regardless of Auto Update Preview -- no
	// raw invalidation, no raycasts, no risk of an original-color fallback for an otherwise-Ready mask.
	RecomposeWorkingColors();
}

FText SVertexMaskForgePanel::GetBlendModeButtonText() const
{
	return VertexMaskForgePanel::GetBlendModeLabel(BoundingBoxBlendMode);
}

// --- Ambient Occlusion Mask ---------------------------------------------------------------------

void SVertexMaskForgePanel::OnAOEnableChanged(const ECheckBoxState NewState)
{
	const bool bWasEnabled = bAOEnabled;
	bAOEnabled = (NewState == ECheckBoxState::Checked);

	// AUDITED (raw/composition separation checkpoint, re-examined per explicit follow-up audit):
	//   - Turning OFF: PURE composition, unconditionally. AmbientOcclusionMask/AOCache/RawValues are
	//     left completely untouched -- ApplyPreviewToEntry's bAOEnabled-gated readiness check simply
	//     stops including this layer in the stack. RecomposeWorkingColors() only.
	//   - Turning ON: MUST NOT invalidate an already-valid derived mask -- doing so was the bug this
	//     follow-up audit found (enabling, by itself, was being used as a reason to invalidate).
	//     Instead: if EVERY entry already has a Ready AmbientOcclusionMask, this is pure composition
	//     too -- just recompose (RecomposeWorkingColors()), reusing AOCache with zero raycasts, working
	//     identically regardless of Auto Update Preview. Only entries WITHOUT a valid derived mask
	//     (never generated, or invalidated by an actual AO raw parameter change while AO was off) need
	//     real (re)generation -- gated by Auto Update Preview exactly like any other raw parameter
	//     (InvalidateAODerivedMask() is deliberately NOT called here -- it would needlessly clear
	//     already-Ready entries too; RunAutoUpdatePreview()'s own per-entry AO logic already leaves a
	//     Ready entry's snapshot untouched by construction when Auto Update runs, and if Auto Update is
	//     off, those already-Ready entries are simply recomposed immediately below instead of waiting).
	if (!bAOEnabled || bWasEnabled)
	{
		RecomposeWorkingColors();
		return;
	}

	bool bAnyEntryNeedsGeneration = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->WorkingMesh.AmbientOcclusionMask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			bAnyEntryNeedsGeneration = true;
			break;
		}
	}

	if (!bAnyEntryNeedsGeneration)
	{
		// Every entry already has a valid, Ready AO slot -- reuse it verbatim, zero raycasts.
		RecomposeWorkingColors();
		return;
	}

	if (bAutoUpdatePreview)
	{
		if (GEditor)
		{
			GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
		}
		RunAutoUpdatePreview();
	}
	else
	{
		// At least one entry needs Generate Mask, but entries that ARE already Ready must still be
		// shown immediately -- never held pending just because a DIFFERENT entry needs generation.
		RecomposeWorkingColors();
	}
}

void SVertexMaskForgePanel::OnAOInvertChanged(const ECheckBoxState NewState)
{
	bAOInvert = (NewState == ECheckBoxState::Checked);

	// AUDITED (raw/composition separation checkpoint, Invert fix): PURE composition -- RawValues in
	// AOCache are NEVER inverted; Invert is applied fresh, live, every recomposition (see
	// ApplyPreviewToEntry's own doc comment on the Invert live-override) directly from
	// FVertexMaskForgeAOCache::RawValues. Zero raycasts, zero Tree rebuild, works identically
	// regardless of Auto Update Preview.
	RecomposeWorkingColors();
}

void SVertexMaskForgePanel::OnAOLevelsChanged()
{
	// AUDITED (AO Levels checkpoint): PURE composition, same contract as OnAOInvertChanged -- RawValues
	// in AOCache/SourceTopologyAOCache are NEVER touched; Levels is applied fresh, live, every
	// recomposition (see ApplyAOLevelsAndInvert and its two live-override call sites in
	// ApplyPreviewToEntry) directly from RawValues. Zero raycasts, zero Tree rebuild, zero
	// GeometryFingerprint invalidation, works identically regardless of Auto Update Preview or domain
	// (Render-Vertex/Source-Topology).
	RecomposeWorkingColors();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateAOBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetBlendModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnAOBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	AOBlendMode = *NewSelection;

	// AUDITED (raw/composition separation checkpoint): PURE composition, same as Bounding Box's own
	// OnBlendModeSelectionChanged -- recomposes immediately, zero raycasts, regardless of Auto Update
	// Preview.
	RecomposeWorkingColors();
}

FText SVertexMaskForgePanel::GetAOBlendModeButtonText() const
{
	return VertexMaskForgePanel::GetBlendModeLabel(AOBlendMode);
}

FText SVertexMaskForgePanel::GetActiveMaskSourceText() const
{
	bool bAnyAxisEnabled = false;
	for (const FVertexMaskForgeAxisMaskParams& Params : BoundingBoxAxisParams)
	{
		if (Params.bEnabled)
		{
			bAnyAxisEnabled = true;
			break;
		}
	}

	if (bAnyAxisEnabled && bAOEnabled)
	{
		return LOCTEXT("ActiveMaskSourceBoth", "Active layers: Bounding Box + Ambient Occlusion");
	}
	if (bAnyAxisEnabled)
	{
		return LOCTEXT("ActiveMaskSourceBBoxOnly", "Active layers: Bounding Box only");
	}
	if (bAOEnabled)
	{
		return LOCTEXT("ActiveMaskSourceAOOnly", "Active layers: Ambient Occlusion only");
	}
	return LOCTEXT("ActiveMaskSourceNone", "Active layers: None -- enable a Bounding Box axis or Ambient Occlusion");
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildBoundingBoxAxisRow(const EVertexMaskForgeBoundsAxis Axis, const FText& Title)
{
	const int32 AxisIndex = static_cast<int32>(Axis);

	return SNew(SVerticalBox)

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 6.f, 0.f, 2.f))
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this, AxisIndex]()
		{
			return BoundingBoxAxisParams[AxisIndex].bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, AxisIndex](const ECheckBoxState NewState)
		{
			BoundingBoxAxisParams[AxisIndex].bEnabled = (NewState == ECheckBoxState::Checked);
			OnAxisParamChangedDiscrete();
		})
		.Content()
		[
			SNew(STextBlock)
			.Text(Title)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
		]
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("AxisPositionLabel", "Position"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.f, 0.f, 8.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.Value_Lambda([this, AxisIndex]() { return BoundingBoxAxisParams[AxisIndex].Position; })
			.OnValueChanged_Lambda([this, AxisIndex](const float NewValue)
			{
				BoundingBoxAxisParams[AxisIndex].Position = NewValue;
				InvalidateBoundingBoxRawMask();
				ScheduleAutoUpdatePreview();
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			// Visible label only -- renamed from "Transition" to "Falloff" per the UI refinement pass.
			// The internal field/parameter name TransitionWidth and its logic are unchanged (see the
			// header doc on FVertexMaskForgeAxisMaskParams::TransitionWidth for why it stays as-is).
			SNew(STextBlock).Text(LOCTEXT("AxisFalloffLabel", "Falloff"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.f, 0.f, 8.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.001f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.Value_Lambda([this, AxisIndex]() { return BoundingBoxAxisParams[AxisIndex].TransitionWidth; })
			.OnValueChanged_Lambda([this, AxisIndex](const float NewValue)
			{
				BoundingBoxAxisParams[AxisIndex].TransitionWidth = NewValue;
				InvalidateBoundingBoxRawMask();
				ScheduleAutoUpdatePreview();
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.f, 0.f, 6.f, 0.f))
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, AxisIndex]()
			{
				return BoundingBoxAxisParams[AxisIndex].bInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, AxisIndex](const ECheckBoxState NewState)
			{
				// AUDITED (BBox Invert exception, follow-up audit): dedicated handler, NOT
				// OnAxisParamChangedDiscrete -- see OnAxisInvertChanged's own doc comment.
				OnAxisInvertChanged(AxisIndex, NewState);
			})
			.Content()
			[
				SNew(STextBlock).Text(LOCTEXT("AxisInvertLabel", "Invert"))
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.f, 0.f, 6.f, 0.f))
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, AxisIndex]()
			{
				return BoundingBoxAxisParams[AxisIndex].bMirror ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, AxisIndex](const ECheckBoxState NewState)
			{
				BoundingBoxAxisParams[AxisIndex].bMirror = (NewState == ECheckBoxState::Checked);
				OnAxisParamChangedDiscrete();
			})
			.Content()
			[
				SNew(STextBlock).Text(LOCTEXT("AxisMirrorLabel", "Mirror"))
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, AxisIndex]()
			{
				return BoundingBoxAxisParams[AxisIndex].bWorldSpace ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, AxisIndex](const ECheckBoxState NewState)
			{
				BoundingBoxAxisParams[AxisIndex].bWorldSpace = (NewState == ECheckBoxState::Checked);
				OnAxisParamChangedDiscrete();
			})
			.Content()
			[
				SNew(STextBlock).Text(LOCTEXT("AxisWorldSpaceLabel", "World Space"))
			]
		]
	];
}

void SVertexMaskForgePanel::Construct(const FArguments& InArgs)
{
	// AddRaw (not AddSP): registered/removed explicitly via WorldCleanupDelegateHandle in the
	// destructor, so there is no reliance on AsShared()/SharedThis() being valid at this point in
	// construction, and no ambiguity about callback lifetime -- the handle is always removed before
	// this object finishes destructing.
	WorldCleanupDelegateHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &SVertexMaskForgePanel::OnWorldCleanup);

	// AddRaw (not AddSP), same rationale as WorldCleanupDelegateHandle above: registered/removed
	// explicitly via SelectionChangedDelegateHandle in the destructor. USelection::SelectionChangedEvent
	// (Editor/UnrealEd/Public/Selection.h) is the engine's own official notification for Actor/
	// Component/BSP scene selection changes -- used the same way by, e.g., SInViewportDetails and
	// FDataLayerMode -- and is the sole automatic trigger for RefreshSelection() now that the manual
	// "Refresh Selection" button is gone (see OnEditorSelectionChanged's own audit note).
	SelectionChangedDelegateHandle = USelection::SelectionChangedEvent.AddRaw(this, &SVertexMaskForgePanel::OnEditorSelectionChanged);

	// AUDITED (Undo/Redo fix): FEditorUndoClient::PostUndo/PostRedo are the engine's own official
	// notification for "an Undo or Redo transaction just finished" (used the same way by, e.g., Actor
	// detail panels and other Editor tool widgets) -- registered/unregistered explicitly via
	// GEditor->RegisterForUndo/UnregisterForUndo, paired with UnregisterForUndo in the destructor, same
	// lifecycle discipline as WorldCleanupDelegateHandle/SelectionChangedDelegateHandle above. This is
	// for INTERNAL PANEL STATE RESYNC ONLY (see PostUndo/PostRedo's own doc comment) -- it is never a
	// substitute for the transaction itself; Accept's own ModifyMeshDescription() call is what makes
	// Undo/Redo actually restore the Static Mesh's colors in the first place.
	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}

	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::OriginalMaterial));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::RGBVertexColor));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::RedChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::GreenChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::BlueChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::AlphaChannel));

	// Order matches the required dropdown order exactly (Copy first == default).
	BlendModeOptions.Add(MakeShared<EVertexMaskForgeBlendMode>(EVertexMaskForgeBlendMode::Copy));
	BlendModeOptions.Add(MakeShared<EVertexMaskForgeBlendMode>(EVertexMaskForgeBlendMode::Add));
	BlendModeOptions.Add(MakeShared<EVertexMaskForgeBlendMode>(EVertexMaskForgeBlendMode::Subtract));
	BlendModeOptions.Add(MakeShared<EVertexMaskForgeBlendMode>(EVertexMaskForgeBlendMode::Multiply));
	BlendModeOptions.Add(MakeShared<EVertexMaskForgeBlendMode>(EVertexMaskForgeBlendMode::Overlay));
	BlendModeOptions.Add(MakeShared<EVertexMaskForgeBlendMode>(EVertexMaskForgeBlendMode::Screen));
	BlendModeOptions.Add(MakeShared<EVertexMaskForgeBlendMode>(EVertexMaskForgeBlendMode::Linear));

	// Z starts enabled to reproduce the exact previously-validated Local-Z-only default; X and Y
	// start disabled (see BoundingBoxAxisParams' doc comment in the header).
	BoundingBoxAxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::Z)].bEnabled = true;

	ChildSlot
	[
		SNew(SBorder)
		.Padding(FMargin(12.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PanelTitle", "Vertex Mask Forge"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PanelSubtitle", "Native Vertex Color authoring tool"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SNew(SSeparator)
			]

			// AUDITED (UI reorganization checkpoint): "Active layers" moved here from inside the
			// Ambient Occlusion Mask panel -- it reports the GLOBAL composition stack (Bounding Box
			// and/or Ambient Occlusion, whichever are currently active), not something owned by AO
			// specifically, so it now reads as a neutral, panel-agnostic status line above both mask
			// panels instead of implying it belongs to AO alone. Same widget/binding as before
			// (GetActiveMaskSourceText, unchanged, still purely a live readout -- no new state), same
			// subdued/secondary text style, no border/background of its own, single occurrence only.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				SNew(STextBlock)
				.Text(this, &SVertexMaskForgePanel::GetActiveMaskSourceText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(false)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("BBoxMaskSectionTitle", "Bounding Box Mask"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				.BodyContent()
				[
					SNew(SVerticalBox)

					// Blend Mode + Opacity: apply to this Bounding Box Mask layer's composition with the
					// input Vertex Color, AFTER the mask itself is generated from Local X/Y/Z below --
					// see ComposeMaskLayer's doc comment for the exact order. Placed above Local X/Y/Z
					// since they configure the OUTPUT stage of the same layer, not another axis.
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 2.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock).Text(LOCTEXT("BlendModeLabel", "Blend Mode:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(BlendModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
							.OptionsSource(&BlendModeOptions)
							.InitiallySelectedItem(BlendModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateBlendModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnBlendModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetBlendModeButtonText)
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock).Text(LOCTEXT("OpacityLabel", "Opacity:"))
						]

						// Slider (fills remaining width, "largura proporcional ao painel") + a compact
						// editable numeric field alongside it -- two independent views of the SAME
						// BoundingBoxOpacity value, each firing its OWN OnValueChanged only from its OWN
						// user interaction, so a single drag/edit can never double-fire and cause two
						// redundant recompositions for one change.
						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value_Lambda([this]() { return BoundingBoxOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								BoundingBoxOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Delta(0.01f)
							.MinFractionalDigits(2)
							.MaxFractionalDigits(2)
							.Value_Lambda([this]() { return BoundingBoxOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								BoundingBoxOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						BuildBoundingBoxAxisRow(EVertexMaskForgeBoundsAxis::X, LOCTEXT("AxisTitleX", "Local X"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						BuildBoundingBoxAxisRow(EVertexMaskForgeBoundsAxis::Y, LOCTEXT("AxisTitleY", "Local Y"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						BuildBoundingBoxAxisRow(EVertexMaskForgeBoundsAxis::Z, LOCTEXT("AxisTitleZ", "Local Z"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Left)
					.Padding(FMargin(0.f, 6.f, 0.f, 0.f))
					[
						SNew(SHorizontalBox)

						// AUDITED (Preview reorganization checkpoint): Generate Mask / Auto Update
						// Preview moved into the Preview Mode box below (see that section) -- Unified
						// Bounds stays here, unchanged, since it is a Bounding-Box-specific parameter,
						// not a preview/update control.
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetUnifiedBoundsState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnUnifiedBoundsChanged)
							.ToolTipText(LOCTEXT("UnifiedBoundsTooltip", "Evaluate all selected meshes within one shared bounding-box domain across X, Y, and Z."))
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("UnifiedBoundsLabel", "Unified Bounds"))
							]
						]
					]
				]
			]

			// Ambient Occlusion Mask: the tool's second spatial mask source, composed together with
			// Bounding Box rather than replacing it (see bAOEnabled's own doc comment). Placed
			// immediately BELOW the Bounding Box Mask panel above.
			//
			// AUDITED (UI reorganization checkpoint): now a collapsible SExpandableArea, matching
			// Bounding Box exactly -- same header style/pattern, same InitiallyCollapsed(false) default.
			// The header shows ONLY the title (matching Bounding Box's header exactly); Enable moved
			// into the body's first row (it is a mask PARAMETER, not always-visible chrome -- collapsing
			// the area must never look like it disabled the layer, and it doesn't: bAOEnabled is
			// untouched by expand/collapse either way). "Active layers" moved out entirely -- see the
			// new top-level status line above the Bounding Box panel.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(false)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("AOSectionTitle", "Ambient Occlusion Mask"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				.BodyContent()
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
					[
						SNew(SCheckBox)
						.IsChecked(this, &SVertexMaskForgePanel::GetAOEnableState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnAOEnableChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AOEnableLabel", "Enable"))
						]
					]

					// AUDITED (composition-stack checkpoint): Blend Mode + Opacity, same Slate
					// controls/dimensions/alignment/labels/tooltips/limits as Bounding Box's own (see
					// the Bounding Box Mask section above) -- independent AOBlendMode/AOOpacity state,
					// same BlendModeOptions list (shared enum, shared GetBlendModeLabel), same
					// ApplyMaskBlendMode/BlendMaskValue formulas via ComposeMaskStack -- never a
					// duplicate/parallel blend implementation.
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 2.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock).Text(LOCTEXT("AOBlendModeLabel", "Blend Mode:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(AOBlendModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
							.OptionsSource(&BlendModeOptions)
							.InitiallySelectedItem(BlendModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateAOBlendModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnAOBlendModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetAOBlendModeButtonText)
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock).Text(LOCTEXT("AOOpacityLabel", "Opacity:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value_Lambda([this]() { return AOOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								AOOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Delta(0.01f)
							.MinFractionalDigits(2)
							.MaxFractionalDigits(2)
							.Value_Lambda([this]() { return AOOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								AOOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetAOInvertState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnAOInvertChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AOInvertLabel", "Invert"))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(12.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AOSamplesLabel", "Samples"))
							.ToolTipText(LOCTEXT("AOSamplesTooltip",
								"Auto Update Preview uses at most 16 samples for a responsive interactive "
								"preview; Generate Mask always uses the full Samples value set here."))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<int32>)
							.MinDesiredWidth(52.f)
							.MinValue(8)
							.MaxValue(256)
							.Delta(1)
							.ToolTipText(LOCTEXT("AOSamplesTooltip",
								"Auto Update Preview uses at most 16 samples for a responsive interactive "
								"preview; Generate Mask always uses the full Samples value set here."))
							.Value_Lambda([this]() { return AOSamples; })
							.OnValueChanged_Lambda([this](const int32 NewValue)
							{
								AOSamples = FMath::Clamp(NewValue, 8, 256);
								InvalidateAODerivedMask();
								ScheduleAutoUpdatePreview();
							})
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock).Text(LOCTEXT("AOMaxDistanceLabel", "Max Distance"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(64.f)
							.MinValue(0.01f)
							.MaxValue(10000.0f)
							.Delta(1.0f)
							.Value_Lambda([this]() { return AOMaxDistance; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								AOMaxDistance = FMath::Clamp(NewValue, 0.01f, 10000.0f);
								InvalidateAODerivedMask();
								ScheduleAutoUpdatePreview();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock).Text(LOCTEXT("AOBiasLabel", "Bias"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.001f)
							.MaxValue(10.0f)
							.Delta(0.01f)
							.Value_Lambda([this]() { return AOBias; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								AOBias = FMath::Clamp(NewValue, 0.001f, 10.0f);
								InvalidateAODerivedMask();
								ScheduleAutoUpdatePreview();
							})
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 4.f, 0.f, 0.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AOLevelsMinLabel", "Levels Min"))
							.ToolTipText(LOCTEXT("AOLevelsMinTooltip", "Values at or below this threshold become black."))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Delta(0.01f)
							.ToolTipText(LOCTEXT("AOLevelsMinTooltip", "Values at or below this threshold become black."))
							.Value_Lambda([this]() { return AOLevelsMin; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								AOLevelsMin = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnAOLevelsChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AOLevelsMaxLabel", "Levels Max"))
							.ToolTipText(LOCTEXT("AOLevelsMaxTooltip", "Values at or above this threshold become white."))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Delta(0.01f)
							.ToolTipText(LOCTEXT("AOLevelsMaxTooltip", "Values at or above this threshold become white."))
							.Value_Lambda([this]() { return AOLevelsMax; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								AOLevelsMax = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnAOLevelsChanged();
							})
						]
					]
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SBorder)
				.Padding(FMargin(8.f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("PreviewModeLabel", "Preview Mode"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
						[
							SAssignNew(PreviewModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgePreviewMode>>)
							.OptionsSource(&PreviewModeOptions)
							.InitiallySelectedItem(PreviewModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGeneratePreviewModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnPreviewModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetPreviewModeButtonText)
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SButton)
							.Text(LOCTEXT("FillWhite", "Fill White"))
							.OnClicked(this, &SVertexMaskForgePanel::OnFillWhiteClicked)
							.IsEnabled(this, &SVertexMaskForgePanel::CanRunFill)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(6.f, 0.f, 0.f, 0.f))
						[
							SNew(SButton)
							.Text(LOCTEXT("FillBlack", "Fill Black"))
							.OnClicked(this, &SVertexMaskForgePanel::OnFillBlackClicked)
							.IsEnabled(this, &SVertexMaskForgePanel::CanRunFill)
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetMaskActionStatusText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ChannelFilterLabel", "Channel Filter"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetChannelFilterRState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnChannelFilterRChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ChannelFilterR", "R"))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetChannelFilterGState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnChannelFilterGChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ChannelFilterG", "G"))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetChannelFilterBState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnChannelFilterBChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ChannelFilterB", "B"))
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetPreviewStatusText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]

					// AUDITED (Preview reorganization checkpoint): Auto Update Preview and Generate
					// Mask moved here from the Bounding Box Mask section above -- this box now owns
					// everything that controls preview visualization AND update/regeneration, per the
					// checkpoint's explicit requirement. Same widgets, same handlers, same
					// lifecycle/behavior as before the move -- only their position changed. No copies
					// remain at the old location.
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(SCheckBox)
						.IsChecked(this, &SVertexMaskForgePanel::GetAutoUpdatePreviewState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnAutoUpdatePreviewChanged)
						.ToolTipText(LOCTEXT("AutoUpdatePreviewTooltip",
							"Automatically regenerate and recompose the preview as parameters change, using a short debounce. Ambient Occlusion uses a reduced sample count while this is on, for responsiveness -- see the Samples tooltip in the Ambient Occlusion Mask section."))
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AutoUpdatePreviewLabel", "Auto Update Preview"))
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Left)
					[
						SNew(SButton)
						.Text(LOCTEXT("GenerateMask", "Generate Mask"))
						.ToolTipText(LOCTEXT("GenerateMaskTooltip",
							"Generate (or regenerate) the enabled mask layer(s) -- Bounding Box and/or Ambient Occlusion -- at full quality, and compose/apply the result to the preview."))
						.OnClicked(this, &SVertexMaskForgePanel::OnGenerateBoundingBoxMaskClicked)
					]
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SBorder)
				.Padding(FMargin(8.f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SPrimaryButton)
							.Text(LOCTEXT("AcceptChanges", "Accept"))
							.OnClicked(this, &SVertexMaskForgePanel::OnAcceptChangesClicked)
							.IsEnabled(this, &SVertexMaskForgePanel::CanAcceptChanges)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("CancelChanges", "Cancel"))
							.OnClicked(this, &SVertexMaskForgePanel::OnCancelChangesClicked)
							.IsEnabled(this, &SVertexMaskForgePanel::CanCancelChanges)
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("NaniteRequiresSourceMeshNotice",
							"Nanite requires vertex colors to be saved to the Static Mesh Asset. This affects every instance using the asset."))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
						.Visibility(this, &SVertexMaskForgePanel::GetNaniteNoticeVisibility)
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetOperationStatusText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]
			]
		]
	];

	RefreshSelection();
}

SVertexMaskForgePanel::~SVertexMaskForgePanel()
{
	// Removed first: guarantees OnWorldCleanup can never fire on a partially-destructed panel while
	// the rest of this destructor (and DestroyAllPreviews below) runs.
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupDelegateHandle);

	// Same rationale, for the same reason: no callback into a partially-destructed panel.
	USelection::SelectionChangedEvent.Remove(SelectionChangedDelegateHandle);

	// Same rationale as the two unregistrations above: no PostUndo/PostRedo callback into a
	// partially-destructed panel. Paired with GEditor->RegisterForUndo(this) in Construct().
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}

	// Explicit cancel, even though ScheduleAutoUpdatePreview() also binds weak-safe (CreateSP) --
	// this guarantees no pending debounce fires even one more Slate tick after this point.
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}

	// Idempotent: safe even if a preview was never activated, was already restored, or was already
	// cleaned up by a prior OnWorldCleanup call for its World. Never writes to any Static Mesh asset
	// -- shutdown/close must never perform an implicit Accept.
	DestroyAllPreviews();
}

void SVertexMaskForgePanel::OnEditorSelectionChanged(UObject* NewSelection)
{
	// AUDITED (Selected Static Meshes panel removal): this is the sole trigger for RefreshSelection()
	// now that the manual "Refresh Selection" button is gone. Bound to USelection::SelectionChangedEvent
	// (fired for Actor/Component/BSP selection sets -- see Editor/UnrealEd/Public/Selection.h; NOT
	// fired for Content Browser asset selection, which uses a completely separate API and is no
	// longer consulted anywhere in this panel -- see CollectViewportSelection, the only collector
	// left). NewSelection (which USelection instance changed) is deliberately unused: regardless of
	// which one fired, the only thing this does is re-derive SelectedMeshes from the CURRENT scene
	// selection, exactly like a manual Refresh Selection click did.
	//
	// Safety against an unresolved Preview (audited, replaces the old manual-refresh YesNoCancel
	// prompt from the removed OnRefreshSelectionClicked): a modal dialog firing on every incidental
	// selection change while a Preview is pending would be disruptive and is unnecessary, since
	// Accept / Cancel are always visible and available regardless of
	// the scene selection. So instead of prompting, this simply DECLINES to refresh at all while
	// OperationState != Idle (PendingChanges: an unaccepted Preview exists; Applying: mid-Accept,
	// never actually observable across a Slate tick since Accept is synchronous; Failed: the last
	// Accept attempt was blocked/failed and its Preview is still intentionally preserved) --
	// SelectedMeshes/PreviewComponents keep pointing at the session's ORIGINAL targets until the user
	// explicitly resolves it. This can never silently apply a pending result to the new selection
	// (RefreshSelection() is not called at all) and never silently discards it (DestroyAllPreviews()
	// is not called either).
	//
	// DEFERRED SYNC (audited): rather than requiring another viewport/World Outliner click after the
	// user resolves the pending operation, this records that a sync is owed
	// (bSceneSelectionChangedDuringActiveOperation = true). SyncSelectionIfChangedDuringOperation(),
	// called at the tail of OnCancelChangesClicked() / AcceptPendingChanges() (and ONLY there -- see
	// its own doc comment), consumes this flag and calls RefreshSelection() automatically once the
	// operation has fully concluded against its ORIGINAL targets. So Accept/Cancel remain fully able
	// to act on the original selection at any time, and the panel still catches up with a changed
	// scene selection automatically, without ever retargeting the operation itself.
	if (OperationState != EVertexMaskForgeOperationState::Idle)
	{
		bSceneSelectionChangedDuringActiveOperation = true;
		return;
	}

	// Already Idle: this call itself is about to sync SelectedMeshes with the current scene selection
	// directly, so any flag left over from an earlier session (e.g. OperationState settled back to
	// Idle through a path other than Cancel/Accept, such as a mask being
	// invalidated with no PreviewComponents left) is moot -- clear it defensively so a later Cancel/
	// Accept never performs a redundant extra refresh for a selection change this call already
	// picked up.
	bSceneSelectionChangedDuringActiveOperation = false;

	RefreshSelection();
}

void SVertexMaskForgePanel::SyncSelectionIfChangedDuringOperation()
{
	if (!bSceneSelectionChangedDuringActiveOperation)
	{
		return;
	}

	bSceneSelectionChangedDuringActiveOperation = false;
	RefreshSelection();
}

void SVertexMaskForgePanel::RefreshSelection()
{
	// Restore/destroy any active preview on the entries about to be discarded, before they (and
	// their PreviewComponents) are replaced. Rebuilding working meshes always resets
	// BoundingBoxMask to NotGenerated on the new entries, so nothing here needs to "invalidate" a
	// mask -- there is no old one left to invalidate once SelectedMeshes is replaced.
	DestroyAllPreviews();
	LastMaskActionStatusText = FText::GetEmpty();

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> NewSelection;
	TMap<FString, int32> PathToIndex;

	// Scene selection only (Actors/Components in the level, via Viewport or World Outliner) --
	// Content Browser asset selection is never consulted anywhere in this panel; a UStaticMesh can
	// only participate via a real, placed UStaticMeshComponent found here.
	CollectViewportSelection(NewSelection, PathToIndex);
	UpdateMeshDiagnostics(NewSelection);
	BuildWorkingMeshes(NewSelection);

	SelectedMeshes = MoveTemp(NewSelection);

	UE_LOG(LogVertexMaskForge, Log, TEXT("Refreshed selection: %d unique Static Mesh asset(s)"), SelectedMeshes.Num());

	// New entries always start with BoundingBoxMask == NotGenerated; if a Vertex Color preview
	// mode is still selected, this shows original colors + "Mask Not Ready" rather than anything
	// stale. bCommit=false: a fresh selection never consolidates anything (there is nothing to
	// consolidate yet -- BaselineColors/CommittedColors/WorkingColors are all freshly empty for
	// every new PreviewComponentState).
	UpdateAllPreviews(/*bCommit=*/false);
}

void SVertexMaskForgePanel::CollectViewportSelection(
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
	TMap<FString, int32>& InOutPathToIndex) const
{
	if (!GEditor)
	{
		return;
	}

	// Centralized eligibility gate for EVERY component reaching AddComponent below, from EITHER
	// source (Actor-owned or directly-selected) -- never bypassed by either pass.
	//
	// AUDITED (explicit, structural rejection of this plugin's OWN transient/preview components --
	// does NOT rely merely on "these are never registered with any USelection set", which is also
	// true but is a fact about CALLERS, not a property of the object itself):
	//   - IsValid(Component): UE 5.8's own official liveness check (UObject/UObjectGlobals.h) --
	//     covers null, garbage-collected/unreachable, and pending-kill objects in one call.
	//   - Component->GetOutermost() == GetTransientPackage(): the PRECISE structural signature of
	//     EnsurePreviewComponent's output (NewObject<UStaticMeshComponent>(GetTransientPackage(),
	//     NAME_None, RF_Transient) -- see its own audit note) and of any other genuinely-transient,
	//     non-serialized object. A real, placed level component's outermost package is always the
	//     level's own package (or, for a template component, the Blueprint class's package) -- never
	//     the global transient package -- so this can never reject a legitimate placed component.
	//   - Component->HasAnyFlags(RF_Transient): explicit flag check requested independently of the
	//     package check above, as a second line of defense against the same class of object.
	//     Ordinary placed level components must NOT be RF_Transient (RF_Transient objects are
	//     excluded from normal level serialization by construction), so this cannot reject a
	//     legitimate placed component either -- only throwaway/never-saved objects like our preview
	//     components carry this flag.
	//   - Component->GetStaticMesh() != nullptr: components with no mesh cannot participate.
	// This is the ONLY place either selection source is filtered; both the Actor pass and the
	// direct-Component pass funnel through this single gate.
	auto IsEligibleComponent = [](const UStaticMeshComponent* Component) -> bool
	{
		if (!IsValid(Component))
		{
			return false;
		}
		if (Component->GetOutermost() == GetTransientPackage() || Component->HasAnyFlags(RF_Transient))
		{
			return false;
		}
		if (!Component->GetStaticMesh())
		{
			return false;
		}
		return true;
	};

	auto AddComponent = [&InOutMeshes, &InOutPathToIndex, &IsEligibleComponent](UStaticMeshComponent* Component)
	{
		if (!IsEligibleComponent(Component))
		{
			return;
		}

		UStaticMesh* Mesh = Component->GetStaticMesh();
		VertexMaskForgePanel::AddOrUpdateSelectedMesh(
			InOutMeshes,
			InOutPathToIndex,
			FSoftObjectPath(Mesh).ToString(),
			Mesh->GetName(),
			TSoftObjectPtr<UStaticMesh>(Mesh),
			Component);
	};

	// AUDITED (precedence -- confirmed against the UE 5.8 Level Editor's own click-handling source,
	// not assumed): GEditor->GetSelectedActors() and GEditor->GetSelectedComponents() are separate
	// USelection VIEWS over the SAME shared UTypedElementSelectionSet in the Level Editor (see
	// SLevelEditor::Constructor: GetSelectedActors()->SetElementSelectionSet(SelectedElements) and
	// GetSelectedComponents()->SetElementSelectionSet(SelectedElements) -- Editor/LevelEditor/Private/
	// SLevelEditor.cpp). Confirmed in ViewportSelectionUtilities.cpp: LevelEditorViewport.cpp's own
	// click dispatch only calls ClickComponent() when the owning Actor is ALREADY exclusively
	// Actor-selected (bActorAlreadySelectedExclusively), and does NOT deselect that Actor when a
	// component is then explicitly selected -- so after explicit component selection, the owning
	// Actor legitimately REMAINS in GetSelectedActors() at the same time the component appears in
	// GetSelectedComponents(). A naive "Actor pass ADDS everything, then Component pass ADDS more"
	// would therefore silently re-include every OTHER component of that Actor too, defeating the
	// user's explicit, narrower selection -- exactly the inconsistency audited here.
	//
	// The engine's own dispatch code (LevelEditorViewport.cpp) uses GetSelectedComponentCount() > 0
	// as its own signal for "is an explicit component selection currently active"; ordinary actor
	// (re)selection always clears the WHOLE shared element set first (UEditorEngine::SelectNone(),
	// via the same shared SelectionSet), so this can never go stale relative to a different Actor --
	// there is no path where a component from Actor A stays selected while Actor B becomes newly
	// Actor-selected. This makes precedence fully deterministic:
	//   - if ANY UStaticMeshComponent is explicitly selected (GetSelectedComponents() non-empty),
	//     those components are the ONLY targets -- the owning Actor is never auto-expanded to its
	//     other components;
	//   - otherwise, fall back to Actor-granularity (every valid UStaticMeshComponent of every
	//     selected Actor), exactly the pre-existing, already-validated behavior.
	// Non-UStaticMeshComponent elements (e.g. a selected StaticMeshComponent's owning Actor being
	// selected via SOME OTHER component type) never affect this -- only UStaticMeshComponent objects
	// are ever queried from either USelection.
	TArray<UStaticMeshComponent*> ExplicitComponents;
	if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
	{
		SelectedComponents->GetSelectedObjects<UStaticMeshComponent>(ExplicitComponents);
	}

	if (!ExplicitComponents.IsEmpty())
	{
		// Explicit component selection takes full precedence -- deduplicated by pointer identity via
		// AddOrUpdateSelectedMesh (see its own audit note); never deduplicated by UStaticMesh, so two
		// explicitly-selected components sharing the same asset remain independent targets.
		for (UStaticMeshComponent* Component : ExplicitComponents)
		{
			AddComponent(Component);
		}
		return;
	}

	// Fallback: no explicit component selection -- every valid UStaticMeshComponent owned by every
	// selected Actor. Actor-granularity by design (pre-existing, already-validated behavior):
	// selecting an Actor with several UStaticMeshComponents collects ALL of them, not just one.
	if (USelection* SelectedActors = GEditor->GetSelectedActors())
	{
		TArray<AActor*> Actors;
		SelectedActors->GetSelectedObjects<AActor>(Actors);

		TArray<UStaticMeshComponent*> Components;
		for (AActor* Actor : Actors)
		{
			if (!IsValid(Actor))
			{
				continue;
			}

			Components.Reset();
			Actor->GetComponents<UStaticMeshComponent>(Components);
			for (UStaticMeshComponent* Component : Components)
			{
				AddComponent(Component);
			}
		}
	}
}

void SVertexMaskForgePanel::UpdateMeshDiagnostics(TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes) const
{
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : InOutMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		// Resolved only for the duration of this refresh; no raw pointer is stored on Entry.
		const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
		Entry->Diagnostics = VertexMaskForgePanel::InspectStaticMesh(Mesh);
	}
}

void SVertexMaskForgePanel::BuildWorkingMeshes(TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes) const
{
	int32 NumReady = 0;
	int32 NumUnavailable = 0;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : InOutMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		// Resolved only for the duration of this call; no raw pointer is stored on Entry.
		const UStaticMesh* Mesh = VertexMaskForgePanel::ResolveWorkingStaticMesh(Entry->Mesh);
		Entry->WorkingMesh = VertexMaskForgePanel::BuildWorkingMeshForStaticMesh(Mesh, Entry->Diagnostics);
		Entry->bUseSourceTopology = VertexMaskForgePanel::ShouldUseSourceTopology(Mesh);

		if (Entry->WorkingMesh.State == EVertexMaskForgeWorkingMeshState::Ready)
		{
			++NumReady;
		}
		else
		{
			++NumUnavailable;
		}
	}

	UE_LOG(LogVertexMaskForge, Log, TEXT("Built %d working mesh copy/copies; %d unavailable"), NumReady, NumUnavailable);
}

FReply SVertexMaskForgePanel::OnGenerateBoundingBoxMaskClicked()
{
	LastMaskActionStatusText = FText::GetEmpty();
	LastOperationErrorText = FText::GetEmpty();

	// AUDITED (composition-stack checkpoint): Bounding Box and Ambient Occlusion are now INDEPENDENT,
	// optional stack layers (see FVertexMaskForgeWorkingMesh::BoundingBoxMask's own doc comment) --
	// generation below populates BOTH slots independently; only the "nothing enabled at all" case is
	// blocked.
	bool bAnyAxisEnabled = false;
	for (const FVertexMaskForgeAxisMaskParams& Params : BoundingBoxAxisParams)
	{
		if (Params.bEnabled)
		{
			bAnyAxisEnabled = true;
			break;
		}
	}
	if (!bAnyAxisEnabled && !bAOEnabled)
	{
		// Per the explicit requirement: never generate an empty mask silently, never replace the
		// previous Preview, never enter Pending Changes with invalid data.
		LastOperationErrorText = LOCTEXT("NoMaskSourceEnabled", "Enable at least one Bounding Box axis or Ambient Occlusion.");
		RecomputeOperationState();
		return FReply::Handled();
	}

	// Computed ONCE for this whole click (batch), before touching any entry's mask -- Unified Bounds
	// must never regenerate just one mesh. Bounding-Box-only: unused (left null) when no axis is
	// enabled.
	TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> CollectiveBounds;
	const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr = nullptr;
	if (bAnyAxisEnabled && bUseUnifiedBounds)
	{
		FText CollectiveError;
		if (!VertexMaskForgePanel::ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/true, CollectiveBounds, CollectiveError))
		{
			LastOperationErrorText = CollectiveError;
			RecomputeOperationState();
			return FReply::Handled();
		}
		CollectiveBoundsPtr = &CollectiveBounds;
	}

	int32 NumBBoxReady = 0, NumBBoxUnavailable = 0, NumBBoxDegenerate = 0, NumBBoxInvalid = 0;
	int32 NumAOReady = 0, NumAOUnavailable = 0;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		// The working mesh (FDynamicMesh3) itself is no longer the source for either generator (see
		// GenerateBoundingBoxMask's audit note), but Ready is kept as the entry-level precondition
		// for consistency with the rest of the panel's pipeline/UX (an entry whose working mesh
		// failed to build is flagged Unavailable across the board, for BOTH slots).
		bool bWorkingMeshOK = Entry->WorkingMesh.State == EVertexMaskForgeWorkingMeshState::Ready;

		// Resolved only for the duration of this call, consistent with the rest of the panel.
		const UStaticMesh* Mesh = bWorkingMeshOK ? Entry->Mesh.LoadSynchronous() : nullptr;
		if (bWorkingMeshOK && (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0)))
		{
			bWorkingMeshOK = false;
		}
		const FStaticMeshRenderData* RenderData = bWorkingMeshOK ? Mesh->GetRenderData() : nullptr;
		if (bWorkingMeshOK && (!RenderData || !RenderData->LODResources.IsValidIndex(0)))
		{
			bWorkingMeshOK = false;
		}

		if (!bWorkingMeshOK)
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.BoundingBoxMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			Entry->WorkingMesh.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.AmbientOcclusionMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			++NumBBoxUnavailable;
			++NumAOUnavailable;
			continue;
		}

		// Entry-level reference evaluation: the first live PreviewComponent's transform, or Identity
		// if this entry has none (Content-Browser-only, or no components currently valid) -- see the
		// audit note on FVertexMaskForgeWorkingMesh::BoundingBoxMask. Actual per-instance Preview/
		// Accept composition re-evaluates per component when needed (both for World Space Bounding
		// Box axes and, unconditionally, for Ambient Occlusion -- see ApplyPreviewToEntry).
		FTransform ReferenceTransform = FTransform::Identity;
		bool bHasLiveComponent = false;
		for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
		{
			if (const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get())
			{
				ReferenceTransform = SourceComponent->GetComponentTransform();
				bHasLiveComponent = true;
				break;
			}
		}

		// Bounding Box slot.
		//
		// AUDITED (Nanite source-topology support): an entry with bUseSourceTopology true is generated
		// against WorkingMesh.Mesh (SOURCE-topology FDynamicMesh3), individual bounds only -- never
		// Unified Bounds, and never RenderData->LODResources[0] (that would be the reduced Nanite
		// fallback -- see GenerateBoundingBoxMaskFromDynamicMesh's own doc comment for why).
		if (bAnyAxisEnabled)
		{
			if (Entry->bUseSourceTopology)
			{
				Entry->WorkingMesh.BoundingBoxMask = VertexMaskForgePanel::GenerateBoundingBoxMaskFromDynamicMesh(
					*Entry->WorkingMesh.Mesh, BoundingBoxAxisParams, ReferenceTransform);
			}
			else
			{
				Entry->WorkingMesh.BoundingBoxMask = VertexMaskForgePanel::GenerateBoundingBoxMask(
					RenderData->LODResources[0], BoundingBoxAxisParams, ReferenceTransform, CollectiveBoundsPtr);
			}
			Entry->WorkingMesh.BoundingBoxMask.SelectionMeshCount = SelectedMeshes.Num();
		}
		else
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
		}
		switch (Entry->WorkingMesh.BoundingBoxMask.State)
		{
		case EVertexMaskForgeScalarMaskState::Ready: ++NumBBoxReady; break;
		case EVertexMaskForgeScalarMaskState::DegenerateBounds: ++NumBBoxDegenerate; break;
		case EVertexMaskForgeScalarMaskState::Invalid: ++NumBBoxInvalid; break;
		default: ++NumBBoxUnavailable; break;
		}

		// AUDITED (double-AO-execution fix): Ambient Occlusion slot -- ENTRY-LEVEL VALIDATION ONLY,
		// never a real computation. See FVertexMaskForgeWorkingMesh::AmbientOcclusionMask's own doc
		// comment: the actual per-vertex AO result is computed exactly once, per component, inside
		// ApplyPreviewToEntry -- this entry-level pass only decides Ready/Unavailable (cheaply, via
		// IsAmbientOcclusionInputValid) and snapshots the CLAMPED, RESOLVED parameters (full Samples --
		// this IS the explicit Generate Mask action) that ApplyPreviewToEntry will use.
		//
		// AUDITED (Nanite source-topology support): the validity check and RenderVertexCount snapshot
		// below both branch on bUseSourceTopology -- for a Source-Topology entry, "render vertex count"
		// in this snapshot means WorkingMesh.Mesh->VertexCount() (Dynamic Mesh vertex count), never
		// LOD0's, since ApplyPreviewToEntry will call GenerateAmbientOcclusionMaskFromDynamicMesh for
		// this entry, not GenerateAmbientOcclusionMask.
		if (bAOEnabled)
		{
			Entry->WorkingMesh.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.AmbientOcclusionMask.Source = EVertexMaskForgeScalarMaskSource::AmbientOcclusion;
			const bool bAOInputValid = Entry->bUseSourceTopology
				? VertexMaskForgePanel::IsAmbientOcclusionInputValidForDynamicMesh(Entry->WorkingMesh.Mesh.Get())
				: VertexMaskForgePanel::IsAmbientOcclusionInputValid(RenderData->LODResources[0]);
			if (bHasLiveComponent && bAOInputValid)
			{
				FVertexMaskForgeAOParams Params;
				Params.Samples = FMath::Clamp(AOSamples, 8, 256);
				Params.MaxDistance = FMath::Clamp(AOMaxDistance, 0.01f, 10000.0f);
				Params.Bias = FMath::Clamp(AOBias, 0.001f, 10.0f);
				Params.bInvert = bAOInvert;
				Params.LevelsMin = AOLevelsMin;
				Params.LevelsMax = AOLevelsMax;
				Entry->WorkingMesh.AmbientOcclusionMask.UsedAOParams = Params;
				Entry->WorkingMesh.AmbientOcclusionMask.RenderVertexCount = Entry->bUseSourceTopology
					? Entry->WorkingMesh.Mesh->VertexCount()
					: static_cast<int32>(RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices());
				Entry->WorkingMesh.AmbientOcclusionMask.State = EVertexMaskForgeScalarMaskState::Ready;
				++NumAOReady;
			}
			else
			{
				// Content-Browser-only entry (no live component), or the input fails its cheap
				// structural check -- AO cannot be evaluated either way.
				Entry->WorkingMesh.AmbientOcclusionMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
				++NumAOUnavailable;
			}
		}
		else
		{
			Entry->WorkingMesh.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
			++NumAOUnavailable;
		}
	}

	// Log (explicit operation summary): this function is only reached by an explicit Generate Mask
	// click -- RunAutoUpdatePreview has its own separate loop and never calls this one, so this line
	// cannot fire on every slider tick.
	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Generate Mask: Bounding Box %d ready/%d unavailable/%d degenerate/%d invalid; Ambient Occlusion %d ready/%d unavailable."),
		NumBBoxReady, NumBBoxUnavailable, NumBBoxDegenerate, NumBBoxInvalid, NumAOReady, NumAOUnavailable);

	// If a Vertex Color preview mode is active, recompose and reapply immediately using the
	// mask(s) just persisted -- the user should not have to reselect the dropdown. bCommit=true:
	// this IS the explicit Generate Mask action -- it consolidates WorkingColors into
	// CommittedColors for every entry, exactly the "explicitly consolidated" contract requires.
	UpdateAllPreviews(/*bCommit=*/true);

	return FReply::Handled();
}

/**
 * AUDITED (raw/composition separation checkpoint): supersedes the old, single, blanket
 * InvalidateBoundingBoxMasks() -- that function invalidated BOTH slots for EVERY parameter change,
 * including purely compositional ones (Blend Mode, Opacity, Invert, Enable/Disable), which meant even
 * with Auto Update Preview OFF, changing e.g. AO Opacity would clear a perfectly valid, Ready
 * AmbientOcclusionMask and flash the preview to original colors until the user clicked Generate Mask
 * again -- exactly the contradiction this checkpoint's audit flagged. The fix is three functions with
 * clear, disjoint responsibilities instead of one generic one:
 *   - InvalidateBoundingBoxRawMask() (this function): Bounding Box's OWN raw geometric parameters
 *     changed (axis Position/Falloff/Invert/Mirror/World Space/Enable, Unified Bounds) -- clears ONLY
 *     BoundingBoxMask; AmbientOcclusionMask (and its AOCache) are completely untouched.
 *   - InvalidateAODerivedMask(): Ambient Occlusion's OWN raw geometric parameters changed (Samples,
 *     Max Distance, Bias) -- clears ONLY AmbientOcclusionMask; BoundingBoxMask is completely
 *     untouched. Never touches AOCache directly either -- GenerateAmbientOcclusionMask's own cache key
 *     (see its CACHE doc comment) transparently rebuilds only the RawValues layer when these actually
 *     changed, the next time it is called with the new values.
 *   - RecomposeWorkingColors(): a PURE composition change (Blend Mode, Opacity, Invert, Channel
 *     Filter, Preview Mode, or Enable/Disable of an already-Ready layer) -- touches NEITHER slot,
 *     calls UpdateAllPreviews(false) directly. Since ApplyPreviewToEntry always re-reads current
 *     Blend Mode/Opacity/Channel Filter/Preview Mode/bAOEnabled live (never from a stale snapshot --
 *     see its own doc comment on the Invert live-override fix) and AmbientOcclusionMask/AOCache stay
 *     exactly as they were, this recomposes immediately and correctly with ZERO raycasts and ZERO
 *     Tree rebuilds, works identically whether Auto Update Preview is on or off, and can never produce
 *     an original-color fallback for a layer that is still genuinely Ready.
 *
 * Both InvalidateBoundingBoxRawMask() and InvalidateAODerivedMask() only call UpdateAllPreviews(false)
 * synchronously when Auto Update Preview is OFF (see the reasoning that used to live on the old
 * combined function: when Auto Update is ON, a regeneration is guaranteed to follow this call very
 * shortly -- either immediately, via the SAME discrete-parameter handler, or via the debounce timer --
 * so an interim synchronous UpdateAllPreviews here would only flash the OTHER, unaffected slot's
 * preview and, for Ambient Occlusion, risk an unnecessary AOCache-destroying fallback in between).
 */
void SVertexMaskForgePanel::InvalidateBoundingBoxRawMask()
{
	LastMaskActionStatusText = FText::GetEmpty();

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
		}
	}

	if (!bAutoUpdatePreview)
	{
		UpdateAllPreviews(/*bCommit=*/false);
	}
}

void SVertexMaskForgePanel::InvalidateAODerivedMask()
{
	LastMaskActionStatusText = FText::GetEmpty();

	// Fires once per Samples/Max Distance/Bias change, or when AO Enable turns on for an entry with
	// no valid derived slot yet -- Verbose (not Log): this can fire once per slider tick during a
	// drag, same noise class as the AO cache-miss lines above. AOCache.RawValues are NEVER touched
	// by this function -- only the entry-level derived (AmbientOcclusionMask) slot is cleared.
	UE_LOG(LogVertexMaskForge, Verbose, TEXT("Vertex Mask Forge: AO derived mask invalidated (entry-level snapshot only, AOCache.RawValues preserved)."));

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->WorkingMesh.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
		}
	}

	if (!bAutoUpdatePreview)
	{
		UpdateAllPreviews(/*bCommit=*/false);
	}
}

void SVertexMaskForgePanel::RecomposeWorkingColors()
{
	// Deliberately does NOT touch BoundingBoxMask or AmbientOcclusionMask -- see this function's own
	// doc note above (on InvalidateBoundingBoxRawMask). ApplyPreviewToEntry re-reads all compositional
	// state (Blend Mode/Opacity/Channel Filter/Preview Mode/bAOEnabled/bAOInvert) live, every call, so
	// this alone is a complete, correct, zero-raycast recomposition.
	UpdateAllPreviews(/*bCommit=*/false);
}

bool SVertexMaskForgePanel::CanRunFill() const
{
	if (OperationState == EVertexMaskForgeOperationState::Applying)
	{
		return false;
	}

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->WorkingMesh.State == EVertexMaskForgeWorkingMeshState::Ready)
		{
			return true;
		}
	}
	return false;
}

void SVertexMaskForgePanel::RunConstantFill(
	const float ConstantValue, const EVertexMaskForgeScalarMaskSource Source, const FText& SuccessMessage)
{
	// A pending Auto Update Preview debounce must never overwrite this explicit Fill moments later.
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}

	LastOperationErrorText = FText::GetEmpty();
	LastMaskActionStatusText = FText::GetEmpty();

	int32 NumReady = 0;
	int32 NumFailed = 0;
	FString FirstFailedAssetName;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		// Same entry-level validity gating as Generate Mask -- but unlike Generate Mask, a failure
		// here leaves the entry's existing mask COMPLETELY UNTOUCHED (preserve the last valid
		// Preview), rather than resetting it to Unavailable.
		if (Entry->WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready)
		{
			++NumFailed;
			if (FirstFailedAssetName.IsEmpty())
			{
				FirstFailedAssetName = Entry->AssetName;
			}
			continue;
		}

		const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
		if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
		{
			++NumFailed;
			if (FirstFailedAssetName.IsEmpty())
			{
				FirstFailedAssetName = Entry->AssetName;
			}
			continue;
		}

		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			++NumFailed;
			if (FirstFailedAssetName.IsEmpty())
			{
				FirstFailedAssetName = Entry->AssetName;
			}
			continue;
		}

		// AUDITED (Nanite source-topology support): a Source-Topology entry's Fill mask is built in the
		// corner domain (3 * TriangleCount), matching UpdateWorkingColorsSourceTopology's own domain --
		// never RenderData->LODResources[0] (the reduced Nanite fallback).
		FVertexMaskForgeScalarMask NewMask = Entry->bUseSourceTopology
			? VertexMaskForgePanel::GenerateConstantMaskForCornerDomain(Entry->WorkingMesh.Mesh->TriangleCount() * 3, ConstantValue, Source)
			: VertexMaskForgePanel::GenerateConstantMask(RenderData->LODResources[0], ConstantValue, Source);
		if (NewMask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			++NumFailed;
			if (FirstFailedAssetName.IsEmpty())
			{
				FirstFailedAssetName = Entry->AssetName;
			}
			continue;
		}

		Entry->WorkingMesh.BoundingBoxMask = MoveTemp(NewMask);
		++NumReady;
	}

	if (NumReady > 0 && NumFailed == 0)
	{
		LastMaskActionStatusText = SuccessMessage;
	}
	else if (NumReady > 0)
	{
		LastMaskActionStatusText = FText::Format(
			LOCTEXT("FillPartialFormat", "{0} ({1} mesh(es) could not be filled and kept their previous Preview.)"),
			SuccessMessage, FText::AsNumber(NumFailed));
	}
	else
	{
		LastOperationErrorText = LOCTEXT("FillNothingEligible", "Fill: no eligible selected mesh could be filled (no valid render data).");
	}

	UE_LOG(LogVertexMaskForge, Log, TEXT("Vertex Mask Forge: Fill (%s): %d ready; %d failed/preserved"),
		Source == EVertexMaskForgeScalarMaskSource::ConstantWhite ? TEXT("White") : TEXT("Black"), NumReady, NumFailed);

	// Recomposes/reapplies via the exact same ApplyPreviewToEntry/UpdateWorkingColors path as every
	// other mask, and marks Pending Changes via RecomputeOperationState() at the end. bCommit=true:
	// Fill is an explicit, equivalent-to-generated action -- it consolidates into CommittedColors
	// exactly like Generate Mask, so a later toggle in another channel can never erase it.
	UpdateAllPreviews(/*bCommit=*/true);
}

FReply SVertexMaskForgePanel::OnFillWhiteClicked()
{
	RunConstantFill(1.0f, EVertexMaskForgeScalarMaskSource::ConstantWhite, LOCTEXT("FillWhiteReady", "White fill preview ready."));
	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnFillBlackClicked()
{
	RunConstantFill(0.0f, EVertexMaskForgeScalarMaskSource::ConstantBlack, LOCTEXT("FillBlackReady", "Black fill preview ready."));
	return FReply::Handled();
}

void SVertexMaskForgePanel::OnAutoUpdatePreviewChanged(const ECheckBoxState NewState)
{
	bAutoUpdatePreview = (NewState == ECheckBoxState::Checked);
	if (!bAutoUpdatePreview && GEditor)
	{
		// Turning it off must not let an already-armed debounce fire afterwards.
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
}

void SVertexMaskForgePanel::OnUnifiedBoundsChanged(const ECheckBoxState NewState)
{
	bUseUnifiedBounds = (NewState == ECheckBoxState::Checked);

	// Cancel any pending debounce/invalidate stale callbacks first -- toggling the domain mode must
	// never let an old callback (carrying the OLD bUseUnifiedBounds implicitly, via whatever it
	// recomputes against) apply after this point.
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}

	// Invalidate every entry's current Bounding-Box-sourced mask -- the domain just changed, so any
	// existing result is stale regardless of Auto Update Preview. Ambient Occlusion is untouched
	// (Unified Bounds has no effect on it).
	InvalidateBoundingBoxRawMask();

	if (bAutoUpdatePreview)
	{
		// Immediate, coherent batch regeneration -- RunAutoUpdatePreview() itself computes the
		// (possibly newly Individual or newly Unified) domain ONCE and reuses it for every eligible
		// entry, never recomputing just one mesh in isolation.
		RunAutoUpdatePreview();
	}
	// Else: parameters updated and results invalidated only, per the same contract every other axis
	// parameter already follows when Auto Update Preview is off -- the user must click Generate Mask.
}

void SVertexMaskForgePanel::ScheduleAutoUpdatePreview()
{
	if (!bAutoUpdatePreview || !GEditor)
	{
		return;
	}

	// ~150ms: within the requested 100-200ms window. SetTimer() on an already-armed handle clears
	// and re-adds it (TimerManager.cpp), so a new change before this fires correctly restarts the wait.
	constexpr float DebounceSeconds = 0.15f;
	GEditor->GetTimerManager()->SetTimer(
		AutoUpdateDebounceTimerHandle,
		FTimerDelegate::CreateSP(this, &SVertexMaskForgePanel::RunAutoUpdatePreview, /*bIncludeAO=*/true),
		DebounceSeconds,
		/*bLoop=*/false);
}

void SVertexMaskForgePanel::RunAutoUpdatePreview(const bool bIncludeAO)
{
	// Not reachable in practice (Accept is fully synchronous, so no Slate tick -- and therefore no
	// timer -- can fire while Applying), but guarded explicitly per the requirement that auto-update
	// must never run during Applying/shutdown/World cleanup.
	if (OperationState == EVertexMaskForgeOperationState::Applying)
	{
		return;
	}

	// Cleared here (fresh attempt), not by RecomputeOperationState() (called via UpdateAllPreviews()
	// below) -- so a failure message set by THIS pass survives that call rather than being wiped by it.
	LastOperationErrorText = FText::GetEmpty();
	// An auto-regenerated Bounding Box mask supersedes any prior Fill status message.
	LastMaskActionStatusText = FText::GetEmpty();

	// AUDITED (composition-stack checkpoint): same independent-slots model as
	// OnGenerateBoundingBoxMaskClicked -- only "nothing enabled at all" is blocked.
	bool bAnyAxisEnabled = false;
	for (const FVertexMaskForgeAxisMaskParams& Params : BoundingBoxAxisParams)
	{
		if (Params.bEnabled)
		{
			bAnyAxisEnabled = true;
			break;
		}
	}
	// AUDITED (BBox Invert exception): when bIncludeAO is false (OnAxisInvertChanged's scoped call),
	// Ambient Occlusion is treated as irrelevant to this call regardless of bAOEnabled's actual value
	// -- see the AO block below, which is skipped entirely in that case.
	if (!bAnyAxisEnabled && !(bIncludeAO && bAOEnabled))
	{
		// Preserve every entry's existing masks untouched and surface the specific message -- do not
		// fall through to the generic "could not be regenerated" wording below.
		LastOperationErrorText = LOCTEXT("NoMaskSourceEnabledAutoUpdate", "Enable at least one Bounding Box axis or Ambient Occlusion.");
		UpdateAllPreviews(/*bCommit=*/false);
		return;
	}

	// Computed ONCE for this whole regeneration (batch), before touching any entry's mask -- Unified
	// Bounds must never recompute just one mesh. A failure here is global (not per-entry), so it is
	// treated like "no axis enabled": preserve every entry's existing Bounding Box mask, surface the
	// specific message, and do not attempt any per-entry Bounding Box regeneration this pass (Ambient
	// Occlusion, if enabled, still proceeds independently below).
	TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> CollectiveBounds;
	const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr = nullptr;
	bool bSkipBoundingBoxThisPass = !bAnyAxisEnabled;
	if (bAnyAxisEnabled && bUseUnifiedBounds)
	{
		FText CollectiveError;
		if (!VertexMaskForgePanel::ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/true, CollectiveBounds, CollectiveError))
		{
			LastOperationErrorText = CollectiveError;
			bSkipBoundingBoxThisPass = true;
		}
		else
		{
			CollectiveBoundsPtr = &CollectiveBounds;
		}
	}

	int32 NumFailed = 0;
	FString FirstFailedAssetName;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid() || Entry->WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready)
		{
			continue;
		}

		const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
		if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
		{
			continue;
		}
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			continue;
		}

		FTransform ReferenceTransform = FTransform::Identity;
		bool bHasLiveComponent = false;
		for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
		{
			if (const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get())
			{
				ReferenceTransform = SourceComponent->GetComponentTransform();
				bHasLiveComponent = true;
				break;
			}
		}

		// Bounding Box slot: unchanged "auto-update never replaces a valid Preview with
		// incomplete/degenerate data" contract.
		//
		// AUDITED (Nanite source-topology support): same domain branch as
		// OnGenerateBoundingBoxMaskClicked -- Source-Topology entries always use individual bounds
		// (bSkipBoundingBoxThisPass/CollectiveBoundsPtr are Unified-Bounds-only concerns and do not
		// apply to this branch at all).
		if (bAnyAxisEnabled && !bSkipBoundingBoxThisPass)
		{
			FVertexMaskForgeScalarMask NewBBoxMask = Entry->bUseSourceTopology
				? VertexMaskForgePanel::GenerateBoundingBoxMaskFromDynamicMesh(
					*Entry->WorkingMesh.Mesh, BoundingBoxAxisParams, ReferenceTransform)
				: VertexMaskForgePanel::GenerateBoundingBoxMask(
					RenderData->LODResources[0], BoundingBoxAxisParams, ReferenceTransform, CollectiveBoundsPtr);
			NewBBoxMask.SelectionMeshCount = SelectedMeshes.Num();

			if (NewBBoxMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				Entry->WorkingMesh.BoundingBoxMask = MoveTemp(NewBBoxMask);
			}
			else
			{
				++NumFailed;
				if (FirstFailedAssetName.IsEmpty())
				{
					FirstFailedAssetName = Entry->AssetName;
				}
			}
		}
		else if (!bAnyAxisEnabled)
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
		}

		// AUDITED (double-AO-execution fix): Ambient Occlusion slot -- ENTRY-LEVEL VALIDATION ONLY,
		// same as OnGenerateBoundingBoxMaskClicked, never a real computation here -- see
		// FVertexMaskForgeWorkingMesh::AmbientOcclusionMask's own doc comment. Samples capped to
		// AOAutoUpdateMaxSamples for interactive responsiveness (AUDITED, interactive-performance
		// fix); the UI's AOSamples value itself is never mutated, only what gets snapshotted for this
		// pass. An explicit Generate Mask click always snapshots the full, user-chosen AOSamples.
		//
		// AUDITED (BBox Invert exception): bIncludeAO false (OnAxisInvertChanged's scoped call) skips
		// this ENTIRE block -- AmbientOcclusionMask is not read, not cleared, not re-validated, not
		// touched in any way, so a BBox Invert change can never have any observable effect on AO.
		if (bIncludeAO)
		{
			if (bAOEnabled)
			{
				Entry->WorkingMesh.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
				Entry->WorkingMesh.AmbientOcclusionMask.Source = EVertexMaskForgeScalarMaskSource::AmbientOcclusion;
				const bool bAOInputValid = Entry->bUseSourceTopology
					? VertexMaskForgePanel::IsAmbientOcclusionInputValidForDynamicMesh(Entry->WorkingMesh.Mesh.Get())
					: VertexMaskForgePanel::IsAmbientOcclusionInputValid(RenderData->LODResources[0]);
				if (bHasLiveComponent && bAOInputValid)
				{
					FVertexMaskForgeAOParams Params;
					Params.Samples = FMath::Clamp(FMath::Min(AOSamples, VertexMaskForgePanel::AOAutoUpdateMaxSamples), 8, 256);
					Params.MaxDistance = FMath::Clamp(AOMaxDistance, 0.01f, 10000.0f);
					Params.Bias = FMath::Clamp(AOBias, 0.001f, 10.0f);
					Params.bInvert = bAOInvert;
					Params.LevelsMin = AOLevelsMin;
					Params.LevelsMax = AOLevelsMax;
					Entry->WorkingMesh.AmbientOcclusionMask.UsedAOParams = Params;
					Entry->WorkingMesh.AmbientOcclusionMask.RenderVertexCount = Entry->bUseSourceTopology
						? Entry->WorkingMesh.Mesh->VertexCount()
						: static_cast<int32>(RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices());
					Entry->WorkingMesh.AmbientOcclusionMask.State = EVertexMaskForgeScalarMaskState::Ready;
				}
				else
				{
					Entry->WorkingMesh.AmbientOcclusionMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
				}
			}
			else
			{
				Entry->WorkingMesh.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
			}
		}
	}

	if (NumFailed > 0)
	{
		LastOperationErrorText = FText::Format(
			LOCTEXT("AutoUpdateFailedFormat",
				"Auto Update Preview: {0} mesh(es) could not be regenerated with the current parameters (e.g. '{1}') -- kept the last valid Preview for those."),
			FText::AsNumber(NumFailed), FText::FromString(FirstFailedAssetName));
	}

	// Recomposes/reapplies from whichever mask each entry ended up with (freshly regenerated, or the
	// preserved previous one), and recomputes OperationState -- but never touches
	// LastOperationErrorText (see the comment at the top of this function). bCommit=false: Auto
	// Update Preview is a transient recomposition -- it must never silently consolidate WorkingColors
	// into CommittedColors, only an explicit Generate Mask click does.
	UpdateAllPreviews(/*bCommit=*/false);
}

// --- Preview (Preview Mode + Channel Filter) --------------------------------------------------

TSharedRef<SWidget> SVertexMaskForgePanel::OnGeneratePreviewModeRow(TSharedPtr<EVertexMaskForgePreviewMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetPreviewModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnPreviewModeSelectionChanged(TSharedPtr<EVertexMaskForgePreviewMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	CurrentPreviewMode = *NewSelection;
	// bCommit=false -- Preview Mode is purely a display transform (DeriveDisplayColors); it never
	// touches WorkingColors' composition and must never consolidate anything.
	UpdateAllPreviews(/*bCommit=*/false);
}

FText SVertexMaskForgePanel::GetPreviewModeButtonText() const
{
	return VertexMaskForgePanel::GetPreviewModeLabel(CurrentPreviewMode);
}

void SVertexMaskForgePanel::OnChannelFilterRChanged(const ECheckBoxState NewState)
{
	bChannelFilterR = (NewState == ECheckBoxState::Checked);
	// AUDITED (Channel Filter toggle fix): bCommit=false -- toggling the Channel Filter never
	// consolidates; WorkingColors rebuilds from CommittedColors, so unchecking a channel immediately
	// reverts it to its last consolidated state (see UpdateWorkingColors' own doc comment).
	UpdateAllPreviews(/*bCommit=*/false);
}

void SVertexMaskForgePanel::OnChannelFilterGChanged(const ECheckBoxState NewState)
{
	bChannelFilterG = (NewState == ECheckBoxState::Checked);
	UpdateAllPreviews(/*bCommit=*/false);
}

void SVertexMaskForgePanel::OnChannelFilterBChanged(const ECheckBoxState NewState)
{
	bChannelFilterB = (NewState == ECheckBoxState::Checked);
	UpdateAllPreviews(/*bCommit=*/false);
}

FText SVertexMaskForgePanel::GetPreviewStatusText() const
{
	if (CurrentPreviewMode == EVertexMaskForgePreviewMode::OriginalMaterial)
	{
		return LOCTEXT("PreviewStatusOriginal", "Preview: Original Material");
	}

	bool bAnyViewportComponent = false;
	bool bAnyMaskNotReady = false;
	bool bAnyReady = false;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid() || Entry->PreviewComponents.IsEmpty())
		{
			continue;
		}

		bAnyViewportComponent = true;
		if (Entry->WorkingMesh.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready
			|| Entry->WorkingMesh.AmbientOcclusionMask.State == EVertexMaskForgeScalarMaskState::Ready)
		{
			bAnyReady = true;
		}
		else
		{
			bAnyMaskNotReady = true;
		}
	}

	if (!bAnyViewportComponent)
	{
		return LOCTEXT("PreviewStatusNoComponent", "Preview unavailable: no viewport component");
	}
	if (bAnyMaskNotReady && !bAnyReady)
	{
		return LOCTEXT("PreviewStatusMaskNotReady", "Preview: Mask Not Ready — Generate Mask");
	}
	if (bAnyMaskNotReady)
	{
		return LOCTEXT("PreviewStatusMixed", "Preview: Active (some selected meshes: Mask Not Ready — Generate Mask)");
	}
	return LOCTEXT("PreviewStatusActive", "Preview: Active");
}

UMaterialInterface* SVertexMaskForgePanel::GetPreviewDebugMaterial()
{
	if (UMaterialInterface* Existing = PreviewDebugMaterial.Get())
	{
		return Existing;
	}

	UMaterialInterface* Loaded = VertexMaskForgePanel::LoadPreviewDebugMaterial();
	if (!Loaded)
	{
		UE_LOG(LogVertexMaskForge, Warning,
			TEXT("Vertex Mask Forge: could not load the built-in Vertex Color preview material; preview is unavailable."));
	}

	PreviewDebugMaterial = Loaded;
	return Loaded;
}

void SVertexMaskForgePanel::PostUndo(bool bSuccess)
{
	HandlePostUndoRedo(bSuccess, /*bIsRedo=*/false);
}

void SVertexMaskForgePanel::PostRedo(bool bSuccess)
{
	HandlePostUndoRedo(bSuccess, /*bIsRedo=*/true);
}

/**
 * AUDITED (Undo/Redo fix): fires for EVERY Undo/Redo in the Editor, not only ones this tool created --
 * there is no cheap way to tell from here whether the transaction that just (un)did actually touched
 * one of this panel's own Static Mesh assets, so this is INTENTIONALLY conservative rather than
 * reactive. It never re-applies WorkingColors, never regenerates a mask, never opens a transaction of
 * its own, and never touches the Redo stack -- its ONLY job is to make sure this panel's own cached
 * state (BaselineColors/CommittedColors/WorkingColors, the Source-Topology equivalents, AO/BBox caches,
 * OperationState) never silently drifts from whatever the Static Mesh asset(s) actually contain after
 * the Transaction Buffer finishes restoring/reapplying them:
 *   - OperationState == Idle (no active PendingChanges/Failed session): safe to resync immediately.
 *     RefreshSelection() is the SAME function OnEditorSelectionChanged already calls for an ordinary
 *     scene selection change -- it only READS the current state of the selected components/assets to
 *     rebuild working meshes and re-capture fresh baselines; it never re-applies a mask or creates
 *     PendingChanges by itself (that only happens later, if the user explicitly clicks Generate Mask /
 *     enables Auto Update Preview). So this can never itself fire another Accept, open a transaction,
 *     or reintroduce stale colors.
 *   - OperationState != Idle (an active PendingChanges/Failed session exists, e.g. a generated-but-not-
 *     yet-accepted mask): never disrupt it just because SOME unrelated Undo/Redo fired elsewhere in the
 *     Editor -- same non-disruptive policy OnEditorSelectionChanged already uses for a scene selection
 *     change during an active operation (see its own audit note). Instead, this defers the resync via
 *     the SAME bSceneSelectionChangedDuringActiveOperation flag / SyncSelectionIfChangedDuringOperation()
 *     mechanism already used for that case: once the user concludes the CURRENT session through their
 *     own Accept or Cancel, the panel automatically re-syncs against whatever the asset(s) actually
 *     contain at that point -- never silently mid-session.
 */
void SVertexMaskForgePanel::HandlePostUndoRedo(bool bSuccess, bool bIsRedo)
{
	if (!bSuccess)
	{
		return;
	}

	const bool bResyncedImmediately = (OperationState == EVertexMaskForgeOperationState::Idle);
	if (bResyncedImmediately)
	{
		RefreshSelection();
	}
	else
	{
		bSceneSelectionChangedDuringActiveOperation = true;
	}

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Post%s (OperationState=%d) -- %s."),
		bIsRedo ? TEXT("Redo") : TEXT("Undo"),
		static_cast<int32>(OperationState),
		bResyncedImmediately ? TEXT("resynced immediately") : TEXT("resync deferred until the active session concludes"));
}

void SVertexMaskForgePanel::RecomputeOperationState()
{
	if (OperationState == EVertexMaskForgeOperationState::Applying)
	{
		return;
	}

	// AUDITED (Accept-in-Original-Material fix): previously gated entirely behind
	// `CurrentPreviewMode != OriginalMaterial`, from when Original Material meant "no active preview,
	// showing the real SourceComponent" and therefore had nothing valid to accept. Since the Original
	// Textures fix, Original Material is a real preview state (transient preview component, live
	// WorkingColors, original materials) exactly like every other Preview Mode -- Preview Mode only
	// selects presentation (see ApplyPreviewToEntry's bUseOriginalMaterials) and must never affect
	// whether pending changes exist. bHasPending is therefore now computed identically regardless of
	// CurrentPreviewMode.
	bool bHasPending = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && !Entry->PreviewComponents.IsEmpty()
			&& (Entry->WorkingMesh.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready
				|| Entry->WorkingMesh.AmbientOcclusionMask.State == EVertexMaskForgeScalarMaskState::Ready))
		{
			bHasPending = true;
			break;
		}
	}

	OperationState = bHasPending ? EVertexMaskForgeOperationState::PendingChanges : EVertexMaskForgeOperationState::Idle;
}

FText SVertexMaskForgePanel::GetOperationStatusText() const
{
	if (!LastOperationErrorText.IsEmpty())
	{
		return LastOperationErrorText;
	}

	switch (OperationState)
	{
	case EVertexMaskForgeOperationState::PendingChanges:
		return LOCTEXT("OperationStatePending", "Pending Changes: Accept writes Vertex Colors to the Source Static Mesh asset (affects every instance); Cancel discards.");
	case EVertexMaskForgeOperationState::Applying:
		return LOCTEXT("OperationStateApplying", "Applying...");
	case EVertexMaskForgeOperationState::Failed:
		return LOCTEXT("OperationStateFailed", "Accept failed (see message above).");
	case EVertexMaskForgeOperationState::Idle:
	default:
		return LOCTEXT("OperationStateIdle", "No pending changes.");
	}
}

FReply SVertexMaskForgePanel::OnAcceptChangesClicked()
{
	AcceptPendingChanges();
	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnCancelChangesClicked()
{
	// AUDITED (full session termination): Cancel now ends the CURRENT session entirely, not just the
	// transient Preview. SelectedMeshes is populated ONLY by RefreshSelection() (called once from
	// Construct(), and automatically from OnEditorSelectionChanged() whenever the scene selection
	// changes AND OperationState == Idle -- see its audit note). Clearing it here is therefore
	// sufficient BY CONSTRUCTION: nothing silently repopulates it afterwards until OperationState
	// actually becomes Idle again (which the RecomputeOperationState() call below does immediately).
	// The Unreal Editor's own selection (GetSelectedActors()) is never touched -- only this panel's
	// OWN SelectedMeshes array and derived session state are cleared. Because OperationState is Idle
	// again immediately after this, the very next scene selection change (or an unchanged selection,
	// if the user re-triggers one) starts a genuinely new session automatically.

	// 1-2. Cancel any pending debounce/Auto Update callback FIRST -- a callback already queued
	// before this click must never fire afterwards and regenerate/repopulate anything.
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}

	// 3-6. Discard Pending Changes: destroys every PreviewComponent, releases every hide token, and
	// restores every original component's visibility (all via DestroyAllPreviews ->
	// RestorePreviewForEntry -> RestoreComponentOriginal, already idempotent and tolerant of
	// divergent attachment bookkeeping -- see DetachAndDestroyPreviewComponent). Never writes to any
	// Static Mesh asset; never marks any asset or map dirty.
	DestroyAllPreviews();

	// 7-9. Clear every session-scoped container/result belonging to the cancelled session -- the
	// tool's OWN selection list (never the Editor's) and all diagnostics/messages.
	SelectedMeshes.Empty();
	LastOperationErrorText = FText::GetEmpty();
	LastMaskActionStatusText = FText::GetEmpty();

	// 10. Ready for a new session: OperationState recomputes to Idle from the now-empty
	// SelectedMeshes (idempotent -- calling Cancel again finds everything already empty/idle and
	// does nothing further, so no Ensure and no re-entrant cleanup).
	RecomputeOperationState();

	// 11. Deferred sync: if the scene selection changed while this (now-cancelled) operation was
	// pending, catch up with the CURRENT scene selection now -- OperationState is Idle at this point
	// (step 10), and the cancelled operation's original targets have already been fully discarded
	// (steps 3-9), so this can never retarget or interrupt anything. No-ops (SelectedMeshes stays
	// empty until the next real selection change) if the selection never changed during the session.
	SyncSelectionIfChangedDuringOperation();

	UE_LOG(LogVertexMaskForge, Log, TEXT("Vertex Mask Forge: Cancel ended the session -- tool selection list cleared, Editor selection untouched."));

	return FReply::Handled();
}

bool SVertexMaskForgePanel::AcceptPendingChanges()
{
	if (OperationState != EVertexMaskForgeOperationState::PendingChanges)
	{
		return false;
	}

	LastOperationErrorText = FText::GetEmpty();
	LastMaskActionStatusText = FText::GetEmpty();

	UE_LOG(LogVertexMaskForge, Log, TEXT("Vertex Mask Forge: Accept started (%d selected entries)."), SelectedMeshes.Num());

	// AUDITED (Nanite source-topology support): preflight BOTH domains BEFORE writing either one --
	// if EITHER fails, nothing is modified (the whole point of validating fully before the first
	// Modify() call). BuildAcceptTargets/BuildSourceTopologyAcceptTargets never produce overlapping
	// targets for the same asset (an entry is exclusively one domain or the other -- see
	// FVertexMaskForgeSelectedMesh::bUseSourceTopology), so there is no cross-domain collision to
	// reconcile, only a combined "nothing eligible at all" / "confirm N assets total" presentation.
	TArray<VertexMaskForgePanel::FVertexMaskForgeAcceptTarget> Targets;
	TArray<VertexMaskForgePanel::FVertexMaskForgeSourceTopologyAcceptTarget> SourceTopologyTargets;
	FText ErrorText;
	if (!VertexMaskForgePanel::BuildAcceptTargets(SelectedMeshes, Targets, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Warning, TEXT("Vertex Mask Forge: Accept blocked: %s"), *ErrorText.ToString());
		return false;
	}
	if (!VertexMaskForgePanel::BuildSourceTopologyAcceptTargets(SelectedMeshes, SourceTopologyTargets, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Warning, TEXT("Vertex Mask Forge: Accept blocked: %s"), *ErrorText.ToString());
		return false;
	}
	if (Targets.IsEmpty() && SourceTopologyTargets.IsEmpty())
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = LOCTEXT("AcceptNothingEligible", "No eligible pending changes to accept.");
		UE_LOG(LogVertexMaskForge, Warning, TEXT("Vertex Mask Forge: Accept blocked: %s"), *LastOperationErrorText.ToString());
		return false;
	}
	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Accept preflight succeeded -- %d render-vertex target(s), %d Source Topology / Nanite target(s)."),
		Targets.Num(), SourceTopologyTargets.Num());

	// Confirm the (permanent, all-instances-affected) destination before the first write. Not shown
	// for every minor adjustment -- only here, at the point of an actually destructive/permanent
	// operation.
	TArray<FString> AssetNames;
	AssetNames.Reserve(Targets.Num() + SourceTopologyTargets.Num());
	for (const VertexMaskForgePanel::FVertexMaskForgeAcceptTarget& Target : Targets)
	{
		AssetNames.Add(Target.AssetName);
	}
	for (const VertexMaskForgePanel::FVertexMaskForgeSourceTopologyAcceptTarget& Target : SourceTopologyTargets)
	{
		AssetNames.Add(FText::Format(LOCTEXT("AcceptConfirmNaniteSuffixFormat", "{0} (Nanite)"), FText::FromString(Target.AssetName)).ToString());
	}
	const int32 TotalTargetCount = Targets.Num() + SourceTopologyTargets.Num();
	const EAppReturnType::Type Choice = FMessageDialog::Open(
		EAppMsgType::OkCancel,
		FText::Format(
			LOCTEXT("AcceptConfirmFormat",
				"This will permanently write Vertex Colors into {0} Static Mesh Asset(s):\n\n{1}\n\n"
				"This affects EVERY instance/placement of these assets in every level, not just the "
				"currently selected one(s). This can be undone with Editor Undo.\n\nProceed?"),
			FText::AsNumber(TotalTargetCount),
			FText::FromString(FString::Join(AssetNames, TEXT("\n")))));
	if (Choice != EAppReturnType::Ok)
	{
		// User declined at the confirmation step -- Preview and state are untouched, not a failure.
		return false;
	}

	OperationState = EVertexMaskForgeOperationState::Applying;

	// AUDITED (Undo/Redo fix, root cause of "Accept doesn't undo"): ONE single top-level
	// FScopedTransaction, opened here and closed at the end of this scope block -- BEFORE any
	// RenderData/Nanite rebuild runs (see BuildModifiedMeshes' own doc comment for why the rebuild
	// must stay outside it). Both domains' writes (render-vertex and Source-Topology/Nanite) happen
	// inside this SAME transaction, so a mixed-domain Accept still produces exactly one Undo History
	// entry ("Apply Vertex Mask"), never two. WriteAcceptTargets/WriteSourceTopologyAcceptTargets no
	// longer open their own transactions or call Build() -- see their own doc comments for the
	// Mesh->ModifyMeshDescription() fix that makes the actual color change participate in the
	// Transaction Buffer at all (Mesh->Modify() alone was never enough).
	TArray<UStaticMesh*> ModifiedMeshes;
	ModifiedMeshes.Reserve(TotalTargetCount);
	{
		FScopedTransaction Transaction(LOCTEXT("AcceptVertexMaskForgeChanges", "Apply Vertex Mask"));

		if (!Targets.IsEmpty() && !VertexMaskForgePanel::WriteAcceptTargets(Targets, ModifiedMeshes, ErrorText))
		{
			OperationState = EVertexMaskForgeOperationState::Failed;
			LastOperationErrorText = ErrorText;
			UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept failed while writing: %s"), *ErrorText.ToString());
			return false;
		}
		if (!SourceTopologyTargets.IsEmpty() && !VertexMaskForgePanel::WriteSourceTopologyAcceptTargets(SourceTopologyTargets, ModifiedMeshes, ErrorText))
		{
			OperationState = EVertexMaskForgeOperationState::Failed;
			LastOperationErrorText = ErrorText;
			UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept (Source Topology) failed while writing: %s"), *ErrorText.ToString());
			return false;
		}
	}
	// Transaction closed above -- the source MeshDescription change for every target is now a single,
	// complete Undo History entry. Everything from here on (RenderData/Nanite rebuild, preview
	// teardown) is DERIVED or transient and must never be captured inside it.

	if (!VertexMaskForgePanel::BuildModifiedMeshes(ModifiedMeshes, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept failed while rebuilding: %s"), *ErrorText.ToString());
		return false;
	}

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Accepted Vertex Color changes for %d Static Mesh asset(s) (%d render-vertex, %d Source Topology / Nanite) -- all writes committed, all Builds completed."),
		TotalTargetCount, Targets.Num(), SourceTopologyTargets.Num());

	// Success: destroy the transient Preview (its job is done -- the colors now live permanently on
	// the asset, and every real SourceComponent already reflects them via the Build() calls above) and
	// return to Idle. AUDITED (BUG FIX round -- teardown semantics confirmed, not changed): this is the
	// SAME DestroyAllPreviews()/RestoreComponentOriginal() Cancel also uses, and that is correct for
	// BOTH cases, not a reused-by-mistake shortcut -- neither path ever mutates SourceComponent's own
	// OverrideVertexColors/materials (only the transient PreviewComponent/SourceTopologyPreviewComponent
	// ever carry those), so "restore" here only ever means "destroy the transient preview and un-hide
	// the real component" -- never "write old colors back". For Cancel, the asset was never touched, so
	// the real component still shows its original appearance (correct discard). For Accept (reached
	// only here, AFTER every write+verify+Build above already succeeded), the real component's own
	// RenderData/Nanite data was already rebuilt from the newly-committed colors BEFORE this call, so it
	// shows the ACCEPTED result, not anything stale -- there is no separate "old baseline" data path for
	// this function to accidentally restore.
	DestroyAllPreviews();
	UE_LOG(LogVertexMaskForge, Log, TEXT("Vertex Mask Forge: Accept -- preview torn down (Accept semantics: real components already show committed colors)."));

	OperationState = EVertexMaskForgeOperationState::Idle;
	LastOperationErrorText = FText::GetEmpty();

	// Deferred sync: the write above already completed against the ORIGINAL SelectedMeshes/Targets
	// captured before this call; only now, with OperationState settled back to Idle, is it safe to
	// catch up with a scene selection that may have changed while this operation was pending.
	SyncSelectionIfChangedDuringOperation();

	UE_LOG(LogVertexMaskForge, Log, TEXT("Vertex Mask Forge: Accept lifecycle finished (pending state cleared, OperationState=Idle)."));

	return true;
}

bool SVertexMaskForgePanel::HasNaniteMeshInSelection() const
{
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		const UStaticMesh* Mesh = Entry->Mesh.Get();
		if (IsValid(Mesh) && Mesh->IsNaniteEnabled())
		{
			return true;
		}
	}
	return false;
}

void SVertexMaskForgePanel::RestorePreviewForEntry(FVertexMaskForgeSelectedMesh& Entry)
{
	for (FVertexMaskForgePreviewComponentState& State : Entry.PreviewComponents)
	{
		VertexMaskForgePanel::RestoreComponentOriginal(State, ActorHideStates);
	}
}

void SVertexMaskForgePanel::RestorePreviewForEntryVisualOnly(FVertexMaskForgeSelectedMesh& Entry)
{
	for (FVertexMaskForgePreviewComponentState& State : Entry.PreviewComponents)
	{
		VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
	}
}

void SVertexMaskForgePanel::ApplyPreviewToEntry(
	const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry,
	const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr,
	const bool bCommit)
{
	if (!Entry.IsValid())
	{
		return;
	}

	// AUDITED (Original Textures fix): Preview Mode == OriginalMaterial used to short-circuit HERE,
	// destroying the transient preview and un-hiding SourceComponent -- SourceComponent's own materials
	// (correct) but its own PERSISTED colors (never the current WorkingColors), so Original Textures
	// could only ever show stale/already-committed data, never live edits. That contract violation is
	// the root cause of the reported bug (RGB/Red/Green/Blue update live, Original Textures does not):
	// those three modes all flow through the SAME per-component composition/preview path below;
	// OriginalMaterial alone bailed out before ever reaching it. Fixed by removing this early return --
	// OriginalMaterial now flows through the identical path as every other mode (same WorkingColors,
	// same live per-component BBox/AO re-evaluation, same "nothing valid yet -> visual-only restore"
	// fallback below), differing ONLY in which material each component's preview is configured with
	// (bUseOriginalMaterials, computed below and threaded through to
	// ApplyPreviewColorsToPreviewComponent / ApplySourceTopologyColorsToPreviewComponent) -- see those
	// functions' own doc comments. This never recomputes BBox/AO, never raycasts, never touches the
	// cache, Channel Filter, pending state, or Accept/Cancel -- it is purely which material renders the
	// SAME already-composed WorkingColors that were already being computed for every other mode anyway.

	if (Entry->PreviewComponents.IsEmpty())
	{
		// Content-Browser-only entry: nothing in the viewport to visualize. Not an error --
		// GetPreviewStatusText() communicates this explicitly.
		return;
	}

	const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->WorkingMesh;
	// AUDITED (composition-stack checkpoint): proceed if EITHER slot is Ready -- Bounding Box and
	// Ambient Occlusion are independent, optional layers now (see BoundingBoxMask's own doc comment).
	const bool bBBoxEntryReady = WorkingMesh.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready;
	// AUDITED (raw/composition separation checkpoint): gated LIVE on bAOEnabled, not just State --
	// disabling AO (OnAOEnableChanged, pure composition) never touches AmbientOcclusionMask/AOCache at
	// all (see that handler's own doc comment), so State can legitimately still read Ready while the
	// layer is meant to be excluded from the stack; bAOEnabled is the live, authoritative "is this
	// layer currently supposed to participate" signal.
	const bool bAOEntryReady = bAOEnabled && WorkingMesh.AmbientOcclusionMask.State == EVertexMaskForgeScalarMaskState::Ready;
	if (WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready
		|| !WorkingMesh.Mesh.IsValid()
		|| (!bBBoxEntryReady && !bAOEntryReady))
	{
		// Nothing safe to preview yet (both slots NotGenerated/Unavailable/DegenerateBounds/Invalid):
		// show the original colors/materials rather than a stale or fabricated result. AUDITED
		// (raw/composition separation checkpoint): visual-only -- this is not necessarily a geometric
		// invalidation (e.g. AO alone was just disabled and BBox was never enabled), so AOCache must
		// not be destroyed speculatively.
		RestorePreviewForEntryVisualOnly(*Entry);
		return;
	}

	// Resolved only for the duration of this call; consistent with the rest of the panel's
	// pattern of never storing a raw UStaticMesh pointer.
	const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
	if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
	{
		// AUDITED (raw/composition separation checkpoint): visual-only, not a confirmed geometric
		// change -- a transient resolve failure should not speculatively destroy AOCache either.
		RestorePreviewForEntryVisualOnly(*Entry);
		return;
	}

	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
	{
		RestorePreviewForEntryVisualOnly(*Entry);
		return;
	}

	// AUDITED (Original Textures fix): Original Textures never needs DebugMaterial at all (it configures
	// each slot from SourceComponent's own materials instead -- see ApplyPreviewColorsToPreviewComponent
	// / ApplySourceTopologyColorsToPreviewComponent), so a missing DebugMaterial asset must not block it.
	const bool bUseOriginalMaterials = (CurrentPreviewMode == EVertexMaskForgePreviewMode::OriginalMaterial);
	UMaterialInterface* DebugMaterial = GetPreviewDebugMaterial();
	if (!bUseOriginalMaterials && !DebugMaterial)
	{
		RestorePreviewForEntryVisualOnly(*Entry);
		return;
	}

	// Composed per-component (not once per entry): each SourceComponent may carry its own
	// pre-existing per-instance OverrideVertexColors, which must take priority over the asset's own
	// colors as the baseline (Problem 3) -- so two components sharing this asset but with different
	// per-instance paint can legitimately get different preview results. Always starts fresh from
	// each render vertex's own effective original color (preserving seams) and the current mask --
	// never from a previous composition -- so repeated filter/mode toggling cannot accumulate.
	for (FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
	{
		UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
		if (!IsValid(SourceComponent))
		{
			continue;
		}

		// AUDITED (peer-mask composition checkpoint): builds an UNORDERED set of mask generators for
		// THIS component, then hands it to UpdateWorkingColors/ComposeMaskStack, which sorts it by
		// Mask->Source and applies the fixed, mode-based canonical order internally -- see those
		// functions' own doc comments for the composition contract itself. The order Layers.Add() is
		// called in below is NOT semantically meaningful (Bounding Box happens to be checked first in
		// this code only for readability; it carries no priority) -- see ComposeMaskStack for why.
		// Two shapes:
		//   - Fill/Constant override (WorkingMesh.BoundingBoxMask.Source == ConstantWhite/Black): a
		//     SINGLE hard layer, exactly the pre-existing Fill contract -- Bounding Box axes/Ambient
		//     Occlusion are NOT combined with a Fill result (Fill is transform-independent, so the
		//     shared entry-level reference is reused directly, same as before this checkpoint).
		//   - Normal composition: up to TWO peer layers -- Bounding Box (if its slot is Ready) and
		//     Ambient Occlusion (if its slot is Ready) -- neither has priority over the other; both
		//     are re-evaluated PER COMPONENT, unconditionally: World Space Bounding Box axes vary per
		//     instance (audited, World Space checkpoint); Ambient Occlusion is ALWAYS transform-
		//     dependent (see GenerateAmbientOcclusionMask's own doc comment), using THIS component's
		//     own AOCache so the (potentially expensive) tree/raycast results are memoized per
		//     component -- see that function's CACHE doc comment for exactly what invalidates it. If
		//     EITHER enabled layer's per-component re-evaluation comes back not-Ready (degenerate World
		//     Space bounds, or an AO input that failed structural validation), OR the resulting set is
		//     empty, this component falls back to its original appearance -- never a stale or
		//     fabricated result.
		TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
		FVertexMaskForgeScalarMask PerComponentBBoxMask;
		FVertexMaskForgeScalarMask PerComponentAOMask;
		bool bAnyLayerFailed = false;

		// AUDITED (Nanite source-topology support): the two domains diverge starting here -- Source-
		// Topology entries never read RenderData/LODResources[0] at all (that would be the reduced
		// Nanite fallback), and use the FDynamicMesh3-based generators/caches/preview instead. See
		// FVertexMaskForgeSelectedMesh::bUseSourceTopology's own doc comment for the domain-selection
		// rule.
		if (Entry->bUseSourceTopology)
		{
			if (WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantWhite
				|| WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantBlack)
			{
				// GenerateConstantMaskForCornerDomain (see RunConstantFill) already built this mask in
				// the corner domain for a Source-Topology entry -- IndexOverride's default (-1) resolves
				// to the shared CornerIndex UpdateWorkingColorsSourceTopology passes as VertexIndex,
				// exactly like the render-vertex Fill path does with render vertex index.
				Layers.Add({ &WorkingMesh.BoundingBoxMask, EVertexMaskForgeBlendMode::Copy, 1.0f });
			}
			else
			{
				if (bBBoxEntryReady)
				{
					PerComponentBBoxMask = VertexMaskForgePanel::GenerateBoundingBoxMaskFromDynamicMesh(
						*WorkingMesh.Mesh, WorkingMesh.BoundingBoxMask.UsedAxisParams, SourceComponent->GetComponentTransform());
					if (PerComponentBBoxMask.State != EVertexMaskForgeScalarMaskState::Ready)
					{
						bAnyLayerFailed = true;
					}
					else
					{
						Layers.Add({ &PerComponentBBoxMask, BoundingBoxBlendMode, BoundingBoxOpacity });
					}
				}
				if (!bAnyLayerFailed && bAOEntryReady)
				{
					// Same "exactly one real computation site per component per update" contract as the
					// render-vertex path below -- see its own AUDITED note for the full history. bInvert
					// is the same live override, for the same reason. AUDITED (AO Levels): LevelsMin/Max
					// are LIVE overrides too, for the identical reason -- purely compositional, applied
					// fresh from AOCache.RawValues every call (see ApplyAOLevelsAndInvert), never
					// requiring a raycast/Tree rebuild.
					FVertexMaskForgeAOParams EffectiveAOParams = WorkingMesh.AmbientOcclusionMask.UsedAOParams;
					EffectiveAOParams.bInvert = bAOInvert;
					EffectiveAOParams.LevelsMin = AOLevelsMin;
					EffectiveAOParams.LevelsMax = AOLevelsMax;
					PerComponentAOMask = VertexMaskForgePanel::GenerateAmbientOcclusionMaskFromDynamicMesh(
						State.SourceTopologyAOCache, *WorkingMesh.Mesh, WorkingMesh.GeometryFingerprint,
						SourceComponent->GetComponentTransform(), EffectiveAOParams);
					if (PerComponentAOMask.State != EVertexMaskForgeScalarMaskState::Ready)
					{
						bAnyLayerFailed = true;
					}
					else
					{
						Layers.Add({ &PerComponentAOMask, AOBlendMode, AOOpacity });
					}
				}
			}

			if (bAnyLayerFailed || Layers.IsEmpty())
			{
				VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
				continue;
			}

			// AUDITED: mutates State.SourceTopologyBaselineColors/CommittedColors/WorkingColors in
			// place -- the SAME arrays Accept (source-topology commit) reads, never writes. bCommit
			// forwarded exactly like the render-vertex call below.
			int32 NumComposed = 0;
			VertexMaskForgePanel::UpdateWorkingColorsSourceTopology(
				State.SourceTopologyBaselineColors, State.SourceTopologyCommittedColors, State.SourceTopologyWorkingColors,
				Layers, *WorkingMesh.Mesh,
				bChannelFilterR, bChannelFilterG, bChannelFilterB,
				bCommit,
				NumComposed);

			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: composed %d/%d corner(s) for '%s' on component '%s' (Source Topology)."),
				NumComposed, WorkingMesh.Mesh->TriangleCount() * 3, *Entry->AssetName, *SourceComponent->GetName());

			VertexMaskForgePanel::ActivateSourceTopologyPreviewForComponent(
				State, *WorkingMesh.Mesh, State.SourceTopologyWorkingColors, DebugMaterial, bUseOriginalMaterials, ActorHideStates);
			continue;
		}

		// AUDITED (peer-mask composition checkpoint): builds an UNORDERED set of mask generators for
		// THIS component, then hands it to UpdateWorkingColors/ComposeMaskStack, which sorts it by
		// Mask->Source and applies the fixed, mode-based canonical order internally -- see those
		// functions' own doc comments for the composition contract itself. The order Layers.Add() is
		// called in below is NOT semantically meaningful (Bounding Box happens to be checked first in
		// this code only for readability; it carries no priority) -- see ComposeMaskStack for why.
		// Two shapes:
		//   - Fill/Constant override (WorkingMesh.BoundingBoxMask.Source == ConstantWhite/Black): a
		//     SINGLE hard layer, exactly the pre-existing Fill contract -- Bounding Box axes/Ambient
		//     Occlusion are NOT combined with a Fill result (Fill is transform-independent, so the
		//     shared entry-level reference is reused directly, same as before this checkpoint).
		//   - Normal composition: up to TWO peer layers -- Bounding Box (if its slot is Ready) and
		//     Ambient Occlusion (if its slot is Ready) -- neither has priority over the other; both
		//     are re-evaluated PER COMPONENT, unconditionally: World Space Bounding Box axes vary per
		//     instance (audited, World Space checkpoint); Ambient Occlusion is ALWAYS transform-
		//     dependent (see GenerateAmbientOcclusionMask's own doc comment), using THIS component's
		//     own AOCache so the (potentially expensive) tree/raycast results are memoized per
		//     component -- see that function's CACHE doc comment for exactly what invalidates it. If
		//     EITHER enabled layer's per-component re-evaluation comes back not-Ready (degenerate World
		//     Space bounds, or an AO input that failed structural validation), OR the resulting set is
		//     empty, this component falls back to its original appearance -- never a stale or
		//     fabricated result.
		if (WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantWhite
			|| WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantBlack)
		{
			Layers.Add({ &WorkingMesh.BoundingBoxMask, EVertexMaskForgeBlendMode::Copy, 1.0f });
		}
		else
		{
			if (bBBoxEntryReady)
			{
				PerComponentBBoxMask = VertexMaskForgePanel::GenerateBoundingBoxMask(
					RenderData->LODResources[0], WorkingMesh.BoundingBoxMask.UsedAxisParams, SourceComponent->GetComponentTransform(),
					CollectiveBoundsPtr);
				if (PerComponentBBoxMask.State != EVertexMaskForgeScalarMaskState::Ready)
				{
					// This specific instance's World Space evaluation came back degenerate/invalid
					// even though the entry-level reference was Ready.
					bAnyLayerFailed = true;
				}
				else
				{
					Layers.Add({ &PerComponentBBoxMask, BoundingBoxBlendMode, BoundingBoxOpacity });
				}
			}
			if (!bAnyLayerFailed && bAOEntryReady)
			{
				// AUDITED (double-AO-execution fix): this is the ONE AND ONLY call site in the whole
				// panel that ever performs a REAL Ambient Occlusion computation -- see
				// FVertexMaskForgeWorkingMesh::AmbientOcclusionMask's own doc comment. Previously, a
				// SEPARATE entry-level call (inside OnGenerateBoundingBoxMaskClicked/RunAutoUpdatePreview)
				// ALSO called GenerateAmbientOcclusionMask for the reference component, producing a
				// full second Tree-build+raycast pass every update in the Output Log even though the
				// two calls' cache keys should have matched -- rather than continue relying on that
				// cache comparison never diverging (its exact failure mode could not be conclusively
				// isolated via static analysis alone, without a live debugger), the redundant call
				// site was removed entirely: the entry-level pass now only validates cheaply (see
				// IsAmbientOcclusionInputValid) and snapshots UsedAOParams, never touching AOCache.
				// Combined with InvalidateAODerivedMask/InvalidateBoundingBoxRawMask (see their own doc
				// comments) that stopped a per-tick interim RestorePreviewForEntry from destroying
				// AOCache before the debounced regeneration even ran, Ambient Occlusion is now
				// structurally computed in exactly ONE place, exactly once per component per update.
				// AUDITED (raw/composition separation checkpoint, Invert live-override fix): Samples/
				// MaxDistance/Bias come from the entry-level snapshot (UsedAOParams) -- those ARE
				// geometric/cache-relevant, correctly resolved at the last real (re)generation (full
				// Samples for an explicit Generate Mask, capped for Auto Update -- see
				// OnGenerateBoundingBoxMaskClicked/RunAutoUpdatePreview). bInvert is deliberately
				// OVERRIDDEN with the LIVE bAOInvert panel value here, every call -- Invert is pure
				// composition (OnAOInvertChanged never re-snapshots UsedAOParams, see its own doc
				// comment), so using the stale snapshot's bInvert would silently ignore an Invert
				// toggle until the next real regeneration. GenerateAmbientOcclusionMask applies Invert
				// fresh from AOCache.RawValues every call regardless (see its own doc comment) -- this
				// override only makes sure it uses the CURRENT checkbox state, never a stale one.
				// AUDITED (AO Levels): LevelsMin/Max are LIVE overrides too, same reasoning as bInvert --
				// purely compositional, applied fresh from AOCache.RawValues every call (see
				// ApplyAOLevelsAndInvert), never requiring a raycast/Tree rebuild.
				FVertexMaskForgeAOParams EffectiveAOParams = WorkingMesh.AmbientOcclusionMask.UsedAOParams;
				EffectiveAOParams.bInvert = bAOInvert;
				EffectiveAOParams.LevelsMin = AOLevelsMin;
				EffectiveAOParams.LevelsMax = AOLevelsMax;
				PerComponentAOMask = VertexMaskForgePanel::GenerateAmbientOcclusionMask(
					State.AOCache, Mesh, RenderData->LODResources[0], SourceComponent->GetComponentTransform(),
					EffectiveAOParams);
				if (PerComponentAOMask.State != EVertexMaskForgeScalarMaskState::Ready)
				{
					bAnyLayerFailed = true;
				}
				else
				{
					Layers.Add({ &PerComponentAOMask, AOBlendMode, AOOpacity });
				}
			}
		}

		if (bAnyLayerFailed || Layers.IsEmpty())
		{
			// AUDITED (raw/composition separation checkpoint, AOCache lifetime fix): visual-only --
			// see RestorePreviewVisualOnly's own doc comment. Neither "a per-component layer
			// re-evaluation genuinely failed" nor "no layer is currently enabled" is a session end or
			// a real geometric invalidation, so BaselineColors/CommittedColors/WorkingColors/AOCache
			// must survive this fallback untouched.
			VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
			continue;
		}

		// Read-only: this buffer belongs to SourceComponent and is never modified by the plugin.
		const FColorVertexBuffer* InstanceOverrideColors =
			SourceComponent->LODData.IsValidIndex(0) ? SourceComponent->LODData[0].OverrideVertexColors : nullptr;

		// TEMPORARY diagnostic (audited render-vertex-order fix): NumComposed vs the LOD's render
		// vertex count directly proves the 1:1 correspondence in the log, per-component, every time
		// the preview is (re)applied.
		//
		// AUDITED (recompute-at-Accept fix / Channel Filter toggle fix): mutates
		// State.BaselineColors/CommittedColors/WorkingColors in place -- the SAME arrays Accept reads
		// (never writes). bCommit forwarded from this call's own
		// caller (UpdateAllPreviews) -- true ONLY for an explicit Generate Mask click or Fill. See
		// UpdateWorkingColors' own doc comment for the full baseline/committed/working contract.
		int32 NumComposed = 0;
		VertexMaskForgePanel::UpdateWorkingColors(
			State.BaselineColors, State.CommittedColors, State.WorkingColors, Layers, RenderData->LODResources[0], InstanceOverrideColors,
			bChannelFilterR, bChannelFilterG, bChannelFilterB,
			bCommit,
			NumComposed);
		const TArray<FColor> RenderOrderColors = VertexMaskForgePanel::DeriveDisplayColors(State.WorkingColors, CurrentPreviewMode);

		const int32 NumRenderVertsForLog = static_cast<int32>(RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices());
		UE_LOG(LogVertexMaskForge, Verbose,
			TEXT("Vertex Mask Forge: composed %d/%d render vertices for '%s' on component '%s' (Preview Mode=%d, originalTextures=%s)."),
			NumComposed, NumRenderVertsForLog, *Entry->AssetName, *SourceComponent->GetName(),
			static_cast<int32>(CurrentPreviewMode), bUseOriginalMaterials ? TEXT("true") : TEXT("false"));

		VertexMaskForgePanel::ActivatePreviewForComponent(State, RenderOrderColors, DebugMaterial, bUseOriginalMaterials, ActorHideStates);
	}
}

void SVertexMaskForgePanel::UpdateAllPreviews(const bool bCommit)
{
	// Computed ONCE per refresh (never cached across calls, consistent with the rest of the panel),
	// then reused for every entry below -- Unified Bounds must never recompute just one mesh in
	// isolation. On failure, only entries actually showing a BoundingBox-sourced preview are
	// affected (restored to original, per the atomic-validation contract); Fill-sourced or
	// not-yet-generated entries are untouched since they never depend on this domain.
	TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> CollectiveBounds;
	const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr = nullptr;
	if (bUseUnifiedBounds)
	{
		FText CollectiveError;
		if (VertexMaskForgePanel::ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/false, CollectiveBounds, CollectiveError))
		{
			CollectiveBoundsPtr = &CollectiveBounds;
			// Clears a stale Unified Bounds failure message from a previous refresh; does not touch
			// LastOperationErrorText when bUseUnifiedBounds is off, so unrelated errors (e.g. Accept)
			// are never disturbed by this path.
			LastOperationErrorText = FText::GetEmpty();
		}
		else
		{
			for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
			{
				if (Entry.IsValid() && Entry->WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::BoundingBox)
				{
					// AUDITED (raw/composition separation checkpoint): visual-only -- this failure is
					// Unified Bounds (Bounding-Box-specific); Ambient Occlusion's own AOCache is
					// unrelated and must not be destroyed as collateral damage.
					RestorePreviewForEntryVisualOnly(*Entry);
				}
			}
			LastOperationErrorText = CollectiveError;
			RecomputeOperationState();
			return;
		}
	}

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		ApplyPreviewToEntry(Entry, CollectiveBoundsPtr, bCommit);
	}

	RecomputeOperationState();
}

void SVertexMaskForgePanel::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (!World)
	{
		return;
	}

	// Never write to any asset here, and never let a pending Auto Update Preview debounce fire after
	// this point -- World cleanup must be non-interactive, side-effect-free cleanup only.
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}

	// Restores/destroys only the previews that actually belong to World -- checked against
	// SourceComponent, PreviewComponent, and HiddenOwner independently (rather than only
	// SourceComponent) because any one of the three may already be the sole surviving reference if
	// the others were destroyed earlier (Problem 5 partial-failure scenarios). Never touches previews
	// belonging to a different, still-live World. Pure cleanup: does not call RefreshSelection, does
	// not recreate anything, and does not assume GEditor is otherwise valid -- safe to run during
	// editor shutdown as well as ordinary level changes/PIE end.
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		for (FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
		{
			const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
			const UStaticMeshComponent* PreviewComponentPtr = State.PreviewComponent.Get();
			const AActor* HiddenOwnerPtr = State.HiddenOwner.Get();

			const bool bBelongsToCleaningWorld =
				(SourceComponent && SourceComponent->GetWorld() == World) ||
				(PreviewComponentPtr && PreviewComponentPtr->GetWorld() == World) ||
				(HiddenOwnerPtr && HiddenOwnerPtr->GetWorld() == World);
			if (!bBelongsToCleaningWorld)
			{
				continue;
			}

			VertexMaskForgePanel::RestoreComponentOriginal(State, ActorHideStates);
		}
	}

	// UI-only: reflects that some previews may have just been force-destroyed for this World. Does
	// not write to any asset and does not touch LastOperationErrorText.
	RecomputeOperationState();
}

void SVertexMaskForgePanel::DestroyAllPreviews()
{
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			RestorePreviewForEntry(*Entry);
		}
	}
}

#undef LOCTEXT_NAMESPACE
