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
#include "VectorUtil.h"
#include "VertexMaskForgeAcceptTargetBuilder.h"
#include "VertexMaskForgeAcceptWriter.h"
#include "VertexMaskForgeAmbientOcclusionGenerator.h"
#include "VertexMaskForgeBoundingBoxGenerator.h"
#include "VertexMaskForgeCurvatureGenerator.h"
#include "VertexMaskForgeDirectionalNormalGenerator.h"
#include "VertexMaskForgeDisplayColorDerivation.h"
#include "VertexMaskForgeGeneratorLayerBridge.h"
#include "VertexMaskForgeGeneratorUtils.h"
#include "VertexMaskForgeLayerOrder.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeNoiseGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeThicknessGenerator.h"
#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingStateOwner.h"
#include "SPrimaryButton.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY(LogVertexMaskForge);

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
			TArray<TUniquePtr<FVertexMaskForgeWorkingStateOwner>>& PreviewComponents = InOutMeshes[EntryIndex]->PreviewComponents;
			const bool bAlreadyTracked = PreviewComponents.ContainsByPredicate(
				[SourceComponent](const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner)
				{
					return StateOwner->GetPreviewState().GetSourceComponent().Get() == SourceComponent;
				});
			if (!bAlreadyTracked)
			{
				// M16-J.0B: the state owner's target component is configured here, and it is attached
				// to THIS entry's own mesh owner immediately -- the mesh owner may not have its identity/
				// Working Mesh installed yet (that happens later, in BuildWorkingMeshes), but the
				// attachment itself (a TWeakPtr) is always safe to establish early.
				TUniquePtr<FVertexMaskForgeWorkingStateOwner> NewStateOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
				NewStateOwner->ConfigureTarget(SourceComponent);
				NewStateOwner->AttachToMeshOwner(InOutMeshes[EntryIndex]->MeshOwner);
				PreviewComponents.Add(MoveTemp(NewStateOwner));
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

	// --- Material Slot Mask (V2-D) -------------------------------------------------------------------

	/** "Slot {Index}: {SlotName} — {MaterialAssetName}" -- never just the material name (the same
	 *  material can appear in several slots) and never just the slot name (names can duplicate). */
	static FText GetMaterialSlotLabel(const FVertexMaskForgeMaterialSlotInfo& Info)
	{
		const FText SlotNameText = Info.MaterialSlotName.IsNone()
			? LOCTEXT("MaterialSlotUnnamed", "(unnamed)")
			: FText::FromName(Info.MaterialSlotName);
		return FText::Format(
			LOCTEXT("MaterialSlotLabelFormat", "Slot {0}: {1} — {2}"),
			FText::AsNumber(Info.SlotIndex), SlotNameText, FText::FromString(Info.MaterialAssetName));
	}

	/**
	 * AUDITED (V2-D, M0-A): resolves EVERY FPolygonGroupID actually present in MeshDescriptionCopy to a
	 * REAL Static Material Slot index, via the SAME correspondence UStaticMesh's own reimport code uses
	 * (UStaticMesh::GetMaterialIndexFromImportedMaterialSlotName, StaticMesh.cpp -- confirmed by reading
	 * the engine source: reimport slot remapping resolves
	 * `GetMaterialIndexFromImportedMaterialSlotName(ExistingMaterialSlotNames[PolygonGroupID])`, the
	 * EXACT SAME PolygonGroupID -> ImportedMaterialSlotName -> StaticMaterials-index chain used here).
	 * UNLIKE that native function (a plain linear first-match scan, confirmed by reading its body --
	 * it does NOT itself detect duplicate ImportedMaterialSlotName values), this wrapper explicitly
	 * checks for duplicates FIRST and refuses to resolve through an ambiguous name at all -- per the
	 * explicit "não aceitar uma correspondência ambígua silenciosamente" requirement. Never uses
	 * MaterialIDAttrib/compacted Polygon Group IDs -- only real FPolygonGroupID objects and real name
	 * matching. Returns one resolved index (or INDEX_NONE) per FPolygonGroupID actually returned by
	 * MeshDescriptionCopy.PolygonGroups().GetElementIDs().
	 */
	static TMap<FPolygonGroupID, int32> ResolvePolygonGroupsToMaterialSlots(
		const UStaticMesh* Mesh,
		const FMeshDescription& MeshDescriptionCopy,
		bool& bOutAllResolved)
	{
		bOutAllResolved = true;
		TMap<FPolygonGroupID, int32> Result;

		if (!IsValid(Mesh))
		{
			bOutAllResolved = false;
			return Result;
		}

		// Duplicate-safe name -> index map: a name seen more than once (including NAME_None appearing
		// on two or more slots) is stored as INDEX_NONE, never silently resolved to "whichever came
		// first" the way the native GetMaterialIndexFromImportedMaterialSlotName would.
		TMap<FName, int32> NameToUniqueSlotIndex;
		const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
		for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
		{
			const FName SlotName = StaticMaterials[SlotIndex].ImportedMaterialSlotName;
			if (const int32* Existing = NameToUniqueSlotIndex.Find(SlotName))
			{
				NameToUniqueSlotIndex.Add(SlotName, INDEX_NONE); // Duplicate -- permanently ambiguous.
				(void)Existing;
			}
			else
			{
				NameToUniqueSlotIndex.Add(SlotName, SlotIndex);
			}
		}

		const FStaticMeshConstAttributes Attributes(MeshDescriptionCopy);
		const TPolygonGroupAttributesConstRef<FName> GroupSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		for (const FPolygonGroupID GroupID : MeshDescriptionCopy.PolygonGroups().GetElementIDs())
		{
			const FName GroupSlotName = GroupSlotNames.IsValid() && GroupSlotNames.GetNumElements() > 0
				? GroupSlotNames.Get(GroupID)
				: NAME_None;
			const int32* Resolved = NameToUniqueSlotIndex.Find(GroupSlotName);
			const int32 ResolvedIndex = (Resolved && *Resolved != INDEX_NONE && StaticMaterials.IsValidIndex(*Resolved))
				? *Resolved
				: INDEX_NONE;
			Result.Add(GroupID, ResolvedIndex);
			if (ResolvedIndex == INDEX_NONE)
			{
				bOutAllResolved = false;
			}
		}

		return Result;
	}

	/**
	 * AUDITED (V2-D, M0-A/M0-B): builds BOTH domains' Material Slot lookups for one working mesh, once,
	 * at BuildWorkingMeshForStaticMesh time (while MeshDescriptionCopy/TriIDMap/LOD0 are all still in
	 * scope) -- never recomputed per generation, only rebuilt on the next RefreshSelection. Populates
	 * WorkingMesh.MaterialSlotOptions (the dropdown's own data source) from Mesh->GetStaticMaterials()
	 * directly (real indices, real names -- a null material's slot reports "None").
	 */
	static void BuildMaterialSlotLookups(
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		const UStaticMesh* Mesh,
		const FMeshDescription& MeshDescriptionCopy,
		const TArray<FTriangleID>& TriIDMap,
		const FStaticMeshLODResources* LOD0)
	{
		WorkingMesh.MaterialSlotOptions.Reset();
		WorkingMesh.DynamicTriangleToMaterialSlot.Reset();
		WorkingMesh.RenderVertexToMaterialSlot.Reset();
		WorkingMesh.bMaterialSlotResolutionValid = true;
		WorkingMesh.bRenderVertexMaterialSlotAmbiguous = false;

		if (!IsValid(Mesh) || !WorkingMesh.Mesh.IsValid())
		{
			WorkingMesh.bMaterialSlotResolutionValid = false;
			return;
		}

		const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
		WorkingMesh.MaterialSlotOptions.Reserve(StaticMaterials.Num());
		for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
		{
			FVertexMaskForgeMaterialSlotInfo Info;
			Info.SlotIndex = SlotIndex;
			Info.MaterialSlotName = StaticMaterials[SlotIndex].MaterialSlotName;
			const UMaterialInterface* MaterialAsset = StaticMaterials[SlotIndex].MaterialInterface;
			Info.MaterialAssetName = IsValid(MaterialAsset) ? MaterialAsset->GetName() : TEXT("None");
			WorkingMesh.MaterialSlotOptions.Add(MoveTemp(Info));
		}

		// --- M0-A: Dynamic TriangleID -> real Material Slot, via TriIDMap + PolygonGroup resolution ---
		bool bAllGroupsResolved = false;
		const TMap<FPolygonGroupID, int32> GroupToSlot = ResolvePolygonGroupsToMaterialSlots(Mesh, MeshDescriptionCopy, bAllGroupsResolved);

		const UE::Geometry::FDynamicMesh3& DynMesh = *WorkingMesh.Mesh;
		const int32 MaxTriangleID = DynMesh.MaxTriangleID();
		WorkingMesh.DynamicTriangleToMaterialSlot.Init(INDEX_NONE, MaxTriangleID);

		bool bAnyTriangleUnresolved = false;
		for (const int32 DynamicTriangleID : DynMesh.TriangleIndicesItr())
		{
			if (!TriIDMap.IsValidIndex(DynamicTriangleID))
			{
				bAnyTriangleUnresolved = true;
				continue;
			}
			const FTriangleID SourceTriangleID = TriIDMap[DynamicTriangleID];
			if (!MeshDescriptionCopy.IsTriangleValid(SourceTriangleID))
			{
				bAnyTriangleUnresolved = true;
				continue;
			}
			const FPolygonGroupID GroupID = MeshDescriptionCopy.GetTrianglePolygonGroup(SourceTriangleID);
			const int32* ResolvedSlot = GroupToSlot.Find(GroupID);
			if (!ResolvedSlot || *ResolvedSlot == INDEX_NONE)
			{
				bAnyTriangleUnresolved = true;
				continue;
			}
			WorkingMesh.DynamicTriangleToMaterialSlot[DynamicTriangleID] = *ResolvedSlot;
		}
		WorkingMesh.bMaterialSlotResolutionValid = bAllGroupsResolved && !bAnyTriangleUnresolved;

		// --- M0-B: LOD0 Render Vertex Index -> real Material Slot, via Sections + IndexBuffer -------
		if (!LOD0)
		{
			return; // Source-Topology entry: non-Nanite lookup intentionally left empty/unused.
		}
		const int32 NumRenderVerts = static_cast<int32>(LOD0->VertexBuffers.PositionVertexBuffer.GetNumVertices());
		if (NumRenderVerts <= 0)
		{
			return;
		}
		WorkingMesh.RenderVertexToMaterialSlot.Init(INDEX_NONE, NumRenderVerts);

		for (const FStaticMeshSection& Section : LOD0->Sections)
		{
			if (!StaticMaterials.IsValidIndex(Section.MaterialIndex))
			{
				continue; // Never guess -- a Section pointing outside the slot table resolves nothing.
			}
			const uint32 FirstIndex = Section.FirstIndex;
			const uint32 NumIndices = Section.NumTriangles * 3u;
			if (FirstIndex + NumIndices > static_cast<uint32>(LOD0->IndexBuffer.GetNumIndices()))
			{
				continue; // Malformed section range -- never read out of bounds.
			}
			for (uint32 i = 0; i < NumIndices; ++i)
			{
				const uint32 RenderIndexU = LOD0->IndexBuffer.GetIndex(FirstIndex + i);
				if (RenderIndexU >= static_cast<uint32>(NumRenderVerts))
				{
					continue;
				}
				const int32 RenderIndex = static_cast<int32>(RenderIndexU);
				const int32 Existing = WorkingMesh.RenderVertexToMaterialSlot[RenderIndex];
				if (Existing == INDEX_NONE)
				{
					WorkingMesh.RenderVertexToMaterialSlot[RenderIndex] = Section.MaterialIndex;
				}
				else if (Existing != Section.MaterialIndex)
				{
					// AUDITED (M0-B): this render vertex is referenced by triangles from two DIFFERENT
					// Sections/MaterialIndex values -- never resolved to first/last/min/max. Marked
					// permanently ambiguous for this vertex AND the whole entry (see
					// bRenderVertexMaterialSlotAmbiguous's own doc comment); the non-Nanite Material
					// Slot Mask refuses to generate for this entry rather than risk bleeding.
					WorkingMesh.RenderVertexToMaterialSlot[RenderIndex] = INDEX_NONE;
					WorkingMesh.bRenderVertexMaterialSlotAmbiguous = true;
				}
			}
		}
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

		// AUDITED (V2-D): built here, while MeshDescriptionCopy/TriIDMap/LOD0 are all still in scope --
		// see BuildMaterialSlotLookups' own doc comment. LOD0 is only available for a non-Source-
		// Topology mesh with valid render data; passing nullptr there is intentional (the render-vertex
		// lookup is simply left empty/unused for a Source-Topology entry, exactly like TriIDMap is
		// unused the other direction).
		{
			const FStaticMeshRenderData* RenderDataForMaterialSlots = Mesh->GetRenderData();
			const FStaticMeshLODResources* LOD0ForMaterialSlots =
				(RenderDataForMaterialSlots && RenderDataForMaterialSlots->LODResources.IsValidIndex(0))
				? &RenderDataForMaterialSlots->LODResources[0]
				: nullptr;
			BuildMaterialSlotLookups(WorkingMesh, Mesh, *MeshDescriptionCopy, TriIDMap, LOD0ForMaterialSlots);
		}

		// AUDITED (Nanite source-topology support): persisted for the Accept commit path -- see
		// FVertexMaskForgeWorkingMesh::TriIDMap's own doc comment. Moved (not copied): TriIDMap is a
		// local variable, never read again after this point in this function.
		WorkingMesh.TriIDMap = MoveTemp(TriIDMap);

		// AUDITED (Nanite source-topology support): guarantee a Normal Overlay exists, then compute the
		// geometry fingerprint AFTER it -- both must happen exactly once, here, before Mesh is ever
		// handed to a generator or cache. See EnsureNormalOverlay/ComputeDynamicMeshGeometryFingerprint
		// for why (hard-edge AO correctness / cache identity robustness).
		EnsureNormalOverlay(*WorkingMesh.Mesh, Mesh->GetName());
		WorkingMesh.GeometryFingerprint = VertexMaskForgeGeneratorUtils::ComputeDynamicMeshGeometryFingerprint(*WorkingMesh.Mesh);

		WorkingMesh.State = EVertexMaskForgeWorkingMeshState::Ready;
		return WorkingMesh;
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

	// --- Noise: procedural 3D Perlin/FBM, Local Space (V1) ------------------------------------------

	static FText GetNoiseTypeLabel(const EVertexMaskForgeNoiseType Type)
	{
		switch (Type)
		{
		case EVertexMaskForgeNoiseType::Perlin:
			return LOCTEXT("NoiseTypePerlin", "Perlin");
		case EVertexMaskForgeNoiseType::FractalPerlin:
			return LOCTEXT("NoiseTypeFractalPerlin", "Fractal Perlin (FBM)");
		case EVertexMaskForgeNoiseType::Billow:
			return LOCTEXT("NoiseTypeBillow", "Billow");
		case EVertexMaskForgeNoiseType::Ridged:
			return LOCTEXT("NoiseTypeRidged", "Ridged");
		case EVertexMaskForgeNoiseType::Turbulence:
			return LOCTEXT("NoiseTypeTurbulence", "Turbulence");
		case EVertexMaskForgeNoiseType::WorleyF1:
			return LOCTEXT("NoiseTypeWorleyF1", "Worley F1");
		case EVertexMaskForgeNoiseType::WorleyF2MinusF1:
			return LOCTEXT("NoiseTypeWorleyF2MinusF1", "Worley F2 - F1");
		case EVertexMaskForgeNoiseType::Voronoi:
			return LOCTEXT("NoiseTypeVoronoi", "Voronoi");
		case EVertexMaskForgeNoiseType::Alligator:
			return LOCTEXT("NoiseTypeAlligator", "Alligator");
		default:
			return FText::GetEmpty();
		}
	}

	// --- Directional Normal Mask (V2-E) --------------------------------------------------------------

	static FText GetNormalDirectionLabel(const EVertexMaskForgeNormalDirection Direction)
	{
		switch (Direction)
		{
		case EVertexMaskForgeNormalDirection::PositiveX: return LOCTEXT("NormalDirectionPositiveX", "X+ (Forward)");
		case EVertexMaskForgeNormalDirection::NegativeX: return LOCTEXT("NormalDirectionNegativeX", "X- (Backward)");
		case EVertexMaskForgeNormalDirection::PositiveY: return LOCTEXT("NormalDirectionPositiveY", "Y+ (Right)");
		case EVertexMaskForgeNormalDirection::NegativeY: return LOCTEXT("NormalDirectionNegativeY", "Y- (Left)");
		case EVertexMaskForgeNormalDirection::PositiveZ: return LOCTEXT("NormalDirectionPositiveZ", "Z+ (Up)");
		case EVertexMaskForgeNormalDirection::NegativeZ: return LOCTEXT("NormalDirectionNegativeZ", "Z- (Down)");
		default: return FText::GetEmpty();
		}
	}

	static FText GetNormalSpaceLabel(const EVertexMaskForgeNormalSpace Space)
	{
		switch (Space)
		{
		case EVertexMaskForgeNormalSpace::Local: return LOCTEXT("NormalSpaceLocal", "Local Space");
		case EVertexMaskForgeNormalSpace::World: return LOCTEXT("NormalSpaceWorld", "World Space");
		default: return FText::GetEmpty();
		}
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

	/**
	 * M16-K.4: human-readable label for one EVertexMaskForgeLayerFill value, used only by the Dynamic
	 * Layers list's own Fill combo (see SVertexMaskForgePanel::BuildDynamicLayerRow). Mirrors
	 * GetBlendModeLabel's own shape -- explicit switch, no numeric/contiguity assumption. None/Black/
	 * White are the only three real enumerators; there is no procedural/textured Fill in this checkpoint.
	 */
	static FText GetDynamicLayerFillLabel(const EVertexMaskForgeLayerFill Fill)
	{
		switch (Fill)
		{
		case EVertexMaskForgeLayerFill::None:
			return LOCTEXT("DynamicLayerFillNone", "None");
		case EVertexMaskForgeLayerFill::Black:
			return LOCTEXT("DynamicLayerFillBlack", "Black");
		case EVertexMaskForgeLayerFill::White:
			return LOCTEXT("DynamicLayerFillWhite", "White");
		default:
			return FText::GetEmpty();
		}
	}

	static FText GetCurvatureTypeLabel(const EVertexMaskForgeCurvatureType Type)
	{
		switch (Type)
		{
		case EVertexMaskForgeCurvatureType::Convex:
			return LOCTEXT("CurvatureTypeConvex", "Convex");
		case EVertexMaskForgeCurvatureType::Concave:
			return LOCTEXT("CurvatureTypeConcave", "Concave");
		case EVertexMaskForgeCurvatureType::Both:
			return LOCTEXT("CurvatureTypeBoth", "Both");
		default:
			return FText::GetEmpty();
		}
	}

	/**
	 * M16-K.3: stable, human-readable name for one generator layer identity, used only by the Layers
	 * list's own rows (see SVertexMaskForgePanel::BuildGeneratorLayerRow). Explicit mapping, one case
	 * per generator -- never derived from the enum's own numeric value or declaration order. Matches
	 * each generator's own existing section title verbatim (BBoxMaskSectionTitle/AOSectionTitle/
	 * CurvatureSectionTitle/NoiseSectionTitle/MaterialSlotMaskSectionTitle/
	 * DirectionalNormalMaskSectionTitle/ThicknessMaskSectionTitle) so the same generator is never named
	 * two different ways in the same panel. ConstantWhite/ConstantBlack (Fill overrides, not generators)
	 * and any unknown/cast-invalid value fall to the diagnosed default -- never a silent empty row (see
	 * VertexMaskForgeLayerOrder::IsGeneratorLayer's own allowlist, which this switch mirrors).
	 */
	static FText GetGeneratorLayerDisplayName(const EVertexMaskForgeScalarMaskSource Source)
	{
		switch (Source)
		{
		case EVertexMaskForgeScalarMaskSource::BoundingBox:
			return LOCTEXT("LayerRowBoundingBox", "Bounding Box");
		case EVertexMaskForgeScalarMaskSource::AmbientOcclusion:
			return LOCTEXT("LayerRowAmbientOcclusion", "Ambient Occlusion");
		case EVertexMaskForgeScalarMaskSource::Curvature:
			return LOCTEXT("LayerRowCurvature", "Curvature");
		case EVertexMaskForgeScalarMaskSource::Noise:
			return LOCTEXT("LayerRowNoise", "Noise");
		case EVertexMaskForgeScalarMaskSource::MaterialSlot:
			return LOCTEXT("LayerRowMaterialSlot", "Material Slot");
		case EVertexMaskForgeScalarMaskSource::DirectionalNormal:
			return LOCTEXT("LayerRowDirectionalNormal", "Directional Normal");
		case EVertexMaskForgeScalarMaskSource::Thickness:
			return LOCTEXT("LayerRowThickness", "Thickness");
		default:
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("Vertex Mask Forge: GetGeneratorLayerDisplayName received a non-generator Source (%d) -- this should never happen for a row built from GeneratorLayerOrder."),
				static_cast<int32>(Source));
			return LOCTEXT("LayerRowUnknown", "<Unknown Layer>");
		}
	}

	// --- Blend Mode math (see EVertexMaskForgeBlendMode) -------------------------------------
	// AUDITED (M16-J final): the panel-level ComposeMaskStack wrapper (which called
	// VertexMaskForgeMaskStackComposer::ComposeStack, the legacy fixed-stage/Blend-Mode-grouped
	// compositor) was REMOVED from this file -- both of its real call sites (ComputeComposedColorsRGB /
	// ComputeComposedColorsRGBSourceTopology below) now call
	// VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential instead, which threads the
	// SAME already-sorted Layers array through the new strictly-sequential fold
	// (VertexMaskForgeSequentialEvaluator::EvaluateFillLayers) rather than ComposeStack's fixed stages.
	// VertexMaskForgeMaskStackComposer::ComposeStack itself is NOT deleted -- it remains, unmodified, as
	// the legacy algorithm characterized/protected by
	// VertexMaskForge.SequentialEvaluator.Legacy.ComposeStackCharacterization
	// (VertexMaskForgeSequentialEvaluatorTests.cpp); this panel simply no longer calls it.

	// ApplyPreviewModeDisplay moved to VertexMaskForgeDisplayColorDerivation.cpp (M3 extraction, private
	// helper there) -- see DeriveDisplayColors' own call site below for the only place this panel still
	// needs Preview Mode display reduction.
	//
	// AUDITED: ToDisplayFColor stays here, unchanged -- UpdateWorkingColors/UpdateWorkingColorsSourceTopology
	// below use it to build the real WorkingColors buffer itself (not just for display), so it could not
	// move without altering those two functions (out of scope for M3). VertexMaskForgeDisplayColorDerivation.cpp
	// has its own private, identical copy for its own (display-only) use.
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
	 * M16-J.0B (2nd rejection-corrective pass): the ONE-TIME baseline capture, extracted out of the
	 * composition math below so it can run BEFORE FVertexMaskForgeWorkingStateOwner::EnsureBaselineCaptured
	 * -- the owner now exclusively decides whether a real capture happens (idempotent on its own
	 * AreColorsInitialized() state), never this function; this function only ever computes what the
	 * bytes WOULD be if asked. Same exact priority as before this checkpoint (AUDITED, Problem 3):
	 *   1. InstanceOverrideColors (SourceComponent's own PRE-EXISTING per-instance OverrideVertexColors)
	 *      IF non-null and its vertex count matches LOD0's.
	 *   2. Otherwise, the asset's own LOD0 ColorVertexBuffer (RenderData), if its count matches.
	 *   3. Otherwise, white.
	 * A buffer present but with a mismatched vertex count is treated exactly like absent.
	 */
	static TArray<FColor> CaptureBaselineColorsRenderVertex(const FStaticMeshLODResources& LOD0, const FColorVertexBuffer* InstanceOverrideColors)
	{
		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const uint32 NumRenderVerts = RenderPositions.GetNumVertices();

		const FColorVertexBuffer& AssetRenderColors = LOD0.VertexBuffers.ColorVertexBuffer;
		const bool bHasInstanceOverride =
			InstanceOverrideColors != nullptr && InstanceOverrideColors->GetNumVertices() == NumRenderVerts;
		const bool bHasAssetColors = !bHasInstanceOverride && AssetRenderColors.GetNumVertices() == NumRenderVerts;

		TArray<FColor> Captured;
		Captured.SetNumUninitialized(NumRenderVerts);
		for (uint32 i = 0; i < NumRenderVerts; ++i)
		{
			// This render vertex's OWN effective original color -- never a value borrowed from a
			// different render vertex (preserves seams).
			Captured[i] = bHasInstanceOverride ? InstanceOverrideColors->VertexColor(i)
				: bHasAssetColors ? AssetRenderColors.VertexColor(i)
				: FColor::White;
		}
		return Captured;
	}

	/**
	 * M16-J.0B (2nd rejection-corrective pass): computes the composed RGB result for one component's
	 * render-vertex domain -- reads BaselineColors/CommittedColors as CONST INPUT (owned by
	 * FVertexMaskForgeWorkingStateOwner, obtained via its own GetBaselineColors()/GetCommittedColors()
	 * getters), writes the result into a FRESH, INDEPENDENT local array (OutFinalColors) the caller then
	 * hands to FVertexMaskForgeWorkingStateOwner::ApplyComposedColorsRGB -- this function never touches
	 * owner storage, mutable or otherwise. The composition MATH ITSELF (sort order, ComposeMaskStack
	 * call, per-vertex Alpha-from-Baseline, "no layer contributed" fallback to Committed) is byte-for-
	 * byte UNCHANGED from before this checkpoint; only where the result is written changed.
	 *
	 * AUDITED (Channel Filter toggle fix): OutFinalColors starts as a copy of CommittedColors (never
	 * carried forward from any previous call's result) -- BEFORE any channel is composed. Each channel
	 * currently enabled in the Channel Filter is then recomputed from BaselineColors through the WHOLE
	 * layer stack -- ALWAYS starting from BaselineColors, never from CommittedColors or a prior result,
	 * which is what prevents that channel from ever accumulating ACROSS repeated recompositions. A
	 * channel NOT currently enabled is simply whatever CommittedColors already holds. OutNumComposed
	 * reports how many vertices had AT LEAST ONE layer contribute a value this call.
	 *
	 * AUDITED (peer-mask composition checkpoint): Layers is an UNORDERED set of every enabled+Ready mask
	 * generator for this component, resolved entirely by the caller (ApplyPreviewToEntry) BEFORE this
	 * call. AUDITED (M16-K.2): this function no longer sorts Layers by Mask->Source -- it resolves
	 * composition order ONCE, via ResolveLayersInPersistentOrder (the panel's own persistent
	 * GeneratorLayerOrder), before the per-vertex loop, then hands that same resolved view to
	 * VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential for every vertex.
	 */
	/**
	 * M16-K.2: resolves Layers (an unordered set of every enabled+Ready generator's own
	 * FVertexMaskForgeMaskLayerParams, built by the caller) into composition order, walking LayerOrder
	 * (the panel's own persistent generator layer order) and picking out each generator's own entry from
	 * Layers by identity (Mask->Source) -- replaces the legacy Sort()-by-Mask->Source used by both
	 * ComputeComposedColorsRGB/ComputeComposedColorsRGBSourceTopology before this checkpoint. A generator
	 * with no matching entry in Layers (disabled, or not Ready) is simply absent from the result -- same
	 * "not contributing" outcome the old Sort() already produced for it, just never present at all now
	 * rather than sorted-but-skipped downstream. Every field of a matched Layer (Mask/BlendMode/Opacity/
	 * IndexOverride) is copied verbatim -- no generator state, cache, or heavy payload is touched, only
	 * these small per-call descriptors.
	 *
	 * A non-generator entry (Fill/Constant override -- VertexMaskForgeLayerOrder::IsGeneratorLayer()==
	 * false for its own Mask->Source) never depends on LayerOrder at all -- it is preserved verbatim, in
	 * Layers' own original relative order, appended after any resolved generator layers. In every real
	 * call site today Layers contains EITHER exactly one Fill override OR up to seven generators, never
	 * both at once (see ApplyPreviewToEntry's own Fill/generator branch) -- this function does not assume
	 * that mutual exclusivity, it simply handles both shapes correctly either way.
	 *
	 * AUDITED (M16-K.2 defensive boundary): LayerOrder is expected to be VertexMaskForgeLayerOrder::
	 * IsValid() by construction -- the panel's own GeneratorLayerOrder member is initialized once via
	 * MakeDefault() and (until M16-K.3 introduces a mutator) never changes at all. If an invalid
	 * LayerOrder is ever passed here regardless (e.g. directly from a test), this function does not
	 * attempt a partial/best-effort reorder or silently repair it -- no generator layer is resolved at
	 * all for that call (an explicit, diagnosed, safe "empty" outcome, logged once via UE_LOG), while
	 * any non-generator (Fill) entry still passes through unaffected, since Fill never depended on
	 * LayerOrder in the first place.
	 */
	static TArray<FVertexMaskForgeMaskLayerParams, TInlineAllocator<8>> ResolveLayersInPersistentOrder(
		const TArrayView<const FVertexMaskForgeMaskLayerParams> Layers,
		const TConstArrayView<EVertexMaskForgeScalarMaskSource> LayerOrder)
	{
		TArray<FVertexMaskForgeMaskLayerParams, TInlineAllocator<8>> Resolved;

		if (VertexMaskForgeLayerOrder::IsValid(LayerOrder))
		{
			for (const EVertexMaskForgeScalarMaskSource Source : LayerOrder)
			{
				for (const FVertexMaskForgeMaskLayerParams& Layer : Layers)
				{
					if (Layer.Mask && Layer.Mask->Source == Source)
					{
						Resolved.Add(Layer);
						break;
					}
				}
			}
		}
		else
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("Vertex Mask Forge: ResolveLayersInPersistentOrder received an invalid LayerOrder (Num=%d) -- no generator layer will be composed this call."),
				LayerOrder.Num());
		}

		for (const FVertexMaskForgeMaskLayerParams& Layer : Layers)
		{
			const bool bIsGeneratorLayer = Layer.Mask && VertexMaskForgeLayerOrder::IsGeneratorLayer(Layer.Mask->Source);
			if (!bIsGeneratorLayer)
			{
				Resolved.Add(Layer);
			}
		}

		return Resolved;
	}

	// AUDITED (M16-J final): no longer `static` -- declared in VertexMaskForgeMaskTypes.h so automation
	// tests can call this exact production function directly (see that header's own doc comment on why).
	void ComputeComposedColorsRGB(
		TConstArrayView<FColor> BaselineColors,
		TConstArrayView<FColor> CommittedColors,
		TArrayView<const FVertexMaskForgeMaskLayerParams> Layers,
		const TConstArrayView<EVertexMaskForgeScalarMaskSource> LayerOrder,
		const bool bFilterR, const bool bFilterG, const bool bFilterB,
		TArray<FColor>& OutFinalColors,
		int32& OutNumComposed)
	{
		OutNumComposed = 0;

		// AUDITED (M16-K.2): resolved via the panel's own persistent LayerOrder -- no Sort()-by-
		// Mask->Source anymore. See ResolveLayersInPersistentOrder's own doc comment for the full contract.
		const TArray<FVertexMaskForgeMaskLayerParams, TInlineAllocator<8>> SortedLayers = ResolveLayersInPersistentOrder(Layers, LayerOrder);

		const int32 NumRenderVerts = BaselineColors.Num();

		OutFinalColors.SetNumUninitialized(NumRenderVerts);
		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			OutFinalColors[i] = CommittedColors[i];
		}

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const FColor& BaselineRenderColor = BaselineColors[i];

			// Alpha always tracks the baseline unconditionally -- also re-forced unconditionally by
			// FVertexMaskForgeWorkingStateOwner::ApplyComposedColorsRGB itself once this result is
			// handed over, so this assignment is defense-in-depth, not the sole guarantee.
			OutFinalColors[i].A = BaselineRenderColor.A;

			const FVector4f BaselineColorF(
				BaselineRenderColor.R / 255.f, BaselineRenderColor.G / 255.f,
				BaselineRenderColor.B / 255.f, BaselineRenderColor.A / 255.f);
			const FColor& CommittedRenderColor = CommittedColors[i];
			const FVector4f CommittedColorF(
				CommittedRenderColor.R / 255.f, CommittedRenderColor.G / 255.f,
				CommittedRenderColor.B / 255.f, CommittedRenderColor.A / 255.f);

			bool bAnyLayerContributed = false;
			// AUDITED (M16-J final): the panel's real composition call site -- replaces the legacy
			// ComposeMaskStack/ComposeStack (fixed-stage, Blend-Mode-grouped) with the strictly
			// sequential (array-order) fold, via VertexMaskForgeGeneratorLayerBridge. SortedLayers is
			// unchanged (still sorted once by Mask->Source outside this loop); the bridge threads each
			// generator's own resolved scalar through VertexMaskForgeSequentialEvaluator::EvaluateFillLayers
			// as an implicit-white Fill Layer -- see that module's own doc comment.
			const FVector4f Composite = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
				BaselineColorF, CommittedColorF, i, SortedLayers,
				bFilterR, bFilterG, bFilterB, bAnyLayerContributed);
			if (!bAnyLayerContributed)
			{
				continue;
			}
			++OutNumComposed;
			OutFinalColors[i] = ToDisplayFColor(Composite);
		}
	}

	/**
	 * M16-J.0B (2nd rejection-corrective pass): sibling of CaptureBaselineColorsRenderVertex for the
	 * Source-Topology corner domain -- same "compute what the bytes would be, never write anywhere"
	 * contract. Baseline color: read from Mesh's own Primary Color Overlay (this corner's own authored
	 * color), or white if the source has no color overlay at all. No per-instance OverrideVertexColors
	 * priority in this domain (Nanite's renderer never reads per-instance overrides).
	 */
	static TArray<FColor> CaptureBaselineColorsSourceTopology(const UE::Geometry::FDynamicMesh3& Mesh)
	{
		using namespace UE::Geometry;

		const FDynamicMeshColorOverlay* ColorOverlay = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryColors() : nullptr;
		const int32 NumCorners = Mesh.TriangleCount() * 3;

		TArray<FColor> Captured;
		Captured.SetNumUninitialized(NumCorners);
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
				Captured[SeedCornerIndex] = Color;
				++SeedCornerIndex;
			}
		}
		return Captured;
	}

	/**
	 * M16-J.0B (2nd rejection-corrective pass): sibling of ComputeComposedColorsRGB for the Source-
	 * Topology corner domain -- same "const input, fresh independent result, no owner storage touched"
	 * contract. The composition MATH ITSELF (IndexOverride wiring per Mask->Source, ComposeMaskStack
	 * call, per-corner Alpha-from-Baseline, "no layer contributed" fallback to Committed) is byte-for-
	 * byte UNCHANGED from before this checkpoint; only where the result is written changed. See
	 * ComputeComposedColorsRGB's own doc comment for the shared parts of this contract, and the
	 * per-generator IndexOverride domain notes originally on UpdateWorkingColorsSourceTopology (Bounding
	 * Box/Curvature/Noise -> Dynamic Mesh Vertex ID, Ambient Occlusion -> Normal Overlay Element ID,
	 * Material Slot/Directional Normal/Fill -> corner-exact).
	 */
	// AUDITED (M16-J final): no longer `static` -- see ComputeComposedColorsRGB's own comment above.
	void ComputeComposedColorsRGBSourceTopology(
		TConstArrayView<FColor> BaselineColors,
		TConstArrayView<FColor> CommittedColors,
		TArrayView<const FVertexMaskForgeMaskLayerParams> Layers,
		const TConstArrayView<EVertexMaskForgeScalarMaskSource> LayerOrder,
		const UE::Geometry::FDynamicMesh3& Mesh,
		const bool bFilterR, const bool bFilterG, const bool bFilterB,
		TArray<FColor>& OutFinalColors,
		int32& OutNumComposed)
	{
		using namespace UE::Geometry;

		OutNumComposed = 0;

		// AUDITED (M16-K.2): same LayerOrder-driven resolution as ComputeComposedColorsRGB's own -- see
		// ResolveLayersInPersistentOrder's own doc comment. Stays mutable here (unlike the render-vertex
		// sibling): the per-corner loop below rewrites each entry's own IndexOverride in place, every
		// corner, exactly as it already did before this checkpoint.
		TArray<FVertexMaskForgeMaskLayerParams, TInlineAllocator<8>> SortedLayers = ResolveLayersInPersistentOrder(Layers, LayerOrder);

		const FDynamicMeshNormalOverlay* NormalOverlay = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryNormals() : nullptr;

		const int32 NumCorners = BaselineColors.Num();
		OutFinalColors.SetNumUninitialized(NumCorners);
		for (int32 i = 0; i < NumCorners; ++i)
		{
			OutFinalColors[i] = CommittedColors[i];
		}

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
					case EVertexMaskForgeScalarMaskSource::Curvature:
						Layer.IndexOverride = VertTri[Corner];
						break;
					case EVertexMaskForgeScalarMaskSource::Noise:
						Layer.IndexOverride = VertTri[Corner];
						break;
					case EVertexMaskForgeScalarMaskSource::MaterialSlot:
						Layer.IndexOverride = CornerIndex;
						break;
					case EVertexMaskForgeScalarMaskSource::DirectionalNormal:
						Layer.IndexOverride = CornerIndex;
						break;
					default: // ConstantWhite / ConstantBlack (Fill) -- corner-domain mask.
						Layer.IndexOverride = CornerIndex;
						break;
					}
				}

				const FColor& BaselineRenderColor = BaselineColors[CornerIndex];
				OutFinalColors[CornerIndex].A = BaselineRenderColor.A;

				const FVector4f BaselineColorF(
					BaselineRenderColor.R / 255.f, BaselineRenderColor.G / 255.f,
					BaselineRenderColor.B / 255.f, BaselineRenderColor.A / 255.f);
				const FColor& CommittedRenderColor = CommittedColors[CornerIndex];
				const FVector4f CommittedColorF(
					CommittedRenderColor.R / 255.f, CommittedRenderColor.G / 255.f,
					CommittedRenderColor.B / 255.f, CommittedRenderColor.A / 255.f);

				bool bAnyLayerContributed = false;
				// AUDITED (M16-J final): same bridge/sequential-fold replacement as the render-vertex
				// branch above -- see ComputeComposedColorsRGB's own call site comment.
				const FVector4f Composite = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
					BaselineColorF, CommittedColorF, CornerIndex, SortedLayers,
					bFilterR, bFilterG, bFilterB, bAnyLayerContributed);
				if (!bAnyLayerContributed)
				{
					continue;
				}
				++OutNumComposed;
				OutFinalColors[CornerIndex] = ToDisplayFColor(Composite);
			}
		}
	}

	// DeriveDisplayColors moved to VertexMaskForgeDisplayColorDerivation.cpp (M3 extraction) -- see
	// ApplyPreviewToEntry's call site (VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors) for
	// the panel's own doc note on why this is called only for the render-vertex domain, never
	// source-topology, and never by Accept (BuildAcceptTargets persists WorkingColors verbatim).

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

		UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
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

		UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
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
		UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
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
	 * already has). WorkingColors here is the CALLER's own DeriveDisplayColors-reduced copy of
	 * SourceTopologyWorkingColors (see ApplyPreviewToEntry's own call site) -- Preview Mode display
	 * reduction (Red/Green/Blue/Alpha Channel) IS implemented for the Source-Topology preview, exactly
	 * mirroring the render-vertex path; the underlying SourceTopologyWorkingColors data Accept reads is
	 * unaffected either way, matching the render-vertex path's own "Preview Mode never affects Accept"
	 * guarantee.
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
		UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
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
		FVertexMaskForgeWorkingStateOwner& StateOwner,
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates)
	{
		FVertexMaskForgePreviewComponentState& State = StateOwner.GetVisualSessionStateMutable();
		RestorePreviewVisualOnly(State, ActorHideStates);

		// Step 6: the baseline snapshot, the last consolidated result, the transient working result,
		// and the AO geometry cache all belong to the session that just concluded for this component --
		// reset together, never independently. A brand new session always starts from a fresh capture
		// (this component's geometry/transform may have changed since, e.g. Accept just wrote new
		// colors, or the level was edited), never reusing a tree/raycast result computed for a
		// concluded operation.
		//
		// M16-J.0B: the six color arrays (render-vertex + Source-Topology corner-domain) and the
		// authenticated alpha authority now live inside StateOwner -- StateOwner.Reset() clears exactly
		// those six arrays and invalidates authority together (see that method's own doc comment), the
		// same "reset together, never independently" invariant this comment already documented before
		// this checkpoint, just now enforced by the owner itself rather than six manual .Reset() calls.
		StateOwner.Reset();

		// DIAGNOSTICS (raw/composition separation checkpoint): low-volume -- this function is only
		// ever called at genuine session-end points (Cancel, Accept, RefreshSelection, World cleanup),
		// never per-tick/per-recomposition, so Log level is safe here.
		if (State.AOCache.IsValid())
		{
			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO cache destroyed (session end/component teardown) for '%s'."),
				State.GetSourceComponent().IsValid() ? *State.GetSourceComponent()->GetName() : TEXT("<invalid component>"));
		}
		State.AOCache.Reset();

		if (State.SourceTopologyAOCache.IsValid())
		{
			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO (Source Topology) cache destroyed (session end/component teardown) for '%s'."),
				State.GetSourceComponent().IsValid() ? *State.GetSourceComponent()->GetName() : TEXT("<invalid component>"));
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
	// A stale, already-armed continuous-slider debounce must never apply after a discrete
	// change -- cancel it and regenerate immediately instead.
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnAxisInvertChanged(const int32 AxisIndex, const ECheckBoxState NewState)
{
	BoundingBoxAxisParams[AxisIndex].bInvert = (NewState == ECheckBoxState::Checked);

	// AUDITED (BBox Invert exception, follow-up audit -- see the header's own doc comment for the
	// full rationale): clears ONLY BoundingBoxMask (inlined, not via InvalidateBoundingBoxRawMask,
	// equivalent either way now that both are immediately followed by an unconditional regeneration),
	// then regenerates immediately. Ambient Occlusion is never touched (RunAutoUpdatePreview's
	// bIncludeAO=false).
	LastMaskActionStatusText = FText::GetEmpty();
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->GeneratorState.BoundingBoxMask = FVertexMaskForgeScalarMask();
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

// AUDITED (M16-K.4): shared dropdown-row generator for every Dynamic Layer's Fill combo -- generic over
// TSharedPtr<EVertexMaskForgeLayerFill> only, no per-row state, exactly mirroring OnGenerateBlendModeRow's
// own shape (which the seven existing generator BlendMode combos already all share as a single method).
TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateDynamicLayerFillRow(TSharedPtr<EVertexMaskForgeLayerFill> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetDynamicLayerFillLabel(*InOption) : FText::GetEmpty());
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
	// immediately, exactly like Preview Mode/Channel Filter -- no raw invalidation, no raycasts, no
	// risk of an original-color fallback for an otherwise-Ready mask.
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
	//     too -- just recompose (RecomposeWorkingColors()), reusing AOCache with zero raycasts. Only
	//     entries WITHOUT a valid derived mask (never generated, or invalidated by an actual AO raw
	//     parameter change while AO was off) need real (re)generation, always immediate
	//     (InvalidateAODerivedMask() is deliberately NOT called here -- it would needlessly clear
	//     already-Ready entries too; RunAutoUpdatePreview()'s own per-entry AO logic already leaves a
	//     Ready entry's snapshot untouched by construction).
	if (!bAOEnabled || bWasEnabled)
	{
		RecomposeWorkingColors();
		return;
	}

	bool bAnyEntryNeedsGeneration = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->GeneratorState.AmbientOcclusionMask.State != EVertexMaskForgeScalarMaskState::Ready)
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

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnAOInvertChanged(const ECheckBoxState NewState)
{
	bAOInvert = (NewState == ECheckBoxState::Checked);

	// AUDITED (raw/composition separation checkpoint, Invert fix): PURE composition -- RawValues in
	// AOCache are NEVER inverted; Invert is applied fresh, live, every recomposition (see
	// ApplyPreviewToEntry's own doc comment on the Invert live-override) directly from
	// FVertexMaskForgeAOCache::RawValues. Zero raycasts, zero Tree rebuild, works identically.
	RecomposeWorkingColors();
}

void SVertexMaskForgePanel::OnAOLevelsChanged()
{
	// AUDITED (AO Levels checkpoint): PURE composition, same contract as OnAOInvertChanged -- RawValues
	// in AOCache/SourceTopologyAOCache are NEVER touched; Levels is applied fresh, live, every
	// recomposition (see ApplyAOLevelsAndInvert and its two live-override call sites in
	// ApplyPreviewToEntry) directly from RawValues. Zero raycasts, zero Tree rebuild, zero
	// GeometryFingerprint invalidation, works identically regardless of domain (Render-Vertex/
	// Source-Topology).
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

void SVertexMaskForgePanel::OnCurvatureEnableChanged(const ECheckBoxState NewState)
{
	const bool bWasEnabled = bCurvatureEnabled;
	bCurvatureEnabled = (NewState == ECheckBoxState::Checked);

	// AUDITED (Curvature layer): same enable/disable contract as OnAOEnableChanged (see its own doc
	// comment) -- turning OFF is always pure composition; turning ON reuses an already-Ready entry
	// immediately (CurvatureMask.State == Ready already, e.g. from a previous session before Disable),
	// and always regenerates immediately if genuinely needed.
	if (!bCurvatureEnabled || bWasEnabled)
	{
		RecomposeWorkingColors();
		return;
	}

	bool bAnyEntryNeedsGeneration = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->GeneratorState.CurvatureMask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			bAnyEntryNeedsGeneration = true;
			break;
		}
	}

	if (!bAnyEntryNeedsGeneration)
	{
		RecomposeWorkingColors();
		return;
	}

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateCurvatureTypeRow(TSharedPtr<EVertexMaskForgeCurvatureType> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetCurvatureTypeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnCurvatureTypeSelectionChanged(TSharedPtr<EVertexMaskForgeCurvatureType> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	CurvatureType = *NewSelection;
	OnCurvatureParamChanged();
}

FText SVertexMaskForgePanel::GetCurvatureTypeButtonText() const
{
	return VertexMaskForgePanel::GetCurvatureTypeLabel(CurvatureType);
}

void SVertexMaskForgePanel::OnCurvatureInvertChanged(const ECheckBoxState NewState)
{
	bCurvatureInvert = (NewState == ECheckBoxState::Checked);

	// AUDITED (Curvature layer): PURE composition, same contract as OnAOInvertChanged -- the cached raw
	// Convex/Concave magnitudes are NEVER touched; Invert is applied fresh, live, every recomposition
	// (see ApplyCurvatureArtisticParams, applied LAST after Levels), via the shared
	// OnCurvatureParamChanged reprocess. Zero re-analysis, works identically regardless of Auto Update
	// Preview or domain (Render-Vertex/Source-Topology).
	OnCurvatureParamChanged();
}

void SVertexMaskForgePanel::OnCurvatureParamChanged()
{
	// AUDITED (Curvature layer): Type/Multiplier/Blur/Levels Min/Levels Max/Invert are ALL cheap,
	// purely downstream reprocessing of each entry's already-cached raw Convex/Concave magnitude arrays
	// (see FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache's own doc comment) -- never the
	// adjacency/dihedral-angle analysis, never GeometryFingerprint. So this is unconditional and
	// immediate, exactly like OnAOInvertChanged/OnAOLevelsChanged:
	// regenerate every entry's CurvatureMask directly from its raw cache, then recompose. An entry with
	// no cached raw curvature yet (Curvature never successfully generated for it) is left untouched
	// here -- Enable/live regeneration are what populate the cache in the first place; this
	// function only ever reprocesses what already exists.
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid() || !bCurvatureEnabled || Entry->GeneratorState.CurvatureRawConvexCache.IsEmpty())
		{
			continue;
		}

		const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();
		FVertexMaskForgeScalarMask NewCurvatureMask;
		if (Entry->bUseSourceTopology)
		{
			NewCurvatureMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
				WorkingMesh, Entry->GeneratorState, CurvatureType, CurvatureMultiplier, CurvatureBlur, CurvatureLevelsMin, CurvatureLevelsMax, bCurvatureInvert);
		}
		else
		{
			const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			const FStaticMeshRenderData* RenderData = IsValid(Mesh) ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				continue;
			}
			NewCurvatureMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMask(
				WorkingMesh, Entry->GeneratorState, Mesh->GetMeshDescription(0), RenderData->LODResources[0],
				CurvatureType, CurvatureMultiplier, CurvatureBlur, CurvatureLevelsMin, CurvatureLevelsMax, bCurvatureInvert);
		}

		if (NewCurvatureMask.State == EVertexMaskForgeScalarMaskState::Ready)
		{
			Entry->GeneratorState.CurvatureMask = MoveTemp(NewCurvatureMask);
		}
	}

	RecomposeWorkingColors();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateCurvatureBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetBlendModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnCurvatureBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	CurvatureBlendMode = *NewSelection;
	RecomposeWorkingColors();
}

FText SVertexMaskForgePanel::GetCurvatureBlendModeButtonText() const
{
	return VertexMaskForgePanel::GetBlendModeLabel(CurvatureBlendMode);
}

void SVertexMaskForgePanel::InvalidateNoiseRawMask()
{
	LastMaskActionStatusText = FText::GetEmpty();

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->GeneratorState.NoiseMask = FVertexMaskForgeScalarMask();
		}
	}

}

// --- Material Slot Mask (V2-D) -------------------------------------------------------------------

void SVertexMaskForgePanel::ReconcileMaterialSlotSelection()
{
	MaterialSlotOptions.Reset();

	if (IsMaterialSlotMaskAvailableForSelection())
	{
		const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry = SelectedMeshes[0];
		if (Entry.IsValid())
		{
			const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();
			for (const FVertexMaskForgeMaterialSlotInfo& Info : WorkingMesh.MaterialSlotOptions)
			{
				MaterialSlotOptions.Add(MakeShared<FVertexMaskForgeMaterialSlotInfo>(Info));
			}

			// Preserve the previous index if it still exists in the new list (e.g. an ordinary,
			// non-destructive re-selection of the SAME mesh); otherwise fall back to Slot 0 -- never
			// leaves a stale index that belonged to a DIFFERENT mesh's slot table silently selected
			// against this one.
			if (!WorkingMesh.MaterialSlotOptions.IsValidIndex(SelectedMaterialSlotIndex))
			{
				SelectedMaterialSlotIndex = 0;
			}
		}
	}
	// Zero or multiple selected meshes (or an invalid single entry): MaterialSlotOptions stays empty.
	// Deliberately does NOT touch bMaterialSlotMaskEnabled -- the checkbox state is preserved so
	// re-selecting a single mesh later resumes with the user's own choice, exactly like every other
	// generator's Enable state survives a selection change.

	// The OptionsSource pointer (&MaterialSlotOptions) never changes, only its contents -- RefreshOptions
	// (not a widget recreation) is the correct, Slate-native way to notify an already-constructed
	// SComboBox that its bound array was repopulated.
	if (MaterialSlotComboBox.IsValid())
	{
		MaterialSlotComboBox->RefreshOptions();
	}
}

FText SVertexMaskForgePanel::GetMaterialSlotMaskDiagnosticText() const
{
	if (!IsMaterialSlotMaskAvailableForSelection())
	{
		return LOCTEXT("MaterialSlotMaskRequiresSingleMesh", "Material Slot Mask currently requires a single selected mesh.");
	}
	const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry = SelectedMeshes[0];
	if (!Entry.IsValid())
	{
		return FText::GetEmpty();
	}
	const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();
	if (WorkingMesh.MaterialSlotOptions.IsEmpty())
	{
		return LOCTEXT("MaterialSlotMaskNoSlots", "The selected mesh has no usable Material Slots.");
	}
	if (!WorkingMesh.bMaterialSlotResolutionValid)
	{
		return LOCTEXT("MaterialSlotMaskResolutionInvalid", "Material Slot Mask unavailable: one or more Material Slots could not be resolved unambiguously (duplicate or missing slot names). Preview/Accept for this layer are blocked.");
	}
	if (WorkingMesh.bRenderVertexMaterialSlotAmbiguous && !Entry->bUseSourceTopology)
	{
		return LOCTEXT("MaterialSlotMaskRenderVertexAmbiguous", "Material Slot Mask unavailable: this mesh has render vertices shared between different Material Slots. Preview/Accept for this layer are blocked.");
	}
	return FText::GetEmpty();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateMaterialSlotRow(TSharedPtr<FVertexMaskForgeMaterialSlotInfo> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetMaterialSlotLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnMaterialSlotSelectionChanged(TSharedPtr<FVertexMaskForgeMaterialSlotInfo> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	SelectedMaterialSlotIndex = NewSelection->SlotIndex;
	OnMaterialSlotMaskGenerativeParamChanged();
}

FText SVertexMaskForgePanel::GetMaterialSlotButtonText() const
{
	for (const TSharedPtr<FVertexMaskForgeMaterialSlotInfo>& Option : MaterialSlotOptions)
	{
		if (Option.IsValid() && Option->SlotIndex == SelectedMaterialSlotIndex)
		{
			return VertexMaskForgePanel::GetMaterialSlotLabel(*Option);
		}
	}
	return FText::GetEmpty();
}

void SVertexMaskForgePanel::InvalidateMaterialSlotMaskRawMask()
{
	LastMaskActionStatusText = FText::GetEmpty();

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->GeneratorState.MaterialSlotMask = FVertexMaskForgeScalarMask();
		}
	}

}

void SVertexMaskForgePanel::OnMaterialSlotMaskGenerativeParamChanged()
{
	// AUDITED (V2-D): mirrors OnNoiseGenerativeParamChanged's exact pattern -- Slot selection/Invert
	// change WHAT the raw binary pattern looks like, so they invalidate every selected entry's
	// MaterialSlotMask and always regenerate immediately, cancelling any stale debounce first. Never
	// touches AO/Curvature/Noise/Alligator state or caches.
	InvalidateMaterialSlotMaskRawMask();
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnMaterialSlotMaskEnableChanged(const ECheckBoxState NewState)
{
	const bool bWasEnabled = bMaterialSlotMaskEnabled;
	bMaterialSlotMaskEnabled = (NewState == ECheckBoxState::Checked);

	// Same enable/disable contract as OnCurvatureEnableChanged/OnNoiseEnableChanged: turning OFF is
	// always pure composition; turning ON reuses an already-Ready entry immediately and always
	// regenerates immediately if genuinely needed.
	if (!bMaterialSlotMaskEnabled || bWasEnabled)
	{
		RecomposeWorkingColors();
		return;
	}

	bool bAnyEntryNeedsGeneration = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->GeneratorState.MaterialSlotMask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			bAnyEntryNeedsGeneration = true;
			break;
		}
	}

	if (!bAnyEntryNeedsGeneration || !IsMaterialSlotMaskAvailableForSelection())
	{
		RecomposeWorkingColors();
		return;
	}

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnMaterialSlotMaskInvertChanged(const ECheckBoxState NewState)
{
	bMaterialSlotMaskInvert = (NewState == ECheckBoxState::Checked);
	OnMaterialSlotMaskGenerativeParamChanged();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateMaterialSlotMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetBlendModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnMaterialSlotMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	MaterialSlotMaskBlendMode = *NewSelection;
	RecomposeWorkingColors();
}

FText SVertexMaskForgePanel::GetMaterialSlotMaskBlendModeButtonText() const
{
	return VertexMaskForgePanel::GetBlendModeLabel(MaterialSlotMaskBlendMode);
}

// --- Directional Normal Mask (V2-E) --------------------------------------------------------------

void SVertexMaskForgePanel::InvalidateDirectionalNormalMaskRawMask()
{
	LastMaskActionStatusText = FText::GetEmpty();

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->GeneratorState.DirectionalNormalMask = FVertexMaskForgeScalarMask();
			Entry->GeneratorState.bDirectionalNormalWorldSpaceConflict = false;
		}
	}

}

void SVertexMaskForgePanel::OnDirectionalNormalMaskGenerativeParamChanged()
{
	// AUDITED (V2-E): mirrors OnNoiseGenerativeParamChanged/OnMaterialSlotMaskGenerativeParamChanged's
	// exact pattern -- Space/Direction/Angle/Falloff/Invert change WHAT the raw angular pattern looks
	// like, so they invalidate every selected entry's DirectionalNormalMask and always regenerate
	// immediately, cancelling any stale debounce first. Never touches AO/Curvature/Noise/Material Slot
	// state or caches.
	InvalidateDirectionalNormalMaskRawMask();
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnDirectionalNormalMaskEnableChanged(const ECheckBoxState NewState)
{
	const bool bWasEnabled = bDirectionalNormalMaskEnabled;
	bDirectionalNormalMaskEnabled = (NewState == ECheckBoxState::Checked);

	// Same enable/disable contract as OnCurvatureEnableChanged/OnNoiseEnableChanged/
	// OnMaterialSlotMaskEnableChanged: turning OFF is always pure composition; turning ON reuses an
	// already-Ready entry immediately and always regenerates immediately if genuinely needed.
	if (!bDirectionalNormalMaskEnabled || bWasEnabled)
	{
		RecomposeWorkingColors();
		return;
	}

	bool bAnyEntryNeedsGeneration = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->GeneratorState.DirectionalNormalMask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			bAnyEntryNeedsGeneration = true;
			break;
		}
	}

	if (!bAnyEntryNeedsGeneration)
	{
		RecomposeWorkingColors();
		return;
	}

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateNormalSpaceRow(TSharedPtr<EVertexMaskForgeNormalSpace> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetNormalSpaceLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnNormalSpaceSelectionChanged(TSharedPtr<EVertexMaskForgeNormalSpace> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	DirectionalNormalSpace = *NewSelection;
	OnDirectionalNormalMaskGenerativeParamChanged();
}

FText SVertexMaskForgePanel::GetNormalSpaceButtonText() const
{
	return VertexMaskForgePanel::GetNormalSpaceLabel(DirectionalNormalSpace);
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateNormalDirectionRow(TSharedPtr<EVertexMaskForgeNormalDirection> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetNormalDirectionLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnNormalDirectionSelectionChanged(TSharedPtr<EVertexMaskForgeNormalDirection> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	DirectionalNormalDirection = *NewSelection;
	OnDirectionalNormalMaskGenerativeParamChanged();
}

FText SVertexMaskForgePanel::GetNormalDirectionButtonText() const
{
	return VertexMaskForgePanel::GetNormalDirectionLabel(DirectionalNormalDirection);
}

void SVertexMaskForgePanel::OnDirectionalNormalMaskInvertChanged(const ECheckBoxState NewState)
{
	bDirectionalNormalMaskInvert = (NewState == ECheckBoxState::Checked);
	OnDirectionalNormalMaskGenerativeParamChanged();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateDirectionalNormalMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetBlendModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnDirectionalNormalMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	DirectionalNormalMaskBlendMode = *NewSelection;
	RecomposeWorkingColors();
}

FText SVertexMaskForgePanel::GetDirectionalNormalMaskBlendModeButtonText() const
{
	return VertexMaskForgePanel::GetBlendModeLabel(DirectionalNormalMaskBlendMode);
}

FText SVertexMaskForgePanel::GetDirectionalNormalMaskDiagnosticText() const
{
	if (SelectedMeshes.IsEmpty())
	{
		return FText::GetEmpty();
	}
	bool bAnyConflict = false;
	bool bAnyInvalid = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		if (Entry->GeneratorState.bDirectionalNormalWorldSpaceConflict)
		{
			bAnyConflict = true;
		}
		if (Entry->GeneratorState.DirectionalNormalMask.State == EVertexMaskForgeScalarMaskState::Invalid)
		{
			bAnyInvalid = true;
		}
	}
	if (bAnyConflict)
	{
		return LOCTEXT("DirectionalNormalMaskWorldSpaceConflict",
			"World Space Directional Normal Mask cannot write conflicting results from differently transformed instances of the same Static Mesh asset. Preview still shows each instance correctly; Accept is blocked for the affected asset(s).");
	}
	if (bAnyInvalid)
	{
		return LOCTEXT("DirectionalNormalMaskDegenerateTransform",
			"Directional Normal Mask unavailable for one or more selected meshes: degenerate World Space transform (zero or near-zero scale on at least one axis).");
	}
	return FText::GetEmpty();
}

// --- Thickness Mask (V2-G) -------------------------------------------------------------------

void SVertexMaskForgePanel::InvalidateThicknessMaskRawMask()
{
	LastMaskActionStatusText = FText::GetEmpty();

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->GeneratorState.ThicknessMask = FVertexMaskForgeScalarMask();
			Entry->GeneratorState.ThicknessCache.Reset();
			Entry->GeneratorState.SourceTopologyThicknessCache.Reset();
		}
	}

}

void SVertexMaskForgePanel::OnThicknessRaycastParamChanged()
{
	// Search Distance/Bias change WHAT gets raycast (RayMaxDistance/EffectiveBias) -- invalidates the
	// cache's Layer 1+2 entirely (same contract as OnDirectionalNormalMaskGenerativeParamChanged), never
	// touches AO/Curvature/Noise/Directional Normal/Material Slot state or caches. Always regenerates
	// immediately, cancelling any stale debounce first.
	InvalidateThicknessMaskRawMask();
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnThicknessPostProcessParamChanged()
{
	// Min/Max Thickness/Blur only change post-processing of the ALREADY-CACHED raw distances (see
	// GenerateThicknessMask's own cache contract -- normalize/Blur/Invert are never cached, always
	// recomputed) -- so this never needs to clear ThicknessCache/SourceTopologyThicknessCache. Still
	// MUST go through a real regeneration pass (RunAutoUpdatePreview), never a plain RecomposeWorkingColors:
	// the normalize/Blur/Invert post-process step lives INSIDE GenerateThicknessMask/
	// GenerateThicknessMaskFromDynamicMesh, not in composition -- a plain recompose would re-blend the
	// STALE Values array and silently never reflect the new Min/Max/Blur/Invert. The raw distance cache
	// being untouched only means this regeneration is cheap (cache hit, no raycast), never that it can
	// be skipped.
	LastMaskActionStatusText = FText::GetEmpty();
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnThicknessMaskEnableChanged(const ECheckBoxState NewState)
{
	const bool bWasEnabled = bThicknessMaskEnabled;
	bThicknessMaskEnabled = (NewState == ECheckBoxState::Checked);

	// Same enable/disable contract as every other generator: turning OFF is always pure composition;
	// turning ON reuses an already-Ready entry immediately, always regenerates immediately if
	// genuinely needed.
	if (!bThicknessMaskEnabled || bWasEnabled)
	{
		RecomposeWorkingColors();
		return;
	}

	bool bAnyEntryNeedsGeneration = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->GeneratorState.ThicknessMask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			bAnyEntryNeedsGeneration = true;
			break;
		}
	}

	if (!bAnyEntryNeedsGeneration)
	{
		RecomposeWorkingColors();
		return;
	}

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnThicknessMaskInvertChanged(const ECheckBoxState NewState)
{
	bThicknessMaskInvert = (NewState == ECheckBoxState::Checked);
	OnThicknessPostProcessParamChanged();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateThicknessMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetBlendModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnThicknessMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	ThicknessMaskBlendMode = *NewSelection;
	RecomposeWorkingColors();
}

FText SVertexMaskForgePanel::GetThicknessMaskBlendModeButtonText() const
{
	return VertexMaskForgePanel::GetBlendModeLabel(ThicknessMaskBlendMode);
}

FText SVertexMaskForgePanel::GetThicknessMaskDiagnosticText() const
{
	if (SelectedMeshes.IsEmpty())
	{
		return FText::GetEmpty();
	}

	bool bAnyInvalid = false;
	bool bAnyStructurallyReadyButEmpty = false;
	int32 TotalNoHit = 0;
	int32 TotalDegenerateDiscarded = 0;
	int32 TotalInvalidOriginNormal = 0;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		if (Entry->GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Invalid
			|| Entry->GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Unavailable)
		{
			bAnyInvalid = true;
		}
		if (Entry->GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Ready
			&& Entry->GeneratorState.ThicknessMask.NumValidValues == 0)
		{
			bAnyStructurallyReadyButEmpty = true;
		}
		if (Entry->GeneratorState.ThicknessCache.IsValid())
		{
			TotalNoHit += Entry->GeneratorState.ThicknessCache->NumNoHit;
			TotalDegenerateDiscarded += Entry->GeneratorState.ThicknessCache->NumDegenerateTrianglesDiscarded;
			TotalInvalidOriginNormal += Entry->GeneratorState.ThicknessCache->NumInvalidOriginNormal;
		}
		if (Entry->GeneratorState.SourceTopologyThicknessCache.IsValid())
		{
			TotalNoHit += Entry->GeneratorState.SourceTopologyThicknessCache->NumNoHit;
			TotalDegenerateDiscarded += Entry->GeneratorState.SourceTopologyThicknessCache->NumDegenerateTrianglesDiscarded;
			TotalInvalidOriginNormal += Entry->GeneratorState.SourceTopologyThicknessCache->NumInvalidOriginNormal;
		}
	}

	if (bAnyInvalid)
	{
		return LOCTEXT("ThicknessMaskUnavailable",
			"Thickness Mask unavailable for one or more selected meshes: no usable geometry/normals, or Source-Topology mapping invalid.");
	}
	if (bAnyStructurallyReadyButEmpty)
	{
		return LOCTEXT("ThicknessMaskNoHit",
			"No opposite surface was found. The mesh may be open, or Search Distance may be too small.");
	}
	if (TotalNoHit > 0 || TotalInvalidOriginNormal > 0 || TotalDegenerateDiscarded > 0)
	{
		return FText::Format(
			LOCTEXT("ThicknessMaskPartialFormat", "Thickness Mask: {0} element(s) with no opposite surface, {1} with invalid origin normal, {2} degenerate triangle(s) discarded."),
			FText::AsNumber(TotalNoHit), FText::AsNumber(TotalInvalidOriginNormal), FText::AsNumber(TotalDegenerateDiscarded));
	}
	return FText::GetEmpty();
}

void SVertexMaskForgePanel::OnNoiseEnableChanged(const ECheckBoxState NewState)
{
	const bool bWasEnabled = bNoiseEnabled;
	bNoiseEnabled = (NewState == ECheckBoxState::Checked);

	// AUDITED (Noise V1): same enable/disable contract as OnCurvatureEnableChanged/OnAOEnableChanged
	// (see their own doc comments) -- turning OFF is always pure composition; turning ON reuses an
	// already-Ready entry immediately, and always regenerates immediately if genuinely needed.
	if (!bNoiseEnabled || bWasEnabled)
	{
		RecomposeWorkingColors();
		return;
	}

	bool bAnyEntryNeedsGeneration = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && Entry->GeneratorState.NoiseMask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			bAnyEntryNeedsGeneration = true;
			break;
		}
	}

	if (!bAnyEntryNeedsGeneration)
	{
		RecomposeWorkingColors();
		return;
	}

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateNoiseTypeRow(TSharedPtr<EVertexMaskForgeNoiseType> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetNoiseTypeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnNoiseTypeSelectionChanged(TSharedPtr<EVertexMaskForgeNoiseType> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	NoiseType = *NewSelection;
	OnNoiseGenerativeParamChanged();
}

FText SVertexMaskForgePanel::GetNoiseTypeButtonText() const
{
	return VertexMaskForgePanel::GetNoiseTypeLabel(NoiseType);
}

void SVertexMaskForgePanel::OnNoiseGenerativeParamChanged()
{
	// AUDITED (Noise V1): mirrors OnAxisParamChangedDiscrete's exact pattern -- Scale/Offset/Seed/
	// Octaves/Roughness/Lacunarity/Type change WHAT the raw pattern looks like, so they invalidate
	// every selected entry's NoiseMask (NoiseRawCache itself is left alone; EnsureNoiseRawCache's own
	// GeometryFingerprint+params comparison decides reuse lazily the next time Noise is actually
	// (re)generated) and always regenerate immediately, cancelling any stale debounce first.
	InvalidateNoiseRawMask();
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::OnNoiseScaleAxesLockChanged(const ECheckBoxState NewState)
{
	// UI/workflow-only flag -- flipping it never touches NoiseScaleX/Y/Z by itself, so OFF never
	// invalidates or Auto Updates (the numeric values genuinely have not changed).
	bNoiseScaleAxesLocked = (NewState == ECheckBoxState::Checked);

	if (bNoiseScaleAxesLocked)
	{
		// Snap Y/Z to the current X immediately, per the explicit "usar imediatamente o valor atual de
		// Scale X como mestre" requirement -- but only invalidate/Auto Update ONCE, and only if a value
		// actually changed (e.g. locking while already X==Y==Z must be a no-op beyond the flag itself).
		bool bAnyValueChanged = false;
		if (NoiseScaleY != NoiseScaleX) { NoiseScaleY = NoiseScaleX; bAnyValueChanged = true; }
		if (NoiseScaleZ != NoiseScaleX) { NoiseScaleZ = NoiseScaleX; bAnyValueChanged = true; }
		if (bAnyValueChanged)
		{
			OnNoiseGenerativeParamChanged();
		}
	}
}

void SVertexMaskForgePanel::OnNoiseScaleXChanged(const float NewValue)
{
	// Single atomic entry point for Scale X -- when locked, folds the Y/Z synchronization into the SAME
	// call so OnNoiseGenerativeParamChanged (one InvalidateNoiseRawMask + at most one Auto Update) fires
	// exactly once per edit, never once per axis.
	const float ClampedValue = FMath::Max(NewValue, 0.001f);
	bool bAnyValueChanged = false;
	if (NoiseScaleX != ClampedValue) { NoiseScaleX = ClampedValue; bAnyValueChanged = true; }
	if (bNoiseScaleAxesLocked)
	{
		if (NoiseScaleY != NoiseScaleX) { NoiseScaleY = NoiseScaleX; bAnyValueChanged = true; }
		if (NoiseScaleZ != NoiseScaleX) { NoiseScaleZ = NoiseScaleX; bAnyValueChanged = true; }
	}
	if (bAnyValueChanged)
	{
		OnNoiseGenerativeParamChanged();
	}
}

void SVertexMaskForgePanel::OnNoiseInvertChanged(const ECheckBoxState NewState)
{
	bNoiseInvert = (NewState == ECheckBoxState::Checked);
	OnNoiseArtisticParamChanged();
}

void SVertexMaskForgePanel::OnNoiseArtisticParamChanged()
{
	// AUDITED (Noise V1): Multiplier/Levels Min/Levels Max/Invert are ALL cheap, purely downstream
	// reprocessing of each entry's already-cached NoiseRawCache (see that field's own doc comment) --
	// never the per-vertex Perlin/FBM evaluation, never GeometryFingerprint, never the generative-params
	// comparison. So this is unconditional and immediate, exactly like OnCurvatureParamChanged:
	// regenerate every entry's NoiseMask directly from its raw cache,
	// then recompose. An entry with no cached raw pattern yet is left untouched here -- Enable/Generate
	// Mask/Auto Update are what populate the cache in the first place.
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid() || !bNoiseEnabled || Entry->GeneratorState.NoiseRawCache.IsEmpty())
		{
			continue;
		}

		const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();
		FVertexMaskForgeScalarMask NewNoiseMask;
		if (Entry->bUseSourceTopology)
		{
			NewNoiseMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
				WorkingMesh, Entry->GeneratorState, Entry->GeneratorState.NoiseCacheUsedParams,
				NoiseMultiplier, NoiseLevelsMin, NoiseLevelsMax, bNoiseInvert);
		}
		else
		{
			const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			const FStaticMeshRenderData* RenderData = IsValid(Mesh) ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				continue;
			}
			NewNoiseMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMask(
				WorkingMesh, Entry->GeneratorState, RenderData->LODResources[0],
				Entry->GeneratorState.NoiseCacheUsedParams,
				NoiseMultiplier, NoiseLevelsMin, NoiseLevelsMax, bNoiseInvert);
		}

		if (NewNoiseMask.State == EVertexMaskForgeScalarMaskState::Ready)
		{
			Entry->GeneratorState.NoiseMask = MoveTemp(NewNoiseMask);
		}
	}

	RecomposeWorkingColors();
}

TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateNoiseBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetBlendModeLabel(*InOption) : FText::GetEmpty());
}

void SVertexMaskForgePanel::OnNoiseBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	NoiseBlendMode = *NewSelection;
	RecomposeWorkingColors();
}

FText SVertexMaskForgePanel::GetNoiseBlendModeButtonText() const
{
	return VertexMaskForgePanel::GetBlendModeLabel(NoiseBlendMode);
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

	TArray<FText, TInlineAllocator<7>> ActiveLayerNames;
	if (bAnyAxisEnabled)
	{
		ActiveLayerNames.Add(LOCTEXT("ActiveLayerBBox", "Bounding Box"));
	}
	if (bAOEnabled)
	{
		ActiveLayerNames.Add(LOCTEXT("ActiveLayerAO", "Ambient Occlusion"));
	}
	if (bCurvatureEnabled)
	{
		ActiveLayerNames.Add(LOCTEXT("ActiveLayerCurvature", "Curvature"));
	}
	if (bDirectionalNormalMaskEnabled)
	{
		ActiveLayerNames.Add(LOCTEXT("ActiveLayerDirectionalNormal", "Directional Normal"));
	}
	if (bNoiseEnabled)
	{
		ActiveLayerNames.Add(LOCTEXT("ActiveLayerNoise", "Noise"));
	}
	if (bMaterialSlotMaskEnabled)
	{
		ActiveLayerNames.Add(LOCTEXT("ActiveLayerMaterialSlot", "Material Slot Mask"));
	}
	if (bThicknessMaskEnabled)
	{
		ActiveLayerNames.Add(LOCTEXT("ActiveLayerThickness", "Thickness"));
	}

	if (ActiveLayerNames.IsEmpty())
	{
		return LOCTEXT("ActiveMaskSourceNone", "Active layers: None -- enable a Bounding Box axis, Ambient Occlusion, Curvature, Directional Normal, Noise, Thickness, or Material Slot Mask");
	}

	TArray<FString> LayerStrings;
	LayerStrings.Reserve(ActiveLayerNames.Num());
	for (const FText& Name : ActiveLayerNames)
	{
		LayerStrings.Add(Name.ToString());
	}
	return FText::Format(
		LOCTEXT("ActiveMaskSourceListFormat", "Active layers: {0}"),
		FText::FromString(FString::Join(LayerStrings, TEXT(" + "))));
}

void SVertexMaskForgePanel::RebuildGeneratorLayerList()
{
	if (!GeneratorLayerListContainer.IsValid())
	{
		return;
	}

	// AUDITED (M16-K.3): view refresh only -- clears and re-adds ROW WIDGETS, never touches
	// GeneratorLayerOrder itself (read here, never written). Walking GeneratorLayerOrder directly (not
	// any cached/parallel copy) is what guarantees the visible list can never drift from the one real
	// order the production composition path also reads.
	GeneratorLayerListContainer->ClearChildren();
	for (const EVertexMaskForgeScalarMaskSource Source : GeneratorLayerOrder)
	{
		GeneratorLayerListContainer->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.f, 1.f))
			[
				BuildGeneratorLayerRow(Source)
			];
	}
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildGeneratorLayerRow(const EVertexMaskForgeScalarMaskSource Source)
{
	// AUDITED (M16-K.3): this row shows identity + reorder controls ONLY -- Enabled/Blend Mode/Opacity/
	// Invert/generator-specific parameters remain exclusively in this generator's own existing section
	// further down the panel (unchanged by this checkpoint). Source is captured by value in both
	// lambdas below; CanMoveGeneratorLayerUp/Down and OnMoveGeneratorLayerUp/Down each resolve Source's
	// CURRENT position fresh, every call -- never a position/index captured once here at row-build time,
	// which would go stale the moment any reorder happens.
	return SNew(SHorizontalBox)

	+ SHorizontalBox::Slot()
	.FillWidth(1.f)
	.VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(VertexMaskForgePanel::GetGeneratorLayerDisplayName(Source))
	]

	+ SHorizontalBox::Slot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
	[
		SNew(SButton)
		.ContentPadding(FMargin(8.f, 1.f))
		.ToolTipText(LOCTEXT("MoveLayerUpTooltip", "Move this layer one position earlier in the composition order."))
		.Text(LOCTEXT("MoveLayerUp", "Up"))
		.IsEnabled_Lambda([this, Source]() { return CanMoveGeneratorLayerUp(Source); })
		.OnClicked(FOnClicked::CreateLambda([this, Source]() { return OnMoveGeneratorLayerUp(Source); }))
	]

	+ SHorizontalBox::Slot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(FMargin(2.f, 0.f, 0.f, 0.f))
	[
		SNew(SButton)
		.ContentPadding(FMargin(8.f, 1.f))
		.ToolTipText(LOCTEXT("MoveLayerDownTooltip", "Move this layer one position later in the composition order."))
		.Text(LOCTEXT("MoveLayerDown", "Down"))
		.IsEnabled_Lambda([this, Source]() { return CanMoveGeneratorLayerDown(Source); })
		.OnClicked(FOnClicked::CreateLambda([this, Source]() { return OnMoveGeneratorLayerDown(Source); }))
	];
}

bool SVertexMaskForgePanel::CanMoveGeneratorLayerUp(const EVertexMaskForgeScalarMaskSource Source) const
{
	if (!VertexMaskForgeLayerOrder::IsGeneratorLayer(Source))
	{
		return false;
	}
	const int32 CurrentIndex = GeneratorLayerOrder.IndexOfByKey(Source);
	return CurrentIndex != INDEX_NONE && CurrentIndex > 0;
}

bool SVertexMaskForgePanel::CanMoveGeneratorLayerDown(const EVertexMaskForgeScalarMaskSource Source) const
{
	if (!VertexMaskForgeLayerOrder::IsGeneratorLayer(Source))
	{
		return false;
	}
	const int32 CurrentIndex = GeneratorLayerOrder.IndexOfByKey(Source);
	return CurrentIndex != INDEX_NONE && CurrentIndex < GeneratorLayerOrder.Num() - 1;
}

FReply SVertexMaskForgePanel::OnMoveGeneratorLayerUp(const EVertexMaskForgeScalarMaskSource Source)
{
	// AUDITED (M16-K.3): the K.1 domain (VertexMaskForgeLayerOrder::MoveUp) is authoritative for the
	// actual movement -- no Swap/RemoveAt/Insert/Sort/enum arithmetic here. On a boundary/invalid no-op
	// (returns false), GeneratorLayerOrder is guaranteed untouched by MoveUp's own contract, so this
	// function does nothing further -- no rebuild, no recomposition -- matching a click that had no
	// real effect.
	if (!VertexMaskForgeLayerOrder::MoveUp(GeneratorLayerOrder, Source))
	{
		return FReply::Handled();
	}

	// AUDITED (M16-K.3): exactly one visual rebuild and one recomposition request per successful move --
	// RecomposeWorkingColors() is pure composition (UpdateAllPreviews(false)), never invalidates a raw
	// mask, never rebuilds Working Mesh, never calls RefreshSelection.
	RebuildGeneratorLayerList();
	RecomposeWorkingColors();
	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnMoveGeneratorLayerDown(const EVertexMaskForgeScalarMaskSource Source)
{
	if (!VertexMaskForgeLayerOrder::MoveDown(GeneratorLayerOrder, Source))
	{
		return FReply::Handled();
	}

	RebuildGeneratorLayerList();
	RecomposeWorkingColors();
	return FReply::Handled();
}

// ==================================================================================================
// M16-K.4: Dynamic Layers UI Prototype -- domain-only, deliberately disconnected from composition/
// preview. Every function below reads/writes ONLY DynamicLayerStack (via its own controlled, GUID-based
// mutation API); none of them call RecomposeWorkingColors, UpdateAllPreviews, invalidate any generator
// mask/cache, touch GeneratorLayerOrder, or call VertexMaskForgeDynamicLayerEvaluator. Structural changes
// (Add/Remove/Move) rebuild the row list; property changes (Enabled/Fill/BlendMode/Opacity/Channel
// Filter/Rename) mutate the stack only -- each control's own displayed value is bound live to the stack
// via a _Lambda accessor, so no rebuild is needed and no widget holds a duplicate copy of the data.
// ==================================================================================================

void SVertexMaskForgePanel::RebuildDynamicLayersList()
{
	if (!DynamicLayersListContainer.IsValid())
	{
		return;
	}

	// View refresh only -- clears and re-adds ROW WIDGETS, never touches DynamicLayerStack itself (read
	// here, never written). Walking DynamicLayerStack.GetLayers() directly (never a cached/parallel copy)
	// is what guarantees the visible list can never drift from the stack's own real order.
	DynamicLayersListContainer->ClearChildren();

	if (DynamicLayerStack.IsEmpty())
	{
		DynamicLayersListContainer->AddSlot()
			.AutoHeight()
			.Padding(FMargin(2.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NoDynamicLayers", "No dynamic layers"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		return;
	}

	for (const FVertexMaskForgeLayer& Layer : DynamicLayerStack.GetLayers())
	{
		// AUDITED: LayerId captured BY VALUE into BuildDynamicLayerRow's own row-building lambdas below --
		// never a pointer/reference into this Layer (which may be relocated or destroyed by any later
		// Add/Remove/Move, all of which may reallocate DynamicLayerStack's internal TArray).
		DynamicLayersListContainer->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f))
			[
				BuildDynamicLayerRow(Layer.LayerId)
			];
	}
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicLayerRow(const FGuid LayerId)
{
	// AUDITED: every accessor below resolves LayerId fresh via DynamicLayerStack.FindLayerById on every
	// call -- never a captured pointer/reference/index. A defensive nullptr guard is used throughout even
	// though a row is only ever built for a LayerId RebuildDynamicLayersList just confirmed present --
	// this is what makes a hypothetical late-firing callback for an already-removed layer safe (the
	// stack's own SetLayer*/RenameLayer already return false/no-op for an unknown id; these UI-side
	// guards additionally prevent dereferencing a null Find result when reading, not just when writing).
	return SNew(SBorder)
	.Padding(FMargin(4.f))
	// AUDITED (M16-K.4B root-cause fix): SBorder's DEFAULT BorderImage (used when none is specified,
	// which is what M16-K.4A shipped) is FCoreStyle's "Border" brush -- FSlateColorBrush(FStyleColors::
	// Panel), a brush whose OWN internal tint is already the dark panel color. BorderBackgroundColor is
	// MULTIPLIED against that brush's own tint at paint time, so (1,0,0,0.15) against an already
	// near-black Panel color zeroes the G/B channels of an already-dark value and barely nudges R --
	// visually indistinguishable from the untinted row (exactly the reported "row stayed gray" bug).
	// Fix: use "WhiteBrush" (FSlateColorBrush(FLinearColor::White), i.e. TintColor == pure white) as the
	// BorderImage instead, so BorderBackgroundColor's own RGBA is what actually renders (White * Color ==
	// Color), un-crushed. The neutral/default case (see GetDynamicLayerChannelTint) now explicitly
	// returns FStyleColors::Panel at full alpha -- White * Panel == Panel, reproducing the EXACT original
	// "Border" brush appearance byte-for-byte, so a multi-channel/no-channel row is visually identical to
	// the pre-K.4A baseline, never solid white.
	.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
	// AUDITED (M16-K.4A): channel-aware tint -- BorderBackgroundColor is a plain SLATE_ATTRIBUTE, so this
	// TAttribute re-evaluates GetDynamicLayerChannelTint(LayerId) fresh every paint; no explicit
	// invalidation/rebuild is needed when a channel checkbox changes (the checkbox's own
	// OnCheckStateChanged_Lambda mutates DynamicLayerStack only -- see GetDynamicLayerChannelTint's own
	// doc comment for why this is presentation-only, never domain data).
	.BorderBackgroundColor_Lambda([this, LayerId]() { return GetDynamicLayerChannelTint(LayerId); })
	[
		SNew(SVerticalBox)

		// --- Structural row: Enabled, Name, Up, Down, Remove -------------------------------------
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
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("DynamicLayerEnabledTooltip", "Whether this layer currently contributes to composition (prototype -- not yet evaluated)."))
				.IsChecked_Lambda([this, LayerId]()
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					return (Layer && Layer->bEnabled) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, LayerId](const ECheckBoxState NewState)
				{
					DynamicLayerStack.SetLayerEnabled(LayerId, NewState == ECheckBoxState::Checked);
				})
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				SNew(SEditableTextBox)
				.ToolTipText(LOCTEXT("DynamicLayerNameTooltip", "Layer name. Duplicate names are allowed -- identity is internal, not the name."))
				.Text_Lambda([this, LayerId]()
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					return Layer ? FText::FromString(Layer->Name) : FText::GetEmpty();
				})
				.OnTextCommitted(FOnTextCommitted::CreateLambda([this, LayerId](const FText& NewText, ETextCommit::Type)
				{
					// AUDITED: RenameLayer has no non-empty/uniqueness policy -- whatever the domain
					// accepts or rejects, the UI never invents its own divergent validation. On rejection
					// (only possible for an unknown LayerId), the widget's own Text_Lambda above already
					// re-reads the stack on next paint, so it naturally reverts to the source of truth --
					// no separate revert logic is needed here.
					DynamicLayerStack.RenameLayer(LayerId, NewText.ToString());
				}))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 2.f, 0.f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.f, 1.f))
				.ToolTipText(LOCTEXT("MoveDynamicLayerUpTooltip", "Move this layer one position earlier."))
				.Text(LOCTEXT("MoveLayerUp", "Up"))
				.IsEnabled_Lambda([this, LayerId]() { return CanMoveDynamicLayerUp(LayerId); })
				.OnClicked(FOnClicked::CreateLambda([this, LayerId]() { return OnMoveDynamicLayerUpClicked(LayerId); }))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 2.f, 0.f))
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.f, 1.f))
				.ToolTipText(LOCTEXT("MoveDynamicLayerDownTooltip", "Move this layer one position later."))
				.Text(LOCTEXT("MoveLayerDown", "Down"))
				.IsEnabled_Lambda([this, LayerId]() { return CanMoveDynamicLayerDown(LayerId); })
				.OnClicked(FOnClicked::CreateLambda([this, LayerId]() { return OnMoveDynamicLayerDownClicked(LayerId); }))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.f, 1.f))
				.ToolTipText(LOCTEXT("RemoveDynamicLayerTooltip", "Remove this layer."))
				.Text(LOCTEXT("RemoveDynamicLayerButton", "Remove"))
				.OnClicked(FOnClicked::CreateLambda([this, LayerId]() { return OnRemoveDynamicLayerClicked(LayerId); }))
			]
		]

		// --- Properties row: Fill, Blend Mode, Opacity, R, G, B ----------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				SNew(SComboBox<TSharedPtr<EVertexMaskForgeLayerFill>>)
				.OptionsSource(&DynamicLayerFillOptions)
				.InitiallySelectedItem(DynamicLayerFillOptions.IsValidIndex(0) ? DynamicLayerFillOptions[0] : nullptr)
				.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateDynamicLayerFillRow)
				.OnSelectionChanged(SComboBox<TSharedPtr<EVertexMaskForgeLayerFill>>::FOnSelectionChanged::CreateLambda(
					[this, LayerId](TSharedPtr<EVertexMaskForgeLayerFill> NewSelection, ESelectInfo::Type)
					{
						if (NewSelection.IsValid())
						{
							DynamicLayerStack.SetLayerFill(LayerId, *NewSelection);
						}
					}))
				[
					SNew(STextBlock)
					.Text_Lambda([this, LayerId]()
					{
						const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
						return Layer ? VertexMaskForgePanel::GetDynamicLayerFillLabel(Layer->Fill) : FText::GetEmpty();
					})
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				SNew(SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
				.OptionsSource(&BlendModeOptions)
				.InitiallySelectedItem(BlendModeOptions.IsValidIndex(0) ? BlendModeOptions[0] : nullptr)
				.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateBlendModeRow)
				.OnSelectionChanged(SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>::FOnSelectionChanged::CreateLambda(
					[this, LayerId](TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type)
					{
						if (NewSelection.IsValid())
						{
							DynamicLayerStack.SetLayerBlendMode(LayerId, *NewSelection);
						}
					}))
				[
					SNew(STextBlock)
					.Text_Lambda([this, LayerId]()
					{
						const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
						return Layer ? VertexMaskForgePanel::GetBlendModeLabel(Layer->BlendMode) : FText::GetEmpty();
					})
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				SNew(SSpinBox<float>)
				.MinDesiredWidth(52.f)
				.MinValue(0.0f)
				.MaxValue(1.0f)
				.Delta(0.01f)
				.MinFractionalDigits(2)
				.MaxFractionalDigits(2)
				.ToolTipText(LOCTEXT("DynamicLayerOpacityTooltip", "Opacity, [0,1]."))
				.Value_Lambda([this, LayerId]()
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					return Layer ? Layer->Opacity : 1.0f;
				})
				.OnValueChanged_Lambda([this, LayerId](const float NewValue)
				{
					// AUDITED: SSpinBox's own Min/MaxValue already constrain NewValue to [0,1]; no manual
					// clamp is applied here -- SetLayerOpacity performs its own finite+range validation
					// and rejects (no-op) anything it would not accept, never silently normalizing.
					DynamicLayerStack.SetLayerOpacity(LayerId, NewValue);
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 2.f, 0.f))
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("DynamicLayerAffectRedTooltip", "Affect Red Channel"))
				.IsChecked_Lambda([this, LayerId]()
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					return (Layer && Layer->bAffectRed) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, LayerId](const ECheckBoxState NewState)
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					if (!Layer)
					{
						return;
					}
					DynamicLayerStack.SetLayerChannelFilter(LayerId, NewState == ECheckBoxState::Checked, Layer->bAffectGreen, Layer->bAffectBlue);
				})
				.Content()
				[
					SNew(STextBlock).Text(LOCTEXT("DynamicLayerAffectRedLabel", "R"))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 2.f, 0.f))
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("DynamicLayerAffectGreenTooltip", "Affect Green Channel"))
				.IsChecked_Lambda([this, LayerId]()
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					return (Layer && Layer->bAffectGreen) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, LayerId](const ECheckBoxState NewState)
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					if (!Layer)
					{
						return;
					}
					DynamicLayerStack.SetLayerChannelFilter(LayerId, Layer->bAffectRed, NewState == ECheckBoxState::Checked, Layer->bAffectBlue);
				})
				.Content()
				[
					SNew(STextBlock).Text(LOCTEXT("DynamicLayerAffectGreenLabel", "G"))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("DynamicLayerAffectBlueTooltip", "Affect Blue Channel"))
				.IsChecked_Lambda([this, LayerId]()
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					return (Layer && Layer->bAffectBlue) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, LayerId](const ECheckBoxState NewState)
				{
					const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
					if (!Layer)
					{
						return;
					}
					DynamicLayerStack.SetLayerChannelFilter(LayerId, Layer->bAffectRed, Layer->bAffectGreen, NewState == ECheckBoxState::Checked);
				})
				.Content()
				[
					SNew(STextBlock).Text(LOCTEXT("DynamicLayerAffectBlueLabel", "B"))
				]
			]
		]
	];
}

bool SVertexMaskForgePanel::CanMoveDynamicLayerUp(const FGuid LayerId) const
{
	const int32 CurrentIndex = DynamicLayerStack.FindLayerIndexById(LayerId);
	return CurrentIndex != INDEX_NONE && CurrentIndex > 0;
}

bool SVertexMaskForgePanel::CanMoveDynamicLayerDown(const FGuid LayerId) const
{
	const int32 CurrentIndex = DynamicLayerStack.FindLayerIndexById(LayerId);
	return CurrentIndex != INDEX_NONE && CurrentIndex < DynamicLayerStack.Num() - 1;
}

FReply SVertexMaskForgePanel::OnMoveDynamicLayerUpClicked(const FGuid LayerId)
{
	// AUDITED: DynamicLayerStack::MoveLayerUp is authoritative for the actual movement -- no Swap/
	// RemoveAt/Insert here. On a boundary/invalid no-op (returns false), the stack is guaranteed untouched
	// by MoveLayerUp's own contract, so this function does nothing further -- no rebuild -- matching a
	// click that had no real effect. On success: exactly one rebuild, no production call whatsoever.
	if (!DynamicLayerStack.MoveLayerUp(LayerId))
	{
		return FReply::Handled();
	}

	RebuildDynamicLayersList();
	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnMoveDynamicLayerDownClicked(const FGuid LayerId)
{
	if (!DynamicLayerStack.MoveLayerDown(LayerId))
	{
		return FReply::Handled();
	}

	RebuildDynamicLayersList();
	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnAddDynamicLayerClicked()
{
	// AUDITED: display name only -- "Layer N" from the CURRENT count, not a persistent counter. Not
	// guaranteed unique (e.g. removing "Layer 2" then adding again can produce another "Layer 2") --
	// deliberately fine, since identity is LayerId, never Name (see DuplicateNamesDoNotConfuseRows).
	const FString NewLayerName = FString::Printf(TEXT("Layer %d"), DynamicLayerStack.Num() + 1);
	DynamicLayerStack.AddLayer(NewLayerName);

	RebuildDynamicLayersList();
	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnRemoveDynamicLayerClicked(const FGuid LayerId)
{
	// AUDITED: RemoveLayer is a safe no-op for an unknown LayerId (already-removed/stale callback) --
	// nothing else needs to guard against that case here. Removing the stack's only/last layer is
	// explicitly supported and safe (IsEmpty() stack is a valid stack) -- RebuildDynamicLayersList's own
	// empty-state branch handles the resulting empty list.
	DynamicLayerStack.RemoveLayer(LayerId);

	RebuildDynamicLayersList();
	return FReply::Handled();
}

FSlateColor SVertexMaskForgePanel::GetDynamicLayerChannelTint(const FGuid LayerId) const
{
	// AUDITED (M16-K.4B root-cause fix): default appearance is FStyleColors::Panel at full alpha -- NOT
	// FLinearColor::White. BuildDynamicLayerRow's SBorder now uses "WhiteBrush" (pure white TintColor) as
	// its BorderImage, so BorderBackgroundColor's own RGBA renders directly (White * Color == Color).
	// Returning White here would multiply to opaque White * White == solid WHITE, replacing the row's
	// entire background -- wrong. Returning FStyleColors::Panel instead reproduces White * Panel == Panel,
	// which is exactly the color SBorder's OLD default "Border" brush (FSlateColorBrush(FStyleColors::
	// Panel)) rendered before M16-K.4A -- i.e. byte-for-byte the pre-tint baseline appearance, for an
	// unknown/removed LayerId, zero active channels, or two-or-more active channels. The actual channel-
	// exclusivity decision is delegated to ResolveDynamicLayerChannelTint (VertexMaskForgeLayerTypes.h) --
	// a pure, Slate-free function of the three channel bools only, kept directly unit-testable without
	// constructing this (or any) Slate widget. This function's only job is resolving LayerId to a layer
	// and mapping that pure decision to an actual presentation color/alpha.
	const FSlateColor DefaultAppearance = FStyleColors::Panel;

	const FVertexMaskForgeLayer* Layer = DynamicLayerStack.FindLayerById(LayerId);
	if (!Layer)
	{
		return DefaultAppearance;
	}

	// Discreet identification, not a highlight -- low alpha over the panel's existing dark background.
	// Diagnostic starting point per this checkpoint's own instruction (0.15); raise moderately (~0.18-0.30)
	// only after manual visual confirmation that the WhiteBrush fix above is proven correct.
	constexpr float TintAlpha = 0.15f;
	const EVertexMaskForgeDynamicLayerChannelTint Tint = ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue);
	if (Tint == EVertexMaskForgeDynamicLayerChannelTint::Default)
	{
		return DefaultAppearance;
	}
	return FSlateColor(GetDynamicLayerChannelTintColor(Tint, TintAlpha));
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

	// AUDITED (V2-E corrective pass): see ActorMovedDelegateHandle's own doc comment for the engine-
	// source evidence that this fires once per completed viewport gizmo move/rotate/scale.
	if (GEngine)
	{
		ActorMovedDelegateHandle = GEngine->OnActorMoved().AddRaw(this, &SVertexMaskForgePanel::OnActorMovedForDirectionalNormal);
	}

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

	// M16-K.4: Dynamic Layers' own Fill options -- None/Black/White, the only three real
	// EVertexMaskForgeLayerFill enumerators. Shared read-only across every Dynamic Layer row's Fill combo,
	// same pattern as BlendModeOptions above.
	DynamicLayerFillOptions.Add(MakeShared<EVertexMaskForgeLayerFill>(EVertexMaskForgeLayerFill::None));
	DynamicLayerFillOptions.Add(MakeShared<EVertexMaskForgeLayerFill>(EVertexMaskForgeLayerFill::Black));
	DynamicLayerFillOptions.Add(MakeShared<EVertexMaskForgeLayerFill>(EVertexMaskForgeLayerFill::White));

	CurvatureTypeOptions.Add(MakeShared<EVertexMaskForgeCurvatureType>(EVertexMaskForgeCurvatureType::Convex));
	CurvatureTypeOptions.Add(MakeShared<EVertexMaskForgeCurvatureType>(EVertexMaskForgeCurvatureType::Concave));
	CurvatureTypeOptions.Add(MakeShared<EVertexMaskForgeCurvatureType>(EVertexMaskForgeCurvatureType::Both));

	NormalSpaceOptions.Add(MakeShared<EVertexMaskForgeNormalSpace>(EVertexMaskForgeNormalSpace::Local));
	NormalSpaceOptions.Add(MakeShared<EVertexMaskForgeNormalSpace>(EVertexMaskForgeNormalSpace::World));

	NormalDirectionOptions.Add(MakeShared<EVertexMaskForgeNormalDirection>(EVertexMaskForgeNormalDirection::PositiveX));
	NormalDirectionOptions.Add(MakeShared<EVertexMaskForgeNormalDirection>(EVertexMaskForgeNormalDirection::NegativeX));
	NormalDirectionOptions.Add(MakeShared<EVertexMaskForgeNormalDirection>(EVertexMaskForgeNormalDirection::PositiveY));
	NormalDirectionOptions.Add(MakeShared<EVertexMaskForgeNormalDirection>(EVertexMaskForgeNormalDirection::NegativeY));
	NormalDirectionOptions.Add(MakeShared<EVertexMaskForgeNormalDirection>(EVertexMaskForgeNormalDirection::PositiveZ));
	NormalDirectionOptions.Add(MakeShared<EVertexMaskForgeNormalDirection>(EVertexMaskForgeNormalDirection::NegativeZ));

	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::Perlin));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::FractalPerlin));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::Billow));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::Ridged));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::Turbulence));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::WorleyF1));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::WorleyF2MinusF1));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::Voronoi));
	NoiseTypeOptions.Add(MakeShared<EVertexMaskForgeNoiseType>(EVertexMaskForgeNoiseType::Alligator));

	// AUDITED (pre-modularization UI/defaults pass): every axis (X/Y/Z) starts disabled -- see
	// FVertexMaskForgeAxisMaskParams' own default member initializers in the header (bEnabled = false).
	// No axis is force-enabled here anymore; a fresh session never generates a mask automatically.

	// AUDITED (vertical scroll checkpoint): SBorder's single child previously received the full
	// available area from ChildSlot and simply handed it straight to the root SVerticalBox -- with
	// every one of that VerticalBox's ~20 top-level slots AutoHeight, the box's DESIRED height is the
	// sum of all of them, but nothing in this chain ever CLIPPED-WITH-SCROLL when the Editor's dock tab
	// allotted LESS actual height than that sum (a fixed-size dock tab does not grow to fit; it simply
	// gives this widget its own client area and lets Slate's normal arrange/clip behavior take over) --
	// so once four expandable panels were open at once, the tail of the vertical stack (Fill White/
	// Fill Black, Channel Filter, Preview status, Accept/Cancel) rendered past the
	// bottom edge with no way to reach it. SScrollBox is inserted as SBorder's sole child, in the SAME
	// position the root SVerticalBox previously occupied -- it inherits the SAME bounded area SBorder
	// already receives from ChildSlot (no explicit FillHeight plumbing needed: SBorder's own single-
	// child slot and SCompoundWidget's ChildSlot both already give their one child the full allotted
	// area, they just never used to have anything that would CLIP+SCROLL that area against oversized
	// content) and provides exactly that missing clip-and-scroll behavior, with a visible vertical
	// scrollbar precisely when content exceeds the available height. The entire pre-existing content
	// (title through Accept/Cancel/status text) moves inside the ScrollBox's one slot, UNCHANGED and in
	// the SAME order -- Accept/Cancel are ordinary AutoHeight rows in the same vertical flow as
	// everything else here (this panel has no separately-docked toolbar/footer), so per the "if the
	// buttons are naturally part of the vertical content, keep them inside the scroll so they remain
	// reachable" instruction, they scroll with everything else rather than being carved out into an
	// artificial fixed footer that this panel's existing layout was never designed around.
	ChildSlot
	[
		SNew(SBorder)
		.Padding(FMargin(12.f))
		[
			SNew(SScrollBox)
			.Orientation(Orient_Vertical)
			+ SScrollBox::Slot()
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

			// AUDITED (M16-K.3): the first visual representation of the generator layer composition
			// order -- a minimal, non-expandable list (identity + Move Up/Move Down only, see
			// BuildGeneratorLayerRow's own doc comment) placed BEFORE the seven individual generator
			// sections below, since it reports/controls the ORDER those sections compose in. Rows are
			// (re)built by RebuildGeneratorLayerList() directly from GeneratorLayerOrder -- this
			// SVerticalBox::Slot() only ever assigns the empty container once; it is never itself
			// re-entered by Construct().
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SNew(SBorder)
				.Padding(FMargin(8.f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("GeneratorLayersSectionTitle", "Layers"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(GeneratorLayerListContainer, SVerticalBox)
					]
				]
			]

			// AUDITED (M16-K.4): "Dynamic Layers" -- a SEPARATE section from "Layers" above (the K.3
			// prototype, still GeneratorLayerOrder-backed). This one is backed entirely by
			// DynamicLayerStack, the M16-K.3A/K.3B domain -- explicitly disconnected from composition/
			// preview (see the "Prototype" caption below and DynamicLayerStack's own doc comment). No
			// production call (RecomposeWorkingColors, UpdateAllPreviews, generator invalidation, etc.)
			// is ever made from any control in this section.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SNew(SBorder)
				.Padding(FMargin(8.f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 2.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("DynamicLayersSectionTitle", "Dynamic Layers"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ContentPadding(FMargin(8.f, 1.f))
							.ToolTipText(LOCTEXT("AddDynamicLayerTooltip", "Add a new, empty (Fill=None) dynamic layer at the end of the list."))
							.Text(LOCTEXT("AddDynamicLayerButton", "+ Add Layer"))
							.OnClicked(FOnClicked::CreateLambda([this]() { return OnAddDynamicLayerClicked(); }))
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("DynamicLayersPrototypeNotice", "Prototype -- not applied to preview yet"))
						.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(DynamicLayersListContainer, SVerticalBox)
					]
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("BBoxMaskSectionTitle", "Bounding Box"))
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

						// AUDITED (live-preview migration): Unified Bounds stays here, unchanged, since it
						// is a Bounding-Box-specific parameter, not a preview/update control.
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
			// Bounding Box exactly -- same header style/pattern, same InitiallyCollapsed(true) default.
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
				.InitiallyCollapsed(true)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("AOSectionTitle", "Ambient Occlusion"))
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
								"Number of hemisphere raycast samples per vertex. Higher values are smoother "
								"but slower; the live preview always uses this full value."))
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
								"Number of hemisphere raycast samples per vertex. Higher values are smoother "
								"but slower; the live preview always uses this full value."))
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

			// Curvature Mask: the tool's third, independent, optional composition-stack layer -- same
			// collapsible panel pattern as Bounding Box/Ambient Occlusion above (SExpandableArea, header
			// title only, Enable moved into the body's first row -- same rationale as AO's own doc note).
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CurvatureSectionTitle", "Curvature"))
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
						.IsChecked(this, &SVertexMaskForgePanel::GetCurvatureEnableState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnCurvatureEnableChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("CurvatureEnableLabel", "Enable"))
						]
					]

					// Blend Mode + Opacity: same Slate controls/dimensions/alignment/labels/tooltips/
					// limits as Bounding Box/Ambient Occlusion's own (see those sections above) --
					// independent CurvatureBlendMode/CurvatureOpacity state, same BlendModeOptions list,
					// same ComposeMaskStack formulas.
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
							SNew(STextBlock).Text(LOCTEXT("CurvatureBlendModeLabel", "Blend Mode:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(CurvatureBlendModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
							.OptionsSource(&BlendModeOptions)
							.InitiallySelectedItem(BlendModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateCurvatureBlendModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnCurvatureBlendModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetCurvatureBlendModeButtonText)
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
							SNew(STextBlock).Text(LOCTEXT("CurvatureOpacityLabel", "Opacity:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value_Lambda([this]() { return CurvatureOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
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
							.Value_Lambda([this]() { return CurvatureOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
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
							SNew(STextBlock).Text(LOCTEXT("CurvatureTypeLabel", "Curvature Type:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(CurvatureTypeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeCurvatureType>>)
							.OptionsSource(&CurvatureTypeOptions)
							.InitiallySelectedItem(CurvatureTypeOptions[1])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateCurvatureTypeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnCurvatureTypeSelectionChanged)
							.ToolTipText(LOCTEXT("CurvatureTypeTooltip",
								"Convex: only outward edges/bulges. Concave: only cavities/creases. Both: both signs, without cancelling out."))
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetCurvatureTypeButtonText)
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(12.f, 0.f, 0.f, 0.f))
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetCurvatureInvertState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnCurvatureInvertChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("CurvatureInvertLabel", "Invert"))
							]
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
							SNew(STextBlock).Text(LOCTEXT("CurvatureMultiplierLabel", "Multiplier"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Value_Lambda([this]() { return CurvatureMultiplier; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureMultiplier = FMath::Max(NewValue, 0.0f);
								OnCurvatureParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Delta(0.01f)
							.Value_Lambda([this]() { return CurvatureMultiplier; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureMultiplier = FMath::Max(NewValue, 0.0f);
								OnCurvatureParamChanged();
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
							SNew(STextBlock)
							.Text(LOCTEXT("CurvatureBlurLabel", "Blur"))
							.ToolTipText(LOCTEXT("CurvatureBlurTooltip",
								"Topological smoothing of the Curvature mask. Whole number = full iterations; fractional part blends toward one more."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Value_Lambda([this]() { return CurvatureBlur; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureBlur = FMath::Clamp(NewValue, 0.0f, 10.0f);
								OnCurvatureParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Delta(0.01f)
							.Value_Lambda([this]() { return CurvatureBlur; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureBlur = FMath::Clamp(NewValue, 0.0f, 10.0f);
								OnCurvatureParamChanged();
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
							SNew(STextBlock)
							.Text(LOCTEXT("CurvatureLevelsMinLabel", "Levels Min"))
							.ToolTipText(LOCTEXT("CurvatureLevelsMinTooltip", "Values at or below this threshold become black."))
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
							.ToolTipText(LOCTEXT("CurvatureLevelsMinTooltip", "Values at or below this threshold become black."))
							.Value_Lambda([this]() { return CurvatureLevelsMin; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureLevelsMin = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnCurvatureParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("CurvatureLevelsMaxLabel", "Levels Max"))
							.ToolTipText(LOCTEXT("CurvatureLevelsMaxTooltip", "Values at or above this threshold become white."))
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
							.ToolTipText(LOCTEXT("CurvatureLevelsMaxTooltip", "Values at or above this threshold become white."))
							.Value_Lambda([this]() { return CurvatureLevelsMax; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								CurvatureLevelsMax = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnCurvatureParamChanged();
							})
						]
					]
				]
			]

			// Directional Normal Mask (V2-E): the tool's independent, optional composition-stack layer
			// positioned visually between Curvature and Noise -- same collapsible panel pattern as
			// Bounding Box/Ambient Occlusion/Curvature above. (Its EVertexMaskForgeScalarMaskSource enum
			// value is appended AFTER Material Slot for numeric stability -- visual position and enum
			// declaration order are deliberately independent, see that enum's own doc comment.)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DirectionalNormalMaskSectionTitle", "Directional Normal"))
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
						.IsChecked(this, &SVertexMaskForgePanel::GetDirectionalNormalMaskEnableState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnDirectionalNormalMaskEnableChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("DirectionalNormalMaskEnableLabel", "Enable"))
						]
					]

					// Blend Mode + Blend: same Slate controls/dimensions/alignment/labels/tooltips/limits
					// as the other layers' own (see those sections above). Purely compositional -- changing
					// either only calls RecomposeWorkingColors(), never regenerates the raw mask.
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
							SNew(STextBlock).Text(LOCTEXT("DirectionalNormalMaskBlendModeLabel", "Blend Mode:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(DirectionalNormalMaskBlendModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
							.OptionsSource(&BlendModeOptions)
							.InitiallySelectedItem(BlendModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateDirectionalNormalMaskBlendModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnDirectionalNormalMaskBlendModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetDirectionalNormalMaskBlendModeButtonText)
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
							SNew(STextBlock).Text(LOCTEXT("DirectionalNormalMaskOpacityLabel", "Opacity:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value_Lambda([this]() { return DirectionalNormalMaskOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalMaskOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
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
							.Value_Lambda([this]() { return DirectionalNormalMaskOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalMaskOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
						]
					]

					// Space: Local vs. World.
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
							SNew(STextBlock)
							.Text(LOCTEXT("NormalSpaceLabel", "Space:"))
							.ToolTipText(LOCTEXT("NormalSpaceTooltip",
								"Local Space evaluates normals in the Static Mesh asset's own space -- the same result for every instance, unaffected by Actor/Component rotation. "
								"World Space evaluates normals using the selected component's transform. The generated colors are saved to the Static Mesh asset and therefore affect every instance of that asset."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						[
							SAssignNew(NormalSpaceComboBox, SComboBox<TSharedPtr<EVertexMaskForgeNormalSpace>>)
							.OptionsSource(&NormalSpaceOptions)
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateNormalSpaceRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnNormalSpaceSelectionChanged)
							.ToolTipText(LOCTEXT("NormalSpaceTooltip",
								"Local Space evaluates normals in the Static Mesh asset's own space -- the same result for every instance, unaffected by Actor/Component rotation. "
								"World Space evaluates normals using the selected component's transform. The generated colors are saved to the Static Mesh asset and therefore affect every instance of that asset."))
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetNormalSpaceButtonText)
							]
						]
					]

					// Direction: the six principal axes.
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
							SNew(STextBlock).Text(LOCTEXT("NormalDirectionLabel", "Direction:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						[
							SAssignNew(NormalDirectionComboBox, SComboBox<TSharedPtr<EVertexMaskForgeNormalDirection>>)
							.OptionsSource(&NormalDirectionOptions)
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateNormalDirectionRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnNormalDirectionSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetNormalDirectionButtonText)
							]
						]
					]

					// Angle (degrees, 0-180).
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
							SNew(STextBlock)
							.Text(LOCTEXT("DirectionalNormalAngleLabel", "Angle"))
							.ToolTipText(LOCTEXT("DirectionalNormalAngleTooltip", "Cone half-angle, in degrees, from the selected Direction. 0 = only exact alignment; 180 = full coverage."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(180.0f)
							.Value_Lambda([this]() { return DirectionalNormalAngle; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalAngle = FMath::Clamp(NewValue, 0.0f, 180.0f);
								OnDirectionalNormalMaskGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(180.0f)
							.Delta(0.1f)
							.Value_Lambda([this]() { return DirectionalNormalAngle; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalAngle = FMath::Clamp(NewValue, 0.0f, 180.0f);
								OnDirectionalNormalMaskGenerativeParamChanged();
							})
						]
					]

					// Falloff (degrees, 0-180, internally clamped to [0, Angle] at generation time).
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
							SNew(STextBlock)
							.Text(LOCTEXT("DirectionalNormalFalloffLabel", "Falloff"))
							.ToolTipText(LOCTEXT("DirectionalNormalFalloffTooltip", "Smooth transition width, in degrees, ending exactly at Angle. Internally clamped to [0, Angle] -- never causes a division by zero."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(180.0f)
							.Value_Lambda([this]() { return DirectionalNormalFalloff; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalFalloff = FMath::Clamp(NewValue, 0.0f, 180.0f);
								OnDirectionalNormalMaskGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(180.0f)
							.Delta(0.1f)
							.Value_Lambda([this]() { return DirectionalNormalFalloff; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalFalloff = FMath::Clamp(NewValue, 0.0f, 180.0f);
								OnDirectionalNormalMaskGenerativeParamChanged();
							})
						]
					]

					// Blur: topological smoothing of the raw Directional Normal Mask, applied BEFORE Invert --
					// same widget/range/default/tooltip pattern as Curvature's own Blur (see that section
					// above and ApplyAdjacencyTopologicalBlur's doc comment for why the underlying adjacency
					// must differ by domain even though the algorithm and UI are identical).
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
							SNew(STextBlock)
							.Text(LOCTEXT("DirectionalNormalBlurLabel", "Blur"))
							.ToolTipText(LOCTEXT("DirectionalNormalBlurTooltip",
								"Topological smoothing of the Directional Normal Mask, applied before Invert. Whole number = full iterations; fractional part blends toward one more."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Value_Lambda([this]() { return DirectionalNormalBlur; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalBlur = FMath::Clamp(NewValue, 0.0f, 10.0f);
								OnDirectionalNormalMaskGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Delta(0.01f)
							.Value_Lambda([this]() { return DirectionalNormalBlur; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								DirectionalNormalBlur = FMath::Clamp(NewValue, 0.0f, 10.0f);
								OnDirectionalNormalMaskGenerativeParamChanged();
							})
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
					[
						SNew(SCheckBox)
						.IsChecked(this, &SVertexMaskForgePanel::GetDirectionalNormalMaskInvertState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnDirectionalNormalMaskInvertChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("DirectionalNormalMaskInvertLabel", "Invert"))
						]
					]

					// Diagnostic: World-Space multi-instance conflict / degenerate transform reasons --
					// empty (renders nothing) when available and valid.
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetDirectionalNormalMaskDiagnosticText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]
			]

			// Thickness Mask (V2-G): the tool's independent, optional composition-stack layer positioned
			// visually between Directional Normal and Noise -- same collapsible panel pattern, same
			// Enable -> Blend Mode -> Opacity -> [generative params] -> Blur -> Invert -> diagnostic
			// layout as Directional Normal above. Asset Local Space ONLY -- no Space seletor.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ThicknessMaskSectionTitle", "Thickness"))
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
						.IsChecked(this, &SVertexMaskForgePanel::GetThicknessMaskEnableState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnThicknessMaskEnableChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ThicknessMaskEnableLabel", "Enable"))
						]
					]

					// Blend Mode + Opacity: same pattern as every other generator, at the top after Enable.
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
							SNew(STextBlock).Text(LOCTEXT("ThicknessMaskBlendModeLabel", "Blend Mode:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(ThicknessMaskBlendModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
							.OptionsSource(&BlendModeOptions)
							.InitiallySelectedItem(BlendModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateThicknessMaskBlendModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnThicknessMaskBlendModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetThicknessMaskBlendModeButtonText)
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
							SNew(STextBlock).Text(LOCTEXT("ThicknessMaskOpacityLabel", "Opacity:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value_Lambda([this]() { return ThicknessMaskOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessMaskOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
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
							.Value_Lambda([this]() { return ThicknessMaskOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessMaskOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
						]
					]

					// Min Thickness (renormalizes only -- never triggers a new raycast).
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
							SNew(STextBlock)
							.Text(LOCTEXT("ThicknessMinLabel", "Min Thickness"))
							.ToolTipText(LOCTEXT("ThicknessMinTooltip", "Measured thickness at or below this value reads as white. Local-space units."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(10000.0f)
							.Value_Lambda([this]() { return ThicknessMinThickness; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessMinThickness = FMath::Clamp(NewValue, 0.0f, 10000.0f);
								OnThicknessPostProcessParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10000.0f)
							.Delta(0.1f)
							.Value_Lambda([this]() { return ThicknessMinThickness; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessMinThickness = FMath::Clamp(NewValue, 0.0f, 10000.0f);
								OnThicknessPostProcessParamChanged();
							})
						]
					]

					// Max Thickness (renormalizes only -- never triggers a new raycast).
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
							SNew(STextBlock)
							.Text(LOCTEXT("ThicknessMaxLabel", "Max Thickness"))
							.ToolTipText(LOCTEXT("ThicknessMaxTooltip", "Measured thickness at or above this value reads as black (still a valid measurement, distinct from Search Distance). Local-space units."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(10000.0f)
							.Value_Lambda([this]() { return ThicknessMaxThickness; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessMaxThickness = FMath::Clamp(NewValue, 0.0f, 10000.0f);
								OnThicknessPostProcessParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10000.0f)
							.Delta(0.1f)
							.Value_Lambda([this]() { return ThicknessMaxThickness; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessMaxThickness = FMath::Clamp(NewValue, 0.0f, 10000.0f);
								OnThicknessPostProcessParamChanged();
							})
						]
					]

					// Search Distance (triggers a new raycast -- distinct from Max Thickness).
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
							SNew(STextBlock)
							.Text(LOCTEXT("ThicknessSearchLabel", "Search Distance"))
							.ToolTipText(LOCTEXT("ThicknessSearchTooltip", "Maximum physical raycast distance from the origin surface. Must be at least Max Thickness -- enforced automatically. Local-space units."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(10000.0f)
							.Value_Lambda([this]() { return ThicknessSearchDistance; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessSearchDistance = FMath::Clamp(NewValue, 0.0f, 10000.0f);
								OnThicknessRaycastParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10000.0f)
							.Delta(0.1f)
							.Value_Lambda([this]() { return ThicknessSearchDistance; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessSearchDistance = FMath::Clamp(NewValue, 0.0f, 10000.0f);
								OnThicknessRaycastParamChanged();
							})
						]
					]

					// Bias (triggers a new raycast).
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
							SNew(STextBlock)
							.Text(LOCTEXT("ThicknessBiasLabel", "Bias"))
							.ToolTipText(LOCTEXT("ThicknessBiasTooltip", "Ray origin offset into the mesh, used only to avoid self-hit; reconstructed out of the measured distance so it never shifts the result artistically."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.001f)
							.MaxValue(10.0f)
							.Value_Lambda([this]() { return ThicknessBias; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessBias = FMath::Clamp(NewValue, 0.001f, 10.0f);
								OnThicknessRaycastParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.001f)
							.MaxValue(10.0f)
							.Delta(0.001f)
							.Value_Lambda([this]() { return ThicknessBias; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessBias = FMath::Clamp(NewValue, 0.001f, 10.0f);
								OnThicknessRaycastParamChanged();
							})
						]
					]

					// Blur: same widget/range/default/tooltip pattern as Directional Normal's own Blur.
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
							SNew(STextBlock)
							.Text(LOCTEXT("ThicknessBlurLabel", "Blur"))
							.ToolTipText(LOCTEXT("ThicknessBlurTooltip",
								"Topological smoothing of the normalized Thickness mask, applied before Invert. Whole number = full iterations; fractional part blends toward one more."))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Value_Lambda([this]() { return ThicknessBlur; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessBlur = FMath::Clamp(NewValue, 0.0f, 10.0f);
								OnThicknessPostProcessParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Delta(0.01f)
							.Value_Lambda([this]() { return ThicknessBlur; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								ThicknessBlur = FMath::Clamp(NewValue, 0.0f, 10.0f);
								OnThicknessPostProcessParamChanged();
							})
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
					[
						SNew(SCheckBox)
						.IsChecked(this, &SVertexMaskForgePanel::GetThicknessMaskInvertState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnThicknessMaskInvertChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ThicknessMaskInvertLabel", "Invert"))
						]
					]

					// Diagnostic: no-hit / invalid-geometry / degenerate reasons -- empty when fully valid.
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetThicknessMaskDiagnosticText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]
			]

			// Noise Mask (V1): the tool's fourth, independent, optional composition-stack layer -- same
			// collapsible panel pattern as Bounding Box/Ambient Occlusion/Curvature above.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NoiseSectionTitle", "Noise"))
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
						.IsChecked(this, &SVertexMaskForgePanel::GetNoiseEnableState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnNoiseEnableChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseEnableLabel", "Enable"))
						]
					]

					// Blend Mode + Opacity: same Slate controls/dimensions/alignment/labels/tooltips/
					// limits as the other three layers' own (see those sections above).
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
							SNew(STextBlock).Text(LOCTEXT("NoiseBlendModeLabel", "Blend Mode:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(NoiseBlendModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
							.OptionsSource(&BlendModeOptions)
							.InitiallySelectedItem(BlendModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateNoiseBlendModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnNoiseBlendModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetNoiseBlendModeButtonText)
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
							SNew(STextBlock).Text(LOCTEXT("NoiseOpacityLabel", "Opacity:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value_Lambda([this]() { return NoiseOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
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
							.Value_Lambda([this]() { return NoiseOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
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
							SNew(STextBlock).Text(LOCTEXT("NoiseTypeLabel", "Noise Type:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(NoiseTypeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeNoiseType>>)
							.OptionsSource(&NoiseTypeOptions)
							.InitiallySelectedItem(NoiseTypeOptions[1])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateNoiseTypeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnNoiseTypeSelectionChanged)
							.ToolTipText(LOCTEXT("NoiseTypeTooltip",
								"Perlin: a single noise octave. Fractal Perlin (FBM): several octaves summed for more detail."))
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetNoiseTypeButtonText)
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(12.f, 0.f, 0.f, 0.f))
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetNoiseInvertState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnNoiseInvertChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("NoiseInvertLabel", "Invert"))
							]
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
							SNew(STextBlock).Text(LOCTEXT("NoiseScaleLabel", "Scale"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.001f)
							.MaxValue(1000.0f)
							.Delta(0.01f)
							.ToolTipText(LOCTEXT("NoiseScaleXTooltip", "Frequency multiplier along local X. 1.0 is approximately one noise unit per meter."))
							.Value_Lambda([this]() { return NoiseScaleX; })
							.OnValueChanged(this, &SVertexMaskForgePanel::OnNoiseScaleXChanged)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.001f)
							.MaxValue(1000.0f)
							.Delta(0.01f)
							.ToolTipText(LOCTEXT("NoiseScaleYTooltip", "Frequency multiplier along local Y."))
							.IsEnabled_Lambda([this]() { return !bNoiseScaleAxesLocked; })
							.Value_Lambda([this]() { return NoiseScaleY; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseScaleY = FMath::Max(NewValue, 0.001f);
								OnNoiseGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.001f)
							.MaxValue(1000.0f)
							.Delta(0.01f)
							.ToolTipText(LOCTEXT("NoiseScaleZTooltip", "Frequency multiplier along local Z."))
							.IsEnabled_Lambda([this]() { return !bNoiseScaleAxesLocked; })
							.Value_Lambda([this]() { return NoiseScaleZ; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseScaleZ = FMath::Max(NewValue, 0.001f);
								OnNoiseGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.ToolTipText(LOCTEXT("NoiseScaleAxesLockedTooltip", "Use Scale X for all three axes."))
							.IsChecked(this, &SVertexMaskForgePanel::GetNoiseScaleAxesLockState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnNoiseScaleAxesLockChanged)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("NoiseScaleAxesLockedLabel", "Lock Axes"))
							]
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
							SNew(STextBlock).Text(LOCTEXT("NoiseOffsetLabel", "Offset"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(-100000.0f)
							.MaxValue(100000.0f)
							.Delta(0.1f)
							.ToolTipText(LOCTEXT("NoiseOffsetXTooltip", "Domain offset along X, in noise space."))
							.Value_Lambda([this]() { return NoiseOffsetX; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseOffsetX = NewValue;
								OnNoiseGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(-100000.0f)
							.MaxValue(100000.0f)
							.Delta(0.1f)
							.ToolTipText(LOCTEXT("NoiseOffsetYTooltip", "Domain offset along Y, in noise space."))
							.Value_Lambda([this]() { return NoiseOffsetY; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseOffsetY = NewValue;
								OnNoiseGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(-100000.0f)
							.MaxValue(100000.0f)
							.Delta(0.1f)
							.ToolTipText(LOCTEXT("NoiseOffsetZTooltip", "Domain offset along Z, in noise space."))
							.Value_Lambda([this]() { return NoiseOffsetZ; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseOffsetZ = NewValue;
								OnNoiseGenerativeParamChanged();
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
							SNew(STextBlock).Text(LOCTEXT("NoiseSeedLabel", "Seed"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
						[
							SNew(SSpinBox<int32>)
							.MinDesiredWidth(64.f)
							.MinValue(-2147483647)
							.MaxValue(2147483647)
							.Delta(1)
							.Value_Lambda([this]() { return NoiseSeed; })
							.OnValueChanged_Lambda([this](const int32 NewValue)
							{
								NoiseSeed = NewValue;
								OnNoiseGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseMultiplierLabel", "Multiplier"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(10.0f)
							.Delta(0.01f)
							.Value_Lambda([this]() { return NoiseMultiplier; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseMultiplier = FMath::Max(NewValue, 0.0f);
								OnNoiseArtisticParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseBlurLabel", "Blur"))
							.ToolTipText(LOCTEXT("NoiseBlurTooltip", "Smooths the procedural noise field before Multiplier and Levels are applied."))
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
							.ToolTipText(LOCTEXT("NoiseBlurTooltip", "Smooths the procedural noise field before Multiplier and Levels are applied."))
							.Value_Lambda([this]() { return NoiseBlur; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseBlur = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnNoiseGenerativeParamChanged();
							})
						]
					]

					// FBM-only controls -- always visible in V1 (no dynamic show/hide by Noise Type),
					// but disabled (IsEnabled) when Noise Type is Perlin since ComputeRawNoiseValue
					// never reads them in that branch.
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
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseOctavesLabel", "Octaves"))
							.ToolTipText(LOCTEXT("NoiseOctavesTooltip", "Multi-octave types only (Fractal Perlin, Billow, Ridged, Turbulence)."))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
						[
							SNew(SSpinBox<int32>)
							.MinDesiredWidth(44.f)
							.MinValue(1)
							.MaxValue(8)
							.Delta(1)
							.IsEnabled_Lambda([this]() { return UsesFractalParameters(); })
							.Value_Lambda([this]() { return NoiseOctaves; })
							.OnValueChanged_Lambda([this](const int32 NewValue)
							{
								NoiseOctaves = FMath::Clamp(NewValue, 1, 8);
								OnNoiseGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseRoughnessLabel", "Roughness"))
							.ToolTipText(LOCTEXT("NoiseRoughnessTooltip", "Per-octave amplitude multiplier. Multi-octave types only."))
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
							.IsEnabled_Lambda([this]() { return UsesFractalParameters(); })
							.Value_Lambda([this]() { return NoiseRoughness; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseRoughness = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnNoiseGenerativeParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseLacunarityLabel", "Lacunarity"))
							.ToolTipText(LOCTEXT("NoiseLacunarityTooltip", "Per-octave frequency multiplier. Multi-octave types only."))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(1.0f)
							.MaxValue(10.0f)
							.Delta(0.01f)
							.IsEnabled_Lambda([this]() { return UsesFractalParameters(); })
							.Value_Lambda([this]() { return NoiseLacunarity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseLacunarity = FMath::Max(NewValue, 1.0f);
								OnNoiseGenerativeParamChanged();
							})
						]
					]

					// Turbulence-only control -- always visible (no dynamic show/hide by Noise Type),
					// but disabled (IsEnabled) unless Noise Type is Turbulence, same contract as the
					// Octaves/Roughness/Lacunarity row above.
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
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseTurbulenceStrengthLabel", "Turbulence Strength"))
							.ToolTipText(LOCTEXT("NoiseTurbulenceStrengthTooltip", "Domain-warp displacement strength, in noise space. Turbulence only."))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(52.f)
							.MinValue(0.0f)
							.MaxValue(5.0f)
							.Delta(0.01f)
							.IsEnabled_Lambda([this]() { return NoiseType == EVertexMaskForgeNoiseType::Turbulence; })
							.Value_Lambda([this]() { return NoiseTurbulenceStrength; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseTurbulenceStrength = FMath::Clamp(NewValue, 0.0f, 5.0f);
								OnNoiseGenerativeParamChanged();
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
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseLevelsMinLabel", "Levels Min"))
							.ToolTipText(LOCTEXT("NoiseLevelsMinTooltip", "Values at or below this threshold become black."))
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
							.ToolTipText(LOCTEXT("NoiseLevelsMinTooltip", "Values at or below this threshold become black."))
							.Value_Lambda([this]() { return NoiseLevelsMin; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseLevelsMin = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnNoiseArtisticParamChanged();
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("NoiseLevelsMaxLabel", "Levels Max"))
							.ToolTipText(LOCTEXT("NoiseLevelsMaxTooltip", "Values at or above this threshold become white."))
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
							.ToolTipText(LOCTEXT("NoiseLevelsMaxTooltip", "Values at or above this threshold become white."))
							.Value_Lambda([this]() { return NoiseLevelsMax; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								NoiseLevelsMax = FMath::Clamp(NewValue, 0.0f, 1.0f);
								OnNoiseArtisticParamChanged();
							})
						]
					]
				]
			]

			// Material Slot Mask (V2-D): the tool's fifth, independent, optional composition-stack layer
			// -- same collapsible panel pattern as Bounding Box/Ambient Occlusion/Curvature/Noise above.
			// V1 SCOPE: requires exactly one selected mesh -- see IsMaterialSlotMaskAvailableForSelection.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.Padding(FMargin(8.f))
				.HeaderContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MaterialSlotMaskSectionTitle", "Material Slot"))
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
						.IsChecked(this, &SVertexMaskForgePanel::GetMaterialSlotMaskEnableState)
						.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnMaterialSlotMaskEnableChanged)
						.Content()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("MaterialSlotMaskEnableLabel", "Enable"))
						]
					]

					// Blend Mode + Opacity: same Slate controls/dimensions/alignment/labels/tooltips/
					// limits as the other four layers' own (see those sections above).
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
							SNew(STextBlock).Text(LOCTEXT("MaterialSlotMaskBlendModeLabel", "Blend Mode:"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(MaterialSlotMaskBlendModeComboBox, SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>)
							.OptionsSource(&BlendModeOptions)
							.InitiallySelectedItem(BlendModeOptions[0])
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateMaterialSlotMaskBlendModeRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnMaterialSlotMaskBlendModeSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetMaterialSlotMaskBlendModeButtonText)
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
							SNew(STextBlock).Text(LOCTEXT("MaterialSlotMaskOpacityLabel", "Opacity:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SSlider)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value_Lambda([this]() { return MaterialSlotMaskOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								MaterialSlotMaskOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
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
							.Value_Lambda([this]() { return MaterialSlotMaskOpacity; })
							.OnValueChanged_Lambda([this](const float NewValue)
							{
								MaterialSlotMaskOpacity = FMath::Clamp(NewValue, 0.0f, 1.0f);
								RecomposeWorkingColors();
							})
						]
					]

					// Material Slot dropdown + Invert -- disabled entirely when the V1 single-mesh scope
					// requirement is not met (see IsMaterialSlotMaskAvailableForSelection), same visual
					// "disabled, not hidden" convention as every other conditionally-relevant control in
					// this panel (e.g. Octaves/Roughness/Lacunarity under Noise).
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
							SNew(STextBlock).Text(LOCTEXT("MaterialSlotLabel", "Material Slot:"))
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
						[
							SAssignNew(MaterialSlotComboBox, SComboBox<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>>)
							.IsEnabled_Lambda([this]() { return IsMaterialSlotMaskAvailableForSelection(); })
							.OptionsSource(&MaterialSlotOptions)
							.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateMaterialSlotRow)
							.OnSelectionChanged(this, &SVertexMaskForgePanel::OnMaterialSlotSelectionChanged)
							[
								SNew(STextBlock)
								.Text(this, &SVertexMaskForgePanel::GetMaterialSlotButtonText)
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsEnabled_Lambda([this]() { return IsMaterialSlotMaskAvailableForSelection(); })
							.IsChecked(this, &SVertexMaskForgePanel::GetMaterialSlotMaskInvertState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnMaterialSlotMaskInvertChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("MaterialSlotMaskInvertLabel", "Invert"))
							]
						]
					]

					// Diagnostic: single-mesh-scope / unresolved-mapping reasons Material Slot Mask may
					// currently be unavailable -- empty (renders nothing) when available and valid.
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetMaterialSlotMaskDiagnosticText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]
			]

			// AUDITED (pre-modularization UI/defaults pass): moved here, immediately below the Material
			// Slot section and directly above Preview Mode -- the sole entry point into an editing
			// session now sits next to session-level controls (Preview Mode/Fill) rather than above every
			// generator panel. Disabled while already editing (CanEditVertexMask) so a repeated click can
			// never start a second, overlapping session. Same widget/callback/tooltip/enablement as
			// before -- moved, not recreated.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 12.f, 0.f, 4.f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
				[
					SNew(SButton)
					.Text(LOCTEXT("EditVertexMask", "Edit Vertex Mask"))
					.ToolTipText(LOCTEXT("EditVertexMaskTooltip", "Start a vertex mask editing session for the current selection."))
					.OnClicked(this, &SVertexMaskForgePanel::OnEditVertexMaskClicked)
					.IsEnabled(this, &SVertexMaskForgePanel::CanEditVertexMask)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SVertexMaskForgePanel::GetEditSessionStatusText)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
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

					// AUDITED (live-preview migration): every parameter change regenerates/republishes the
					// preview automatically (RunAutoUpdatePreview via an immediate call or a short
					// debounce) -- there is no manual "Generate Mask" action and no "Auto Update Preview"
					// toggle to gate it; this status line alone communicates preview state.
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetPreviewStatusText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
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
			] // closes SScrollBox::Slot content
		]
	];

	// AUDITED (M16-K.3): GeneratorLayerListContainer now exists (assigned via SAssignNew above, inside
	// the ChildSlot expression just completed) -- populate its rows from GeneratorLayerOrder's own
	// member-initializer default (VertexMaskForgeLayerOrder::MakeDefault(), set once when this panel
	// instance's fields were constructed, BEFORE this function body even started running). This is a
	// single, one-time initial population -- never a second default derivation.
	RebuildGeneratorLayerList();

	// AUDITED (M16-K.4): DynamicLayersListContainer now exists (assigned via SAssignNew above) --
	// populate its rows from DynamicLayerStack's own member-initializer default
	// (FVertexMaskForgeDynamicLayerStack::MakeInitialStack(), set once when this panel instance's
	// fields were constructed) -- a single, one-time initial population, mirroring
	// RebuildGeneratorLayerList's own call just above. DynamicLayerStack itself is never
	// re-initialized here or anywhere else in Construct().
	RebuildDynamicLayersList();

	// AUDITED (UX1, explicit Edit Vertex Mask session entry): Construct() must never itself start an
	// editing session (previously called RefreshSelection() here unconditionally, which meant simply
	// opening the panel with something already selected in the Editor began editing immediately) --
	// only populates the Idle-state candidate list so "Edit Vertex Mask" is correctly enabled/disabled
	// from the very first paint.
	RefreshCandidateSelection();
}

SVertexMaskForgePanel::~SVertexMaskForgePanel()
{
	// Removed first: guarantees OnWorldCleanup can never fire on a partially-destructed panel while
	// the rest of this destructor (and DestroyAllPreviews below) runs.
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupDelegateHandle);

	// Same rationale, for the same reason: no callback into a partially-destructed panel.
	USelection::SelectionChangedEvent.Remove(SelectionChangedDelegateHandle);

	// Same rationale, for the same reason: no callback into a partially-destructed panel.
	if (GEngine)
	{
		GEngine->OnActorMoved().Remove(ActorMovedDelegateHandle);
	}

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
	// Bound to USelection::SelectionChangedEvent (fired for Actor/Component/BSP selection sets -- see
	// Editor/UnrealEd/Public/Selection.h; NOT fired for Content Browser asset selection, which uses a
	// completely separate API and is no longer consulted anywhere in this panel -- see
	// CollectViewportSelection, the only collector left). NewSelection (which USelection instance
	// changed) is deliberately unused: regardless of which one fired, this only ever re-derives
	// candidate/session state from the CURRENT scene selection.
	//
	// AUDITED (UX1, explicit Edit Vertex Mask session entry): a scene selection change must NEVER
	// start, retarget, or silently end an editing session by itself -- only the "Edit Vertex Mask"
	// button (OnEditVertexMaskClicked) may call RefreshSelection(). So while bIsEditingVertexMask is
	// true, SelectedMeshes/PreviewComponents keep pointing at the session's ORIGINAL targets
	// unconditionally; this never calls RefreshSelection() or RefreshCandidateSelection() in that case
	// (neither touches SelectedMeshes, but RefreshCandidateSelection() is also deliberately skipped so
	// CandidateMeshes reflects the pre-session selection, not a mid-session distraction).
	//
	// DEFERRED SYNC (audited): rather than requiring another viewport/World Outliner click after the
	// user concludes the session, this records that a candidate resync is owed
	// (bSceneSelectionChangedDuringActiveOperation = true). SyncSelectionIfChangedDuringOperation(),
	// called at the tail of OnCancelChangesClicked() / AcceptPendingChanges() (and ONLY there -- see
	// its own doc comment), consumes this flag and calls RefreshCandidateSelection() once the session
	// has fully concluded -- never RefreshSelection(), so concluding a session never auto-starts
	// another one (requirement #6).
	if (bIsEditingVertexMask)
	{
		bSceneSelectionChangedDuringActiveOperation = true;
		return;
	}

	// Not editing: this call itself is about to sync CandidateMeshes with the current scene selection
	// directly, so any flag left over from an earlier session is moot -- clear it defensively so a
	// later Cancel/Accept never performs a redundant extra candidate refresh for a selection change
	// this call already picked up.
	bSceneSelectionChangedDuringActiveOperation = false;

	RefreshCandidateSelection();
}

/**
 * AUDITED (V2-E corrective pass, transform freshness): fires once per completed viewport gizmo move/
 * rotate/scale, for ANY actor anywhere in the level (see ActorMovedDelegateHandle's own doc comment for
 * the engine-source evidence) -- filtered here to do nothing at all unless Directional Normal Mask is
 * actually enabled AND in World Space (Local Space is transform-independent by construction and must
 * never be regenerated by an instance transform change), and unless the moved Actor actually owns one of
 * THIS panel's own currently-tracked PreviewComponents. Invalidates ONLY DirectionalNormalMask (and its
 * own bDirectionalNormalWorldSpaceConflict flag) for the affected entry/entries -- never touches AO/
 * Curvature/Noise/Material Slot state or caches. Mirrors OnDirectionalNormalMaskGenerativeParamChanged's
 * own "invalidate, then always regenerate immediately" contract -- reuses the SAME debounce-clearing/
 * RunAutoUpdatePreview call, no new timer.
 */
void SVertexMaskForgePanel::OnActorMovedForDirectionalNormal(AActor* Actor)
{
	if (!Actor || !bDirectionalNormalMaskEnabled || DirectionalNormalSpace != EVertexMaskForgeNormalSpace::World)
	{
		return;
	}

	bool bAnyEntryAffected = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		bool bThisEntryAffected = false;
		for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
		{
			const UStaticMeshComponent* Component = StateOwner->GetPreviewState().GetSourceComponent().Get();
			if (IsValid(Component) && Component->GetOwner() == Actor)
			{
				bThisEntryAffected = true;
				break;
			}
		}
		if (bThisEntryAffected)
		{
			Entry->GeneratorState.DirectionalNormalMask = FVertexMaskForgeScalarMask();
			Entry->GeneratorState.bDirectionalNormalWorldSpaceConflict = false;
			bAnyEntryAffected = true;
		}
	}

	if (!bAnyEntryAffected)
	{
		return; // The moved Actor owns none of this panel's tracked components -- nothing to do.
	}

	LastMaskActionStatusText = FText::GetEmpty();
	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
	}
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::SyncSelectionIfChangedDuringOperation()
{
	if (!bSceneSelectionChangedDuringActiveOperation)
	{
		return;
	}

	bSceneSelectionChangedDuringActiveOperation = false;

	// AUDITED (UX1): re-derives CandidateMeshes only -- never starts another session automatically
	// (see requirement #6). The user must click "Edit Vertex Mask" again for the new selection.
	RefreshCandidateSelection();
}

void SVertexMaskForgePanel::RefreshCandidateSelection()
{
	// AUDITED (UX1, explicit Edit Vertex Mask session entry): the Idle-state counterpart to
	// RefreshSelection() -- discovers/validates the CURRENT scene selection exactly the same way
	// (same CollectViewportSelection precedence/filtering, same UpdateMeshDiagnostics), but stops
	// there: never BuildWorkingMeshes() (no FDynamicMesh3 conversion), never DestroyAllPreviews()/
	// UpdateAllPreviews() (no PreviewComponents created, no components hidden, no generators run), and
	// never touches SelectedMeshes (the session's own targets). Safe to call at any time while not
	// editing -- it can only ever affect CandidateMeshes and, through it, the "Edit Vertex Mask"
	// button's enabled state and the Idle status text.
	CandidateMeshes.Empty();
	TMap<FString, int32> PathToIndex;
	CollectViewportSelection(CandidateMeshes, PathToIndex);
	UpdateMeshDiagnostics(CandidateMeshes);
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

	// AUDITED (V2-D): rebuilds the Material Slot dropdown from the (possibly new) single-mesh
	// selection and reconciles SelectedMaterialSlotIndex against it -- see this function's own doc
	// comment. Must run AFTER SelectedMeshes is assigned (it reads SelectedMeshes.Num()/[0]).
	ReconcileMaterialSlotSelection();

	UE_LOG(LogVertexMaskForge, Log, TEXT("Refreshed selection: %d unique Static Mesh asset(s)"), SelectedMeshes.Num());

	// New entries always start with BoundingBoxMask == NotGenerated; if a Vertex Color preview
	// mode is still selected, this shows original colors + "Mask Not Ready" rather than anything
	// stale. bCommit=false: a fresh selection never consolidates anything (there is nothing to
	// consolidate yet -- BaselineColors/CommittedColors/WorkingColors are all freshly empty for
	// every new PreviewComponentState).
	UpdateAllPreviews(/*bCommit=*/false);

	// AUDITED (live-preview migration): a generator's Enable state is preserved across a selection
	// change (see e.g. OnThicknessMaskEnableChanged's own doc comment), but the newly selected
	// entry/entries have never been generated for -- with no manual "Generate Mask" action left to
	// fall back on, this is the only remaining place that can produce a live preview for a NEW
	// selection when a generator was already enabled from working with a PREVIOUS one. A no-op
	// (immediately returns) when nothing is enabled.
	bool bAnyAxisEnabledOnRefresh = false;
	for (const FVertexMaskForgeAxisMaskParams& Params : BoundingBoxAxisParams)
	{
		if (Params.bEnabled)
		{
			bAnyAxisEnabledOnRefresh = true;
			break;
		}
	}
	if (bAnyAxisEnabledOnRefresh || bAOEnabled || bCurvatureEnabled || bNoiseEnabled
		|| bMaterialSlotMaskEnabled || bDirectionalNormalMaskEnabled || bThicknessMaskEnabled)
	{
		if (GEditor)
		{
			GEditor->GetTimerManager()->ClearTimer(AutoUpdateDebounceTimerHandle);
		}
		RunAutoUpdatePreview();
	}
}

FReply SVertexMaskForgePanel::OnEditVertexMaskClicked()
{
	// AUDITED (UX1, explicit Edit Vertex Mask session entry): the sole entry point into an editing
	// session. CanEditVertexMask() (bound to this button's IsEnabled) already guards against a
	// repeated/duplicate click while a session is active or with no valid candidate, but re-check here
	// too -- IsEnabled is a Slate presentation binding, not an input guarantee, and this handler must
	// stay correct even if invoked through another path in the future.
	if (bIsEditingVertexMask || CandidateMeshes.IsEmpty())
	{
		return FReply::Handled();
	}

	bIsEditingVertexMask = true;

	// Re-validates the selection and captures a snapshot of every currently valid target -- reuses
	// RefreshSelection() completely unchanged (same CollectViewportSelection precedence/filtering, so
	// multi-target sessions are preserved exactly as before), which re-queries the CURRENT scene
	// selection itself rather than reusing CandidateMeshes verbatim, so the snapshot reflects the
	// selection at the moment of THIS click, not a possibly-stale prior candidate refresh.
	RefreshSelection();

	return FReply::Handled();
}

FText SVertexMaskForgePanel::GetEditSessionStatusText() const
{
	if (bIsEditingVertexMask)
	{
		return FText::Format(
			LOCTEXT("EditSessionStatusEditingFormat", "Editing {0} target(s)."),
			FText::AsNumber(SelectedMeshes.Num()));
	}

	if (CandidateMeshes.IsEmpty())
	{
		return LOCTEXT("EditSessionStatusNoSelection", "No valid selection.");
	}

	return FText::Format(
		LOCTEXT("EditSessionStatusCandidateFormat", "{0} target(s) selected. Click Edit Vertex Mask to begin."),
		FText::AsNumber(CandidateMeshes.Num()));
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
		UStaticMesh* Mesh = const_cast<UStaticMesh*>(VertexMaskForgePanel::ResolveWorkingStaticMesh(Entry->Mesh));
		FVertexMaskForgeWorkingMesh NewWorkingMesh = VertexMaskForgePanel::BuildWorkingMeshForStaticMesh(Mesh, Entry->Diagnostics);
		Entry->bUseSourceTopology = VertexMaskForgePanel::ShouldUseSourceTopology(Mesh);

		// M16-J.0B: the geometry itself is still produced by the exact same, unmodified builder call
		// above -- MeshOwner only ever authenticates/owns the RESULT, once per entry (never per
		// component), via its own real generation/Provenance lifecycle (see InstallWorkingMesh's own doc
		// comment). ExpectedCardinality mirrors the real per-sample domain (LOD0 render-vertex count, or
		// Mesh.TriangleCount()*3 for Source-Topology) -- never fabricated, never treated as identity
		// proof by the validator.
		int32 ExpectedCardinality = 0;
		if (NewWorkingMesh.State == EVertexMaskForgeWorkingMeshState::Ready)
		{
			if (Entry->bUseSourceTopology && NewWorkingMesh.Mesh.IsValid())
			{
				ExpectedCardinality = NewWorkingMesh.Mesh->TriangleCount() * 3;
			}
			else if (!Entry->bUseSourceTopology && IsValid(Mesh) && Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.IsValidIndex(0))
			{
				ExpectedCardinality = static_cast<int32>(Mesh->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices());
			}
		}

		Entry->MeshOwner->ConfigureIdentity(Mesh, /*LODIndex=*/0, Entry->bUseSourceTopology);
		Entry->MeshOwner->InstallWorkingMesh(MoveTemp(NewWorkingMesh), ExpectedCardinality);

		if (Entry->MeshOwner->GetWorkingMesh().State == EVertexMaskForgeWorkingMeshState::Ready)
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

/**
 * AUDITED (raw/composition separation checkpoint): supersedes the old, single, blanket
 * InvalidateBoundingBoxMasks() -- that function invalidated BOTH slots for EVERY parameter change,
 * including purely compositional ones (Blend Mode, Opacity, Invert, Enable/Disable), which meant
 * changing e.g. AO Opacity would clear a perfectly valid, Ready AmbientOcclusionMask and flash the
 * preview to original colors -- exactly the contradiction this checkpoint's audit flagged. The fix is
 * three functions with clear, disjoint responsibilities instead of one generic one:
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
 *     Tree rebuilds, and can never produce an original-color fallback for a layer that is still
 *     genuinely Ready.
 *
 * Neither InvalidateBoundingBoxRawMask() nor InvalidateAODerivedMask() calls UpdateAllPreviews() itself
 * -- both are always immediately followed, by their own caller, with a real live regeneration (either
 * an immediate RunAutoUpdatePreview() call or ScheduleAutoUpdatePreview()'s short debounce), which is
 * what actually recomputes and republishes the now-invalidated slot. An interim synchronous
 * UpdateAllPreviews() here would only flash the OTHER, unaffected slot's preview and, for Ambient
 * Occlusion, risk an unnecessary AOCache-destroying fallback in between.
 */
void SVertexMaskForgePanel::InvalidateBoundingBoxRawMask()
{
	LastMaskActionStatusText = FText::GetEmpty();

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			Entry->GeneratorState.BoundingBoxMask = FVertexMaskForgeScalarMask();
		}
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
			Entry->GeneratorState.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
		}
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
		if (Entry.IsValid() && Entry->MeshOwner->GetWorkingMesh().State == EVertexMaskForgeWorkingMeshState::Ready)
		{
			return true;
		}
	}
	return false;
}

void SVertexMaskForgePanel::RunConstantFill(
	const float ConstantValue, const EVertexMaskForgeScalarMaskSource Source, const FText& SuccessMessage)
{
	// A pending live-update debounce must never overwrite this explicit Fill moments later.
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
		const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();

		// Same entry-level validity gating as live generation -- but a failure
		// here leaves the entry's existing mask COMPLETELY UNTOUCHED (preserve the last valid
		// Preview), rather than resetting it to Unavailable.
		if (WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready)
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
			? VertexMaskForgePanel::GenerateConstantMaskForCornerDomain(WorkingMesh.Mesh->TriangleCount() * 3, ConstantValue, Source)
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

		Entry->GeneratorState.BoundingBoxMask = MoveTemp(NewMask);
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
	// exactly like a fresh generation, so a later toggle in another channel can never erase it.
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
	// existing result is always stale. Ambient Occlusion is untouched (Unified Bounds has no effect
	// on it).
	InvalidateBoundingBoxRawMask();

	// Immediate, coherent batch regeneration -- RunAutoUpdatePreview() itself computes the (possibly
	// newly Individual or newly Unified) domain ONCE and reuses it for every eligible entry, never
	// recomputing just one mesh in isolation.
	RunAutoUpdatePreview();
}

void SVertexMaskForgePanel::ScheduleAutoUpdatePreview()
{
	if (!GEditor)
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

	// AUDITED (composition-stack checkpoint): Bounding Box and Ambient Occlusion (and every other
	// generator) are independent, optional stack layers -- only "nothing enabled at all" is blocked.
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
	if (!bAnyAxisEnabled && !(bIncludeAO && bAOEnabled) && !bCurvatureEnabled && !bNoiseEnabled && !bMaterialSlotMaskEnabled && !bDirectionalNormalMaskEnabled && !bThicknessMaskEnabled)
	{
		// Preserve every entry's existing masks untouched and surface the specific message -- do not
		// fall through to the generic "could not be regenerated" wording below.
		LastOperationErrorText = LOCTEXT("NoMaskSourceEnabledAutoUpdate", "Enable at least one Bounding Box axis, Ambient Occlusion, Curvature, Directional Normal, Noise, Thickness, or Material Slot Mask.");
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
		if (!VertexMaskForgeBoundingBoxGenerator::ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/true, CollectiveBounds, CollectiveError))
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

	// AUDITED (Noise V1): computed once per batch, before touching any entry's mask -- Noise's raw
	// pattern generation only needs the CURRENT UI values, never per-entry state.
	FVertexMaskForgeNoiseGenerativeParams NoiseGenerativeParams;
	NoiseGenerativeParams.NoiseType = NoiseType;
	NoiseGenerativeParams.ScaleX = NoiseScaleX;
	NoiseGenerativeParams.ScaleY = NoiseScaleY;
	NoiseGenerativeParams.ScaleZ = NoiseScaleZ;
	NoiseGenerativeParams.OffsetX = NoiseOffsetX;
	NoiseGenerativeParams.OffsetY = NoiseOffsetY;
	NoiseGenerativeParams.OffsetZ = NoiseOffsetZ;
	NoiseGenerativeParams.Seed = NoiseSeed;
	NoiseGenerativeParams.Octaves = NoiseOctaves;
	NoiseGenerativeParams.Roughness = NoiseRoughness;
	NoiseGenerativeParams.Lacunarity = NoiseLacunarity;
	NoiseGenerativeParams.TurbulenceStrength = NoiseTurbulenceStrength;
	NoiseGenerativeParams.Blur = NoiseBlur;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid() || Entry->MeshOwner->GetWorkingMesh().State != EVertexMaskForgeWorkingMeshState::Ready)
		{
			continue;
		}
		// M16-J.0B.1 corrective pass: local, short-lived const view -- see
		// FVertexMaskForgeSelectedMesh::MeshOwner's own doc comment on why this is never cached in a
		// persistent member. Valid for this entire loop iteration since Entry (and therefore its
		// MeshOwner) is held alive by the range-for's own reference to the array element.
		const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();

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
		// AUDITED (Curvature layer): only needed for the render-vertex correspondence build (non-Source-
		// Topology entries) inside GenerateCurvatureMask -- resolved here, once, alongside RenderData.
		const FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);

		FTransform ReferenceTransform = FTransform::Identity;
		bool bHasLiveComponent = false;
		for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
		{
			if (const UStaticMeshComponent* SourceComponent = StateOwner->GetPreviewState().GetSourceComponent().Get())
			{
				ReferenceTransform = SourceComponent->GetComponentTransform();
				bHasLiveComponent = true;
				break;
			}
		}

		// Bounding Box slot: unchanged "auto-update never replaces a valid Preview with
		// incomplete/degenerate data" contract.
		//
		// AUDITED (Nanite source-topology support): Source-Topology entries always use individual
		// bounds (bSkipBoundingBoxThisPass/CollectiveBoundsPtr are Unified-Bounds-only concerns and do not
		// apply to this branch at all).
		if (bAnyAxisEnabled && !bSkipBoundingBoxThisPass)
		{
			FVertexMaskForgeScalarMask NewBBoxMask = Entry->bUseSourceTopology
				? VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
					*WorkingMesh.Mesh, BoundingBoxAxisParams, ReferenceTransform)
				: VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMask(
					RenderData->LODResources[0], BoundingBoxAxisParams, ReferenceTransform, CollectiveBoundsPtr);
			NewBBoxMask.SelectionMeshCount = SelectedMeshes.Num();

			if (NewBBoxMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				Entry->GeneratorState.BoundingBoxMask = MoveTemp(NewBBoxMask);
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
			Entry->GeneratorState.BoundingBoxMask = FVertexMaskForgeScalarMask();
		}

		// AUDITED (double-AO-execution fix): Ambient Occlusion slot -- ENTRY-LEVEL VALIDATION ONLY,
		// never a real computation here -- see FVertexMaskForgeWorkingMesh::AmbientOcclusionMask's own
		// doc comment. Always snapshots the full, user-chosen AOSamples (the UI value itself is never
		// mutated, only read).
		//
		// AUDITED (BBox Invert exception): bIncludeAO false (OnAxisInvertChanged's scoped call) skips
		// this ENTIRE block -- AmbientOcclusionMask is not read, not cleared, not re-validated, not
		// touched in any way, so a BBox Invert change can never have any observable effect on AO.
		if (bIncludeAO)
		{
			if (bAOEnabled)
			{
				Entry->GeneratorState.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
				Entry->GeneratorState.AmbientOcclusionMask.Source = EVertexMaskForgeScalarMaskSource::AmbientOcclusion;
				const bool bAOInputValid = Entry->bUseSourceTopology
					? VertexMaskForgeAmbientOcclusionGenerator::IsAmbientOcclusionInputValidForDynamicMesh(WorkingMesh.Mesh.Get())
					: VertexMaskForgeAmbientOcclusionGenerator::IsAmbientOcclusionInputValid(RenderData->LODResources[0]);
				if (bHasLiveComponent && bAOInputValid)
				{
					FVertexMaskForgeAOParams Params;
					Params.Samples = FMath::Clamp(AOSamples, 8, 256);
					Params.MaxDistance = FMath::Clamp(AOMaxDistance, 0.01f, 10000.0f);
					Params.Bias = FMath::Clamp(AOBias, 0.001f, 10.0f);
					Params.bInvert = bAOInvert;
					Params.LevelsMin = AOLevelsMin;
					Params.LevelsMax = AOLevelsMax;
					Entry->GeneratorState.AmbientOcclusionMask.UsedAOParams = Params;
					Entry->GeneratorState.AmbientOcclusionMask.RenderVertexCount = Entry->bUseSourceTopology
						? WorkingMesh.Mesh->VertexCount()
						: static_cast<int32>(RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices());
					Entry->GeneratorState.AmbientOcclusionMask.State = EVertexMaskForgeScalarMaskState::Ready;
				}
				else
				{
					Entry->GeneratorState.AmbientOcclusionMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
				}
			}
			else
			{
				Entry->GeneratorState.AmbientOcclusionMask = FVertexMaskForgeScalarMask();
			}
		}

		// AUDITED (Curvature layer): a real, entry-level computation, cache-reusing (GenerateCurvatureMask/
		// GenerateCurvatureMaskFromDynamicMesh internally reuse the cached raw analysis whenever
		// GeometryFingerprint still matches, so repeated live regeneration passes after the first never
		// redo the expensive adjacency/dihedral pass). Never gated by bIncludeAO: that flag's own
		// contract (see the AO block's doc comment above) is
		// narrowly about shielding Ambient Occlusion from BBox Invert's immediate-regeneration exception,
		// the same way Bounding Box's own regeneration above is never gated by it either.
		if (bCurvatureEnabled)
		{
			FVertexMaskForgeScalarMask NewCurvatureMask = Entry->bUseSourceTopology
				? VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
					WorkingMesh, Entry->GeneratorState, CurvatureType, CurvatureMultiplier, CurvatureBlur, CurvatureLevelsMin, CurvatureLevelsMax, bCurvatureInvert)
				: VertexMaskForgeCurvatureGenerator::GenerateCurvatureMask(
					WorkingMesh, Entry->GeneratorState, MeshDescription, RenderData->LODResources[0],
					CurvatureType, CurvatureMultiplier, CurvatureBlur, CurvatureLevelsMin, CurvatureLevelsMax, bCurvatureInvert);
			if (NewCurvatureMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				Entry->GeneratorState.CurvatureMask = MoveTemp(NewCurvatureMask);
			}
			// else: preserve whatever CurvatureMask this entry already had -- same "auto-update never
			// replaces a valid Preview with incomplete/degenerate data" contract as Bounding Box above.
		}
		else
		{
			Entry->GeneratorState.CurvatureMask = FVertexMaskForgeScalarMask();
		}

		// AUDITED (Noise V1): same "real, entry-level computation, cache-reusing" contract as the
		// Curvature block above -- see that block's own doc comment. Never gated by bIncludeAO either
		// (same rationale as Curvature).
		if (bNoiseEnabled)
		{
			FVertexMaskForgeScalarMask NewNoiseMask = Entry->bUseSourceTopology
				? VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
					WorkingMesh, Entry->GeneratorState, NoiseGenerativeParams, NoiseMultiplier, NoiseLevelsMin, NoiseLevelsMax, bNoiseInvert)
				: VertexMaskForgeNoiseGenerator::GenerateNoiseMask(
					WorkingMesh, Entry->GeneratorState, RenderData->LODResources[0],
					NoiseGenerativeParams, NoiseMultiplier, NoiseLevelsMin, NoiseLevelsMax, bNoiseInvert);
			if (NewNoiseMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				Entry->GeneratorState.NoiseMask = MoveTemp(NewNoiseMask);
			}
			// else: preserve whatever NoiseMask this entry already had -- same "auto-update never
			// replaces a valid Preview with incomplete/degenerate data" contract as Bounding Box/Curvature.
		}
		else
		{
			Entry->GeneratorState.NoiseMask = FVertexMaskForgeScalarMask();
		}

		// AUDITED (V2-D): same "real, entry-level computation" contract as Curvature/Noise above, but
		// only when exactly one mesh is selected (V1 scope) -- see IsMaterialSlotMaskAvailableForSelection.
		// Never gated by bIncludeAO (same rationale as Curvature/Noise).
		if (bMaterialSlotMaskEnabled && IsMaterialSlotMaskAvailableForSelection())
		{
			FVertexMaskForgeScalarMask NewMaterialSlotMask = Entry->bUseSourceTopology
				? VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
					WorkingMesh, SelectedMaterialSlotIndex, bMaterialSlotMaskInvert)
				: VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMask(
					WorkingMesh, RenderData->LODResources[0], SelectedMaterialSlotIndex, bMaterialSlotMaskInvert);
			if (NewMaterialSlotMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				Entry->GeneratorState.MaterialSlotMask = MoveTemp(NewMaterialSlotMask);
			}
			// else: preserve whatever MaterialSlotMask this entry already had -- same "auto-update never
			// replaces a valid Preview with incomplete/degenerate data" contract as the other generators.
		}
		else
		{
			Entry->GeneratorState.MaterialSlotMask = FVertexMaskForgeScalarMask();
		}

		// AUDITED (V2-E): entry-level reference, cheap enough to just recompute every pass. Never
		// gated by bIncludeAO (same rationale as Curvature/Noise/Material Slot).
		if (bDirectionalNormalMaskEnabled)
		{
			FVertexMaskForgeScalarMask NewDirectionalNormalMask = Entry->bUseSourceTopology
				? VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
					WorkingMesh, DirectionalNormalSpace, DirectionalNormalDirection,
					DirectionalNormalAngle, DirectionalNormalFalloff, DirectionalNormalBlur, bDirectionalNormalMaskInvert, ReferenceTransform)
				: VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMask(
					RenderData->LODResources[0], DirectionalNormalSpace, DirectionalNormalDirection,
					DirectionalNormalAngle, DirectionalNormalFalloff, DirectionalNormalBlur, bDirectionalNormalMaskInvert, ReferenceTransform);

			Entry->GeneratorState.bDirectionalNormalWorldSpaceConflict = false;
			if (DirectionalNormalSpace == EVertexMaskForgeNormalSpace::World)
			{
				float MaxDeviationDegrees = 0.0f;
				Entry->GeneratorState.bDirectionalNormalWorldSpaceConflict =
					VertexMaskForgeWorkingMeshTypes::HasConflictingWorldSpaceNormalTransforms(Entry->PreviewComponents, MaxDeviationDegrees);
			}

			if (NewDirectionalNormalMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				Entry->GeneratorState.DirectionalNormalMask = MoveTemp(NewDirectionalNormalMask);
			}
			// else: preserve whatever DirectionalNormalMask this entry already had -- same "auto-update
			// never replaces a valid Preview with incomplete/degenerate data" contract as the others.
		}
		else
		{
			Entry->GeneratorState.DirectionalNormalMask = FVertexMaskForgeScalarMask();
			Entry->GeneratorState.bDirectionalNormalWorldSpaceConflict = false;
		}

		// AUDITED (V2-G): entry-level reference, cheap enough to just recompute every pass -- Asset
		// Local Space, never gated by any transform. "Last valid preview kept on failure" -- same
		// contract as Directional Normal
		// above (a transient Unavailable/no-hit result during a drag never replaces a valid preview).
		if (bThicknessMaskEnabled)
		{
			FVertexMaskForgeScalarMask NewThicknessMask = Entry->bUseSourceTopology
				? VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
					Entry->GeneratorState.SourceTopologyThicknessCache, WorkingMesh,
					ThicknessMinThickness, ThicknessMaxThickness, ThicknessSearchDistance, ThicknessBias, ThicknessBlur, bThicknessMaskInvert)
				: VertexMaskForgeThicknessGenerator::GenerateThicknessMask(
					Entry->GeneratorState.ThicknessCache, Mesh, RenderData->LODResources[0],
					ThicknessMinThickness, ThicknessMaxThickness, ThicknessSearchDistance, ThicknessBias, ThicknessBlur, bThicknessMaskInvert);
			if (NewThicknessMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				Entry->GeneratorState.ThicknessMask = MoveTemp(NewThicknessMask);
			}
		}
		else
		{
			Entry->GeneratorState.ThicknessMask = FVertexMaskForgeScalarMask();
		}
	}

	if (NumFailed > 0)
	{
		LastOperationErrorText = FText::Format(
			LOCTEXT("AutoUpdateFailedFormat",
				"{0} mesh(es) could not be regenerated with the current parameters (e.g. '{1}') -- kept the last valid Preview for those."),
			FText::AsNumber(NumFailed), FText::FromString(FirstFailedAssetName));
	}

	// Recomposes/reapplies from whichever mask each entry ended up with (freshly regenerated, or the
	// preserved previous one), and recomputes OperationState -- but never touches
	// LastOperationErrorText (see the comment at the top of this function). bCommit=false: live
	// regeneration is a transient recomposition -- it must never silently consolidate WorkingColors
	// into CommittedColors; only an explicit Fill White/Black action does.
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
		if (Entry->GeneratorState.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready
			|| Entry->GeneratorState.AmbientOcclusionMask.State == EVertexMaskForgeScalarMaskState::Ready
			|| Entry->GeneratorState.CurvatureMask.State == EVertexMaskForgeScalarMaskState::Ready
			|| Entry->GeneratorState.NoiseMask.State == EVertexMaskForgeScalarMaskState::Ready
			|| Entry->GeneratorState.MaterialSlotMask.State == EVertexMaskForgeScalarMaskState::Ready
			|| Entry->GeneratorState.DirectionalNormalMask.State == EVertexMaskForgeScalarMaskState::Ready
			|| Entry->GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Ready)
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
		return LOCTEXT("PreviewStatusMaskNotReady", "Preview: Mask Not Ready — enable a generator or adjust its parameters");
	}
	if (bAnyMaskNotReady)
	{
		return LOCTEXT("PreviewStatusMixed", "Preview: Active (some selected meshes: Mask Not Ready)");
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
 * the Transaction Buffer finishes restoring/reapplying them.
 *
 * AUDITED (UX1, explicit Edit Vertex Mask session entry): gated on bIsEditingVertexMask, not
 * OperationState, for the same reason as OnEditorSelectionChanged (a session can be Editing with
 * OperationState still Idle -- see bIsEditingVertexMask's own doc comment). Previously this called
 * RefreshSelection() directly whenever OperationState == Idle, which meant an unrelated Undo/Redo
 * elsewhere in the Editor could silently START an editing session while the panel was merely Idle/
 * Selection -- exactly the automatic-entry problem UX1 removes. Now:
 *   - NOT editing (bIsEditingVertexMask false): only resyncs the candidate list
 *     (RefreshCandidateSelection()) -- it only READS the current state of the selected components/
 *     assets to refresh diagnostics; never builds a WorkingMesh, never creates a PreviewComponent,
 *     never starts a session.
 *   - Editing (bIsEditingVertexMask true, an active session against its own captured targets exists):
 *     never disrupt it just because SOME unrelated Undo/Redo fired elsewhere in the Editor -- same non-
 *     disruptive policy OnEditorSelectionChanged already uses for a scene selection change during an
 *     active session (see its own audit note). Instead, this defers the resync via the SAME
 *     bSceneSelectionChangedDuringActiveOperation flag / SyncSelectionIfChangedDuringOperation()
 *     mechanism already used for that case: once the user concludes the CURRENT session through their
 *     own Accept or Cancel, the panel automatically re-syncs its candidate list -- never silently
 *     mid-session, and never automatically starting another session.
 */
void SVertexMaskForgePanel::HandlePostUndoRedo(bool bSuccess, bool bIsRedo)
{
	if (!bSuccess)
	{
		return;
	}

	const bool bResyncedImmediately = !bIsEditingVertexMask;
	if (bResyncedImmediately)
	{
		RefreshCandidateSelection();
	}
	else
	{
		bSceneSelectionChangedDuringActiveOperation = true;
	}

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Post%s (Editing=%s) -- %s."),
		bIsRedo ? TEXT("Redo") : TEXT("Undo"),
		bIsEditingVertexMask ? TEXT("true") : TEXT("false"),
		bResyncedImmediately ? TEXT("candidate list resynced immediately") : TEXT("resync deferred until the active session concludes"));
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
			&& (Entry->GeneratorState.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready
				|| Entry->GeneratorState.AmbientOcclusionMask.State == EVertexMaskForgeScalarMaskState::Ready
				|| Entry->GeneratorState.CurvatureMask.State == EVertexMaskForgeScalarMaskState::Ready
				|| Entry->GeneratorState.NoiseMask.State == EVertexMaskForgeScalarMaskState::Ready
				|| Entry->GeneratorState.MaterialSlotMask.State == EVertexMaskForgeScalarMaskState::Ready
				|| Entry->GeneratorState.DirectionalNormalMask.State == EVertexMaskForgeScalarMaskState::Ready
				|| Entry->GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Ready))
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
	// AUDITED (full session termination): Cancel ends the CURRENT session entirely, not just the
	// transient Preview. The Unreal Editor's own selection (GetSelectedActors()) is never touched --
	// only this panel's OWN SelectedMeshes array and derived session state are cleared.
	//
	// AUDITED (UX1, explicit Edit Vertex Mask session entry): SelectedMeshes is now populated ONLY by
	// RefreshSelection(), which itself is called ONLY from OnEditVertexMaskClicked() -- so, unlike
	// before, ending the session here does NOT by itself allow a new one to start automatically.
	// bIsEditingVertexMask = false (below) re-enables the "Edit Vertex Mask" button once a valid
	// candidate exists again; the user must click it explicitly for a new session to begin.

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

	// AUDITED (UX1): the session itself has now ended -- return to Idle/Selection. Must happen AFTER
	// SelectedMeshes is fully discarded (above) and BEFORE SyncSelectionIfChangedDuringOperation()
	// below (which re-derives CandidateMeshes and therefore CanEditVertexMask()).
	bIsEditingVertexMask = false;

	// 11. Deferred sync: if the scene selection changed while this (now-cancelled) session was
	// active, catch up the CANDIDATE list with the CURRENT scene selection now -- bIsEditingVertexMask
	// is false at this point, and the cancelled session's original targets have already been fully
	// discarded (steps 3-9), so this can never retarget or interrupt anything. Never starts a new
	// session automatically (see SyncSelectionIfChangedDuringOperation's own doc comment). No-ops if
	// the selection never changed during the session.
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

	// AUDITED (V2-E corrective pass, transform freshness): Directional Normal Mask's World Space per-
	// component result is only ever recomputed live inside ApplyPreviewToEntry -- if the user moved a
	// component's Actor faster than the debounced live regeneration could catch up,
	// WorkingColors could still reflect a STALE transform at the moment Accept is
	// clicked. A synchronous, non-committing recomposition pass HERE (the exact same call every other
	// live-recompose path already uses) guarantees WorkingColors reflects the CURRENT live component
	// transform(s) before a single byte is read for persistence -- reusing 100% existing, already-
	// audited machinery (ApplyPreviewToEntry recomputes Directional Normal Mask fresh per component in
	// World Space every single call; Local Space and every other generator are entirely unaffected,
	// since this never invalidates any raw cache, only recomposes from what is already cached).
	if (bDirectionalNormalMaskEnabled && DirectionalNormalSpace == EVertexMaskForgeNormalSpace::World)
	{
		UpdateAllPreviews(/*bCommit=*/false);
	}

	// AUDITED (Nanite source-topology support): preflight BOTH domains BEFORE writing either one --
	// if EITHER fails, nothing is modified (the whole point of validating fully before the first
	// Modify() call). BuildAcceptTargets/BuildSourceTopologyAcceptTargets never produce overlapping
	// targets for the same asset (an entry is exclusively one domain or the other -- see
	// FVertexMaskForgeSelectedMesh::bUseSourceTopology), so there is no cross-domain collision to
	// reconcile, only a combined "nothing eligible at all" / "confirm N assets total" presentation.
	TArray<VertexMaskForgeAcceptTargetBuilder::FAcceptTarget> Targets;
	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> SourceTopologyTargets;
	FText ErrorText;
	if (!VertexMaskForgeAcceptTargetBuilder::BuildAcceptTargets(SelectedMeshes, bDirectionalNormalMaskEnabled, DirectionalNormalSpace, Targets, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Warning, TEXT("Vertex Mask Forge: Accept blocked: %s"), *ErrorText.ToString());
		return false;
	}
	if (!VertexMaskForgeAcceptTargetBuilder::BuildSourceTopologyAcceptTargets(SelectedMeshes, bDirectionalNormalMaskEnabled, DirectionalNormalSpace, SourceTopologyTargets, ErrorText))
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
	for (const VertexMaskForgeAcceptTargetBuilder::FAcceptTarget& Target : Targets)
	{
		AssetNames.Add(Target.AssetName);
	}
	for (const VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget& Target : SourceTopologyTargets)
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

		if (!Targets.IsEmpty() && !VertexMaskForgeAcceptWriter::WriteAcceptTargets(Targets, ModifiedMeshes, ErrorText))
		{
			OperationState = EVertexMaskForgeOperationState::Failed;
			LastOperationErrorText = ErrorText;
			UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept failed while writing: %s"), *ErrorText.ToString());
			return false;
		}
		if (!SourceTopologyTargets.IsEmpty() && !VertexMaskForgeAcceptWriter::WriteSourceTopologyAcceptTargets(SourceTopologyTargets, ModifiedMeshes, ErrorText))
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

	if (!VertexMaskForgeAcceptWriter::BuildModifiedMeshes(ModifiedMeshes, ErrorText))
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

	// AUDITED (UX1, explicit Edit Vertex Mask session entry): the session has now concluded --
	// discard its targets (mirrors OnCancelChangesClicked's own SelectedMeshes.Empty(), so Idle-state
	// status text/CanRunFill/etc. never keep referencing an already-persisted session) and return to
	// Idle/Selection. SelectedMeshes is only ever repopulated by the next explicit "Edit Vertex Mask"
	// click (RefreshSelection(), via OnEditVertexMaskClicked()).
	SelectedMeshes.Empty();
	bIsEditingVertexMask = false;

	OperationState = EVertexMaskForgeOperationState::Idle;
	LastOperationErrorText = FText::GetEmpty();

	// Deferred sync: the write above already completed against the ORIGINAL SelectedMeshes/Targets
	// captured before this call; only now, with the session concluded (bIsEditingVertexMask false), is
	// it safe to catch up CandidateMeshes with a scene selection that may have changed while the
	// session was active. Never starts a new session automatically.
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
	for (TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry.PreviewComponents)
	{
		VertexMaskForgePanel::RestoreComponentOriginal(*StateOwner, ActorHideStates);
	}
}

void SVertexMaskForgePanel::RestorePreviewForEntryVisualOnly(FVertexMaskForgeSelectedMesh& Entry)
{
	for (TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry.PreviewComponents)
	{
		VertexMaskForgePanel::RestorePreviewVisualOnly(StateOwner->GetVisualSessionStateMutable(), ActorHideStates);
	}
}

TOptional<EVertexMaskForgeWorkingColorsPublicationValidationStatus> SVertexMaskForgePanel::ValidatePublicationBindingForComponent(const UStaticMeshComponent* Component) const
{
	if (!IsValid(Component))
	{
		return TOptional<EVertexMaskForgeWorkingColorsPublicationValidationStatus>();
	}

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
		{
			if (StateOwner->GetPreviewState().GetSourceComponent().Get() == Component)
			{
				return StateOwner->ValidateBinding();
			}
		}
	}

	return TOptional<EVertexMaskForgeWorkingColorsPublicationValidationStatus>();
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

	// M16-J.0B.1 corrective pass: local, short-lived const view (no persistent WorkingMesh reference member
	// exists on FVertexMaskForgeSelectedMesh anymore -- see that member's own removed doc comment). Valid
	// for this whole function call since Entry (held by the caller) keeps MeshOwner alive throughout.
	const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();
	// M16-J.0B.1 (WorkingMesh Domain Split): Generator Artistic State (BoundingBoxMask/AmbientOcclusionMask/
	// CurvatureMask/NoiseMask/MaterialSlotMask/DirectionalNormalMask/ThicknessMask and their raw caches) no
	// longer lives on WorkingMesh -- this is the one function that reads both the const Identity view
	// (WorkingMesh, e.g. WorkingMesh.Mesh below) and the mutable Artistic State in the same expressions, so
	// both references are declared side by side here (see FVertexMaskForgeGeneratorState's own module
	// comment for why these fields moved).
	FVertexMaskForgeGeneratorState& GeneratorState = Entry->GeneratorState;
	// AUDITED (composition-stack checkpoint): proceed if EITHER slot is Ready -- Bounding Box and
	// Ambient Occlusion are independent, optional layers now (see BoundingBoxMask's own doc comment).
	const bool bBBoxEntryReady = GeneratorState.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready;
	// AUDITED (raw/composition separation checkpoint): gated LIVE on bAOEnabled, not just State --
	// disabling AO (OnAOEnableChanged, pure composition) never touches AmbientOcclusionMask/AOCache at
	// all (see that handler's own doc comment), so State can legitimately still read Ready while the
	// layer is meant to be excluded from the stack; bAOEnabled is the live, authoritative "is this
	// layer currently supposed to participate" signal.
	const bool bAOEntryReady = bAOEnabled && GeneratorState.AmbientOcclusionMask.State == EVertexMaskForgeScalarMaskState::Ready;
	// AUDITED (Curvature layer): same live-gating rationale as bAOEntryReady above, and same
	// "entry-level result IS the per-component contribution" property as bBBoxEntryReady when no
	// per-component re-evaluation is needed (Curvature never needs one -- see CurvatureMask's own doc
	// comment) -- GeneratorState.CurvatureMask.State can legitimately still read Ready while disabled.
	const bool bCurvatureEntryReady = bCurvatureEnabled && GeneratorState.CurvatureMask.State == EVertexMaskForgeScalarMaskState::Ready;
	// AUDITED (Noise V1): same live-gating rationale as bCurvatureEntryReady above -- Noise is also
	// entry-level, real values, no per-component re-evaluation needed.
	const bool bNoiseEntryReady = bNoiseEnabled && GeneratorState.NoiseMask.State == EVertexMaskForgeScalarMaskState::Ready;
	// AUDITED (V2-D): same live-gating rationale as bCurvatureEntryReady/bNoiseEntryReady above --
	// Material Slot Mask is also entry-level, real values, no per-component re-evaluation needed.
	const bool bMaterialSlotMaskEntryReady = bMaterialSlotMaskEnabled && GeneratorState.MaterialSlotMask.State == EVertexMaskForgeScalarMaskState::Ready;
	// AUDITED (V2-E): same live-gating rationale as bAOEntryReady above -- in World Space,
	// GeneratorState.DirectionalNormalMask.State is VALIDATION ONLY (same contract as AmbientOcclusionMask),
	// the real per-component result is computed fresh below; in Local Space it holds the real, final
	// entry-level values directly, like Curvature/Noise/Material Slot.
	const bool bDirectionalNormalMaskEntryReady = bDirectionalNormalMaskEnabled && GeneratorState.DirectionalNormalMask.State == EVertexMaskForgeScalarMaskState::Ready;
	// AUDITED (V2-G): Thickness is ALWAYS Asset Local Space -- same "no per-component re-evaluation"
	// contract as Curvature/Noise/Material Slot (never like Directional Normal World Space/AO).
	const bool bThicknessMaskEntryReady = bThicknessMaskEnabled && GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Ready;
	if (WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready
		|| !WorkingMesh.Mesh.IsValid())
	{
		// Nothing safe to preview yet (working mesh itself invalid/not ready): show the original
		// colors/materials rather than a stale or fabricated result. AUDITED (raw/composition
		// separation checkpoint): visual-only -- not necessarily a geometric invalidation, so AOCache
		// must not be destroyed speculatively.
		//
		// AUDITED (preview-stuck-on-Original-Material fix): "zero masks currently enabled/Ready" is
		// deliberately NOT part of this gate -- an empty Layers set is a valid, fully-supported input
		// to UpdateWorkingColors/UpdateWorkingColorsSourceTopology (see ComposeStack's own
		// bOutAnyLayerContributed=false/CommittedColor-passthrough branch): it simply yields the
		// current Baseline/Committed working colors, exactly what RGB/Channel Preview Modes must show
		// when no mask is active. Bailing out here unconditionally on "no mask ready" used to skip the
		// entire per-component composition + material-selection path below for EVERY Preview Mode,
		// leaving the viewport visually stuck on the real component's original materials regardless of
		// CurrentPreviewMode. The per-component loop below now only falls back to the real component's
		// appearance on a genuine per-component evaluation failure (bAnyLayerFailed) -- an empty Layers
		// set alone proceeds through composition/material-selection like any other case.
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
	for (TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
	{
		// M16-J.0B (2nd rejection-corrective pass): the real composition MATH below (ComputeComposedColorsRGB
		// et al.) is unchanged -- only the storage State refers to has moved, from an array element
		// directly embedded in FVertexMaskForgeSelectedMesh to StateOwner's own internal
		// PreviewComponentState. GetVisualSessionStateMutable() only grants access to the panel-owned
		// visual/session fields this loop needs directly (AOCache, PreviewComponent, ...) plus read-only
		// access to SourceComponent via its own getter -- BaselineColors/CommittedColors/WorkingColors are
		// reached separately below, via StateOwner's own const getters (for reads) and
		// EnsureBaselineCaptured/ApplyComposedColorsRGB (for the two authorized writes), never through
		// State itself (those fields are private on the struct now, unreachable through this reference).
		FVertexMaskForgePreviewComponentState& State = StateOwner->GetVisualSessionStateMutable();
		UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
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
		//   - Fill/Constant override (GeneratorState.BoundingBoxMask.Source == ConstantWhite/Black): a
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
		FVertexMaskForgeScalarMask PerComponentDirectionalNormalMask;
		bool bAnyLayerFailed = false;

		// AUDITED (Nanite source-topology support): the two domains diverge starting here -- Source-
		// Topology entries never read RenderData/LODResources[0] at all (that would be the reduced
		// Nanite fallback), and use the FDynamicMesh3-based generators/caches/preview instead. See
		// FVertexMaskForgeSelectedMesh::bUseSourceTopology's own doc comment for the domain-selection
		// rule.
		if (Entry->bUseSourceTopology)
		{
			if (GeneratorState.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantWhite
				|| GeneratorState.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantBlack)
			{
				// GenerateConstantMaskForCornerDomain (see RunConstantFill) already built this mask in
				// the corner domain for a Source-Topology entry -- IndexOverride's default (-1) resolves
				// to the shared CornerIndex UpdateWorkingColorsSourceTopology passes as VertexIndex,
				// exactly like the render-vertex Fill path does with render vertex index.
				Layers.Add({ &GeneratorState.BoundingBoxMask, EVertexMaskForgeBlendMode::Copy, 1.0f });
			}
			else
			{
				if (bBBoxEntryReady)
				{
					PerComponentBBoxMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
						*WorkingMesh.Mesh, GeneratorState.BoundingBoxMask.UsedAxisParams, SourceComponent->GetComponentTransform());
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
					FVertexMaskForgeAOParams EffectiveAOParams = GeneratorState.AmbientOcclusionMask.UsedAOParams;
					EffectiveAOParams.bInvert = bAOInvert;
					EffectiveAOParams.LevelsMin = AOLevelsMin;
					EffectiveAOParams.LevelsMax = AOLevelsMax;
					PerComponentAOMask = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMaskFromDynamicMesh(
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
				// AUDITED (Curvature layer): NO per-component re-evaluation, unlike Bounding Box/Ambient
				// Occlusion above -- GeneratorState.CurvatureMask already holds the REAL, final values for
				// every component of this entry (see its own doc comment), so this simply adds the
				// entry-level mask directly. IndexOverride is resolved per-corner by
				// UpdateWorkingColorsSourceTopology's own switch (Curvature -> Dynamic Mesh Vertex ID).
				if (!bAnyLayerFailed && bCurvatureEntryReady)
				{
					Layers.Add({ &GeneratorState.CurvatureMask, CurvatureBlendMode, CurvatureOpacity });
				}
				// AUDITED (Noise V1): same "no per-component re-evaluation" contract as Curvature above --
				// GeneratorState.NoiseMask already holds the REAL, final values for every component of this
				// entry. Composed AFTER Curvature, per the explicit ordering requirement (Bounding Box ->
				// Ambient Occlusion -> Curvature -> Noise) -- though see ComposeMaskStack's own doc
				// comment: canonical composition order is actually determined by each layer's OWN Blend
				// Mode, not by Layers.Add() call order, which only matters for documentation clarity here.
				if (!bAnyLayerFailed && bNoiseEntryReady)
				{
					Layers.Add({ &GeneratorState.NoiseMask, NoiseBlendMode, NoiseOpacity });
				}
				// AUDITED (V2-D): same "no per-component re-evaluation" contract as Curvature/Noise above
				// -- GeneratorState.MaterialSlotMask already holds the REAL, final, CORNER-EXACT values (see
				// its own doc comment) for every component of this entry. IndexOverride is resolved
				// per-corner by UpdateWorkingColorsSourceTopology's own switch (MaterialSlot -> CornerIndex,
				// NOT Dynamic Mesh Vertex ID -- unlike Curvature/Noise, since two corners at the same
				// position on opposite sides of a material boundary must read different values).
				if (!bAnyLayerFailed && bMaterialSlotMaskEntryReady)
				{
					Layers.Add({ &GeneratorState.MaterialSlotMask, MaterialSlotMaskBlendMode, MaterialSlotMaskOpacity });
				}
				// AUDITED (V2-E): Local Space is transform-independent -- GeneratorState.DirectionalNormalMask
				// already holds the REAL, final, CORNER-EXACT values (see its own doc comment), reused
				// directly like Curvature/Noise/Material Slot above. World Space is ALWAYS transform-
				// dependent (like Ambient Occlusion) -- re-evaluated fresh per component, using THIS
				// component's own transform, into a per-component-scoped local variable (never cached,
				// since it is cheap -- no raycasting, just a dot product per corner).
				if (!bAnyLayerFailed && bDirectionalNormalMaskEntryReady)
				{
					if (DirectionalNormalSpace == EVertexMaskForgeNormalSpace::Local)
					{
						Layers.Add({ &GeneratorState.DirectionalNormalMask, DirectionalNormalMaskBlendMode, DirectionalNormalMaskOpacity });
					}
					else
					{
						PerComponentDirectionalNormalMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
							WorkingMesh, DirectionalNormalSpace, DirectionalNormalDirection,
							DirectionalNormalAngle, DirectionalNormalFalloff, DirectionalNormalBlur, bDirectionalNormalMaskInvert,
							SourceComponent->GetComponentTransform());
						if (PerComponentDirectionalNormalMask.State != EVertexMaskForgeScalarMaskState::Ready)
						{
							bAnyLayerFailed = true;
						}
						else
						{
							Layers.Add({ &PerComponentDirectionalNormalMask, DirectionalNormalMaskBlendMode, DirectionalNormalMaskOpacity });
						}
					}
				}
				// AUDITED (V2-G): Thickness is ALWAYS Asset Local Space -- no per-component
				// re-evaluation, same "no per-component re-evaluation" contract as Curvature/Noise/
				// Material Slot above -- GeneratorState.ThicknessMask already holds the REAL, final,
				// CORNER-EXACT values for every component of this entry.
				if (!bAnyLayerFailed && bThicknessMaskEntryReady)
				{
					Layers.Add({ &GeneratorState.ThicknessMask, ThicknessMaskBlendMode, ThicknessMaskOpacity });
				}
			}

			// AUDITED (preview-stuck-on-Original-Material fix): Layers.IsEmpty() alone is no longer
			// part of this fallback -- see ApplyPreviewToEntry's entry-level gate doc comment above for
			// why an empty Layers set (no mask currently enabled) is a valid, fully-supported input to
			// UpdateWorkingColorsSourceTopology (it simply yields the current Baseline/Committed working
			// colors). Only a genuine per-component evaluation failure still falls back to the real
			// component's appearance here.
			if (bAnyLayerFailed)
			{
				VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
				continue;
			}

			// M16-J.0B (2nd rejection-corrective pass): Baseline/Committed/Working (Source-Topology
			// corner domain) live inside StateOwner as PRIVATE fields -- this call site never receives a
			// mutable reference/pointer into them. EnsureBaselineCaptured is idempotent (a no-op once
			// already initialized), so the (O(NumCorners)) capture below only actually runs once per
			// generation. The composition math itself (ComputeComposedColorsRGBSourceTopology) is
			// byte-for-byte the same algorithm as before this checkpoint; only the caller now computes a
			// fresh, independent result instead of writing into owner storage directly.
			if (!StateOwner->AreColorsInitialized())
			{
				StateOwner->EnsureBaselineCaptured(VertexMaskForgePanel::CaptureBaselineColorsSourceTopology(*WorkingMesh.Mesh));
			}

			TArray<FColor> FinalColors;
			int32 NumComposed = 0;
			VertexMaskForgePanel::ComputeComposedColorsRGBSourceTopology(
				StateOwner->GetBaselineColors(), StateOwner->GetCommittedColors(),
				Layers, GeneratorLayerOrder, *WorkingMesh.Mesh,
				bChannelFilterR, bChannelFilterG, bChannelFilterB,
				FinalColors, NumComposed);

			// AUDITED: this operation, not the caller, preserves Alpha byte-identical (forced from the
			// owner's own Baseline unconditionally), validates cardinality, decides whether Committed is
			// promoted (bCommit forwarded exactly as before -- ONLY a Fill Black/White action passes
			// true), and re-derives Authority as a direct consequence. See ApplyComposedColorsRGB's own
			// doc comment.
			if (!StateOwner->ApplyComposedColorsRGB(MoveTemp(FinalColors),
				bCommit ? FVertexMaskForgeWorkingStateOwner::EColorCommitMode::Consolidate : FVertexMaskForgeWorkingStateOwner::EColorCommitMode::PreviewOnly))
			{
				VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
				continue;
			}

			// M16-J.0B: real, production call site of the binding seam -- diagnostic only, never
			// consumed for any decision, never executes recipe/M16-H/M16-I/M16-J. Proves
			// CreateBinding()/ValidateBinding() work end-to-end against the panel's own real storage.
			if (const TOptional<EVertexMaskForgeWorkingColorsPublicationValidationStatus> BindingStatus = ValidatePublicationBindingForComponent(SourceComponent))
			{
				UE_LOG(LogVertexMaskForge, VeryVerbose,
					TEXT("Vertex Mask Forge: publication binding status for '%s' (Source Topology) = %d."),
					*SourceComponent->GetName(), static_cast<int32>(*BindingStatus));
			}

			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: composed %d/%d corner(s) for '%s' on component '%s' (Source Topology)."),
				NumComposed, WorkingMesh.Mesh->TriangleCount() * 3, *Entry->AssetName, *SourceComponent->GetName());

			// AUDITED (Preview Mode channel-isolation fix, Source Topology): reduces the raw composited
			// corner-domain colors through the current Preview Mode -- see DeriveDisplayColors' own doc
			// comment -- exactly mirroring the render-vertex path's RenderOrderColors below. Previously
			// this call passed the raw working colors directly (unreduced), so every non-Original-Material
			// Preview Mode (RGB, Red, Green, Blue, Alpha Channel) rendered the SAME raw RGB for a Nanite/
			// Source-Topology mesh -- isolated channel modes were indistinguishable from RGB Vertex Color.
			// DISPLAY-ONLY: never mutates the working buffer, and Accept (BuildSourceTopologyAcceptTargets/
			// WriteSourceTopologyAcceptTargets) reads it verbatim, never this reduced copy -- same "Preview
			// Mode never affects Accept" guarantee the render-vertex path already has.
			const TArray<FColor> SourceTopologyRenderOrderColors =
				VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors(StateOwner->GetWorkingColors(), CurrentPreviewMode);

			VertexMaskForgePanel::ActivateSourceTopologyPreviewForComponent(
				State, *WorkingMesh.Mesh, SourceTopologyRenderOrderColors, DebugMaterial, bUseOriginalMaterials, ActorHideStates);
			continue;
		}

		// AUDITED (peer-mask composition checkpoint): builds an UNORDERED set of mask generators for
		// THIS component, then hands it to UpdateWorkingColors/ComposeMaskStack, which sorts it by
		// Mask->Source and applies the fixed, mode-based canonical order internally -- see those
		// functions' own doc comments for the composition contract itself. The order Layers.Add() is
		// called in below is NOT semantically meaningful (Bounding Box happens to be checked first in
		// this code only for readability; it carries no priority) -- see ComposeMaskStack for why.
		// Two shapes:
		//   - Fill/Constant override (GeneratorState.BoundingBoxMask.Source == ConstantWhite/Black): a
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
		if (GeneratorState.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantWhite
			|| GeneratorState.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::ConstantBlack)
		{
			Layers.Add({ &GeneratorState.BoundingBoxMask, EVertexMaskForgeBlendMode::Copy, 1.0f });
		}
		else
		{
			if (bBBoxEntryReady)
			{
				PerComponentBBoxMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMask(
					RenderData->LODResources[0], GeneratorState.BoundingBoxMask.UsedAxisParams, SourceComponent->GetComponentTransform(),
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
				// SEPARATE entry-level call (inside the batch regeneration pass) ALSO called
				// GenerateAmbientOcclusionMask for the reference component, producing a
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
				// geometric/cache-relevant, correctly resolved at the last real (re)generation -- see
				// RunAutoUpdatePreview. bInvert is deliberately
				// OVERRIDDEN with the LIVE bAOInvert panel value here, every call -- Invert is pure
				// composition (OnAOInvertChanged never re-snapshots UsedAOParams, see its own doc
				// comment), so using the stale snapshot's bInvert would silently ignore an Invert
				// toggle until the next real regeneration. GenerateAmbientOcclusionMask applies Invert
				// fresh from AOCache.RawValues every call regardless (see its own doc comment) -- this
				// override only makes sure it uses the CURRENT checkbox state, never a stale one.
				// AUDITED (AO Levels): LevelsMin/Max are LIVE overrides too, same reasoning as bInvert --
				// purely compositional, applied fresh from AOCache.RawValues every call (see
				// ApplyAOLevelsAndInvert), never requiring a raycast/Tree rebuild.
				FVertexMaskForgeAOParams EffectiveAOParams = GeneratorState.AmbientOcclusionMask.UsedAOParams;
				EffectiveAOParams.bInvert = bAOInvert;
				EffectiveAOParams.LevelsMin = AOLevelsMin;
				EffectiveAOParams.LevelsMax = AOLevelsMax;
				PerComponentAOMask = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMask(
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
			// AUDITED (Curvature layer): same "no per-component re-evaluation needed" contract as the
			// Source-Topology branch above -- GeneratorState.CurvatureMask already holds the REAL, final
			// per-render-vertex values (see its own doc comment); IndexOverride stays at its default
			// (-1), so ComposeMaskStack simply looks it up by the shared render vertex index, exactly
			// like Bounding Box/Ambient Occlusion already do in this domain.
			if (!bAnyLayerFailed && bCurvatureEntryReady)
			{
				Layers.Add({ &GeneratorState.CurvatureMask, CurvatureBlendMode, CurvatureOpacity });
			}
			// AUDITED (Noise V1): same "no per-component re-evaluation needed" contract as Curvature
			// above -- GeneratorState.NoiseMask already holds the REAL, final per-render-vertex values;
			// IndexOverride stays at its default (-1), looked up by the shared render vertex index.
			if (!bAnyLayerFailed && bNoiseEntryReady)
			{
				Layers.Add({ &GeneratorState.NoiseMask, NoiseBlendMode, NoiseOpacity });
			}
			// AUDITED (V2-D): same "no per-component re-evaluation needed" contract as Curvature/Noise
			// above -- GeneratorState.MaterialSlotMask already holds the REAL, final per-render-vertex
			// values; IndexOverride stays at its default (-1), looked up by the shared render vertex
			// index (this is the RENDER-VERTEX domain, unlike the Source-Topology branch's corner-exact
			// one -- see GenerateMaterialSlotMask's own doc comment).
			if (!bAnyLayerFailed && bMaterialSlotMaskEntryReady)
			{
				Layers.Add({ &GeneratorState.MaterialSlotMask, MaterialSlotMaskBlendMode, MaterialSlotMaskOpacity });
			}
			// AUDITED (V2-E): Local Space reuses the REAL, final per-render-vertex entry-level values
			// directly (IndexOverride default -1), like Curvature/Noise/Material Slot above. World Space
			// is ALWAYS transform-dependent (like Ambient Occlusion) -- re-evaluated fresh per component
			// into a per-component-scoped local variable, never cached.
			if (!bAnyLayerFailed && bDirectionalNormalMaskEntryReady)
			{
				if (DirectionalNormalSpace == EVertexMaskForgeNormalSpace::Local)
				{
					Layers.Add({ &GeneratorState.DirectionalNormalMask, DirectionalNormalMaskBlendMode, DirectionalNormalMaskOpacity });
				}
				else
				{
					PerComponentDirectionalNormalMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMask(
						RenderData->LODResources[0], DirectionalNormalSpace, DirectionalNormalDirection,
						DirectionalNormalAngle, DirectionalNormalFalloff, DirectionalNormalBlur, bDirectionalNormalMaskInvert,
						SourceComponent->GetComponentTransform());
					if (PerComponentDirectionalNormalMask.State != EVertexMaskForgeScalarMaskState::Ready)
					{
						bAnyLayerFailed = true;
					}
					else
					{
						Layers.Add({ &PerComponentDirectionalNormalMask, DirectionalNormalMaskBlendMode, DirectionalNormalMaskOpacity });
					}
				}
			}
		}

			// AUDITED (V2-G): Thickness is ALWAYS Asset Local Space -- reuses the REAL, final per-
			// render-vertex entry-level values directly (IndexOverride default -1), like Curvature/
			// Noise/Material Slot above -- no per-component re-evaluation.
			if (!bAnyLayerFailed && bThicknessMaskEntryReady)
			{
				Layers.Add({ &GeneratorState.ThicknessMask, ThicknessMaskBlendMode, ThicknessMaskOpacity });
			}
		// AUDITED (preview-stuck-on-Original-Material fix): Layers.IsEmpty() alone is no longer part
		// of this fallback -- see ApplyPreviewToEntry's entry-level gate doc comment above for why an
		// empty Layers set (no mask currently enabled) is a valid, fully-supported input to
		// UpdateWorkingColors (it simply yields the current Baseline/Committed working colors). Only a
		// genuine per-component evaluation failure still falls back to the real component's appearance
		// here; BaselineColors/CommittedColors/WorkingColors/AOCache survive this fallback untouched
		// either way (see RestorePreviewVisualOnly's own doc comment).
		if (bAnyLayerFailed)
		{
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
		// M16-J.0B (2nd rejection-corrective pass): see the Source-Topology sibling call's own comment
		// above -- same EnsureBaselineCaptured/ComputeComposedColorsRGB/ApplyComposedColorsRGB contract.
		// bCommit forwarded from this call's own caller (UpdateAllPreviews) -- true ONLY for an explicit
		// Fill.
		if (!StateOwner->AreColorsInitialized())
		{
			StateOwner->EnsureBaselineCaptured(VertexMaskForgePanel::CaptureBaselineColorsRenderVertex(RenderData->LODResources[0], InstanceOverrideColors));
		}

		TArray<FColor> FinalColors;
		int32 NumComposed = 0;
		VertexMaskForgePanel::ComputeComposedColorsRGB(
			StateOwner->GetBaselineColors(), StateOwner->GetCommittedColors(), Layers, GeneratorLayerOrder,
			bChannelFilterR, bChannelFilterG, bChannelFilterB,
			FinalColors, NumComposed);

		if (!StateOwner->ApplyComposedColorsRGB(MoveTemp(FinalColors),
			bCommit ? FVertexMaskForgeWorkingStateOwner::EColorCommitMode::Consolidate : FVertexMaskForgeWorkingStateOwner::EColorCommitMode::PreviewOnly))
		{
			VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
			continue;
		}

		// M16-J.0B: same real, diagnostic-only binding seam call site as the Source-Topology branch above.
		if (const TOptional<EVertexMaskForgeWorkingColorsPublicationValidationStatus> BindingStatus = ValidatePublicationBindingForComponent(SourceComponent))
		{
			UE_LOG(LogVertexMaskForge, VeryVerbose,
				TEXT("Vertex Mask Forge: publication binding status for '%s' = %d."),
				*SourceComponent->GetName(), static_cast<int32>(*BindingStatus));
		}

		const TArray<FColor> RenderOrderColors = VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors(StateOwner->GetWorkingColors(), CurrentPreviewMode);

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
		if (VertexMaskForgeBoundingBoxGenerator::ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/false, CollectiveBounds, CollectiveError))
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
				if (Entry.IsValid() && Entry->GeneratorState.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::BoundingBox)
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

	// Never write to any asset here, and never let a pending live-update debounce fire after
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

		for (TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
		{
			const FVertexMaskForgePreviewComponentState& State = StateOwner->GetPreviewState();
			const UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
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

			VertexMaskForgePanel::RestoreComponentOriginal(*StateOwner, ActorHideStates);
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
