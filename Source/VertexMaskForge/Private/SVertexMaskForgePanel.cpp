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
#include "Framework/Application/SlateApplication.h"
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
#include "VertexMaskForgeColorConversion.h"
#include "VertexMaskForgeCurvatureGenerator.h"
#include "VertexMaskForgeDirectionalNormalGenerator.h"
#include "VertexMaskForgeDynamicAcceptTargetBuilder.h"
#include "VertexMaskForgeDisplayColorDerivation.h"
#include "VertexMaskForgeDynamicSourceTopologyComposition.h"
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

	/**
	 * M16-K.6D-8F-C: parameterized extraction of the Legacy panel's own UsesFractalParameters() member
	 * (SVertexMaskForgePanel.h) -- same five-type switch (FractalPerlin, Billow, Ridged, Turbulence,
	 * Alligator), but taking an explicit EVertexMaskForgeNoiseType instead of reading the Legacy-only
	 * NoiseType field, so the Dynamic Noise block's Octaves/Roughness/Lacunarity gating can reuse the exact
	 * same rule for a layer-owned FVertexMaskForgeNoiseParams::Type value. Real production logic (not
	 * test-only), private to this file, removes what would otherwise be a third duplicate of the switch.
	 * Behavior is byte-for-byte identical to the Legacy member for every enumerator.
	 */
	static bool NoiseTypeUsesFractalParameters(const EVertexMaskForgeNoiseType Type)
	{
		switch (Type)
		{
		case EVertexMaskForgeNoiseType::FractalPerlin:
		case EVertexMaskForgeNoiseType::Billow:
		case EVertexMaskForgeNoiseType::Ridged:
		case EVertexMaskForgeNoiseType::Turbulence:
		case EVertexMaskForgeNoiseType::Alligator:
			return true;
		default:
			return false;
		}
	}

	// M16-K.6D-8F-D.1: definitions for the two production-internal Dynamic Noise Scale-lock functions
	// declared (with their full contracts) in SVertexMaskForgePanel.h -- deliberately NOT `static`
	// (unlike GetCurvatureTypeLabel/NoiseTypeUsesFractalParameters above), since these two specifically
	// need external linkage so VertexMaskForgeDynamicNoiseUISelectorTests.cpp's own automation tests can
	// call the SAME functions SVertexMaskForgePanel::OnDynamicNoiseScaleAxesLockChanged and the Scale X
	// callback (below) call, rather than a test-local reconstruction of this algorithm.
	bool NormalizeDynamicNoiseScaleForAxisLock(FVertexMaskForgeNoiseParams& Params)
	{
		if (Params.ScaleY == Params.ScaleX && Params.ScaleZ == Params.ScaleX)
		{
			return false;
		}
		Params.ScaleY = Params.ScaleX;
		Params.ScaleZ = Params.ScaleX;
		return true;
	}

	void ApplyDynamicNoiseScaleXEdit(FVertexMaskForgeNoiseParams& Params, const float NewValue, const bool bAxesLocked)
	{
		const float ClampedValue = FMath::Max(NewValue, 0.001f);
		Params.ScaleX = ClampedValue;
		if (bAxesLocked)
		{
			Params.ScaleY = ClampedValue;
			Params.ScaleZ = ClampedValue;
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

	/**
	 * M16-K.6B; extended M16-K.6D-8C-C, M16-K.6D-8D-C, M16-K.6D-8E-C, M16-K.6D-8G-F, M17-TH-DL-B: human-
	 * readable label for one Dynamic Layer row's Generator Type combo option/current value. A null InOption
	 * represents "None/Unassigned" (mirrors DynamicLayerGeneratorTypeOptions' own element-0-is-null
	 * convention). A non-null InOption is now one of all seven EVertexMaskForgeGeneratorType enumerators,
	 * every one of which DynamicLayerGeneratorTypeOptions offers (M17-TH-DL-B added Thickness, the last
	 * remaining value) -- the default case is retained defensively only (never structurally unreachable,
	 * since GeneratorType is a plain uint8 enum with no Count/Max sentinel), not because any value is still
	 * deliberately unsupported.
	 */
	static FText GetDynamicLayerGeneratorTypeLabel(const EVertexMaskForgeGeneratorType* InOption)
	{
		if (!InOption)
		{
			return LOCTEXT("DynamicLayerGeneratorNone", "None");
		}
		switch (*InOption)
		{
		case EVertexMaskForgeGeneratorType::MaterialSlot:
			return LOCTEXT("DynamicLayerGeneratorMaterialSlot", "Material Slot");
		case EVertexMaskForgeGeneratorType::BoundingBox:
			// M18: artist-facing rename only (Substance Painter vocabulary alignment) -- the internal
			// enumerator/generator name/cache identity/serialized value are all unchanged.
			return LOCTEXT("DynamicLayerGeneratorPosition", "Position");
		case EVertexMaskForgeGeneratorType::AmbientOcclusion:
			return LOCTEXT("DynamicLayerGeneratorAmbientOcclusion", "Ambient Occlusion");
		case EVertexMaskForgeGeneratorType::DirectionalNormal:
			return LOCTEXT("DynamicLayerGeneratorDirectionalNormal", "Directional Normal");
		case EVertexMaskForgeGeneratorType::Curvature:
			return LOCTEXT("DynamicLayerGeneratorCurvature", "Curvature");
		case EVertexMaskForgeGeneratorType::Noise:
			return LOCTEXT("DynamicLayerGeneratorNoise", "Noise/Grunge");
		case EVertexMaskForgeGeneratorType::Thickness:
			return LOCTEXT("DynamicLayerGeneratorThickness", "Thickness");
		default:
			return LOCTEXT("DynamicLayerGeneratorUnsupported", "Unsupported");
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
	// AUDITED (M16-K.5F): the local ToDisplayFColor duplicate that used to live here was removed --
	// UpdateWorkingColors/UpdateWorkingColorsSourceTopology below now call
	// VertexMaskForgeColorConversion::ToDisplayFColor, the single shared implementation (same formula,
	// verified byte-identical). VertexMaskForgeDisplayColorDerivation.cpp's own former copy was removed
	// the same way -- see that file's own diff.

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

			const FVector4f BaselineColorF = VertexMaskForgeColorConversion::ToLinearColorF(BaselineRenderColor);
			const FColor& CommittedRenderColor = CommittedColors[i];
			const FVector4f CommittedColorF = VertexMaskForgeColorConversion::ToLinearColorF(CommittedRenderColor);

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
			OutFinalColors[i] = VertexMaskForgeColorConversion::ToDisplayFColor(Composite);
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
					Color = VertexMaskForgeColorConversion::ToDisplayFColor(ColorOverlay->GetElement(ElementID));
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

				const FVector4f BaselineColorF = VertexMaskForgeColorConversion::ToLinearColorF(BaselineRenderColor);
				const FColor& CommittedRenderColor = CommittedColors[CornerIndex];
				const FVector4f CommittedColorF = VertexMaskForgeColorConversion::ToLinearColorF(CommittedRenderColor);

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
				OutFinalColors[CornerIndex] = VertexMaskForgeColorConversion::ToDisplayFColor(Composite);
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
	 * M16-K.6D-2 (correction): the low-level overlay-writing primitive -- writes RenderOrderDisplayColors
	 * (an already Preview-Mode-reduced, render-order array -- see ApplySuppliedSourceTopologyPreviewColors,
	 * the real seam above this function in the file, which is the only production caller) into the
	 * preview component's own Primary Color Overlay. This function reads no panel/owner state of any
	 * kind: not FVertexMaskForgeWorkingStateOwner (no GetWorkingColors/GetSourceTopologyWorkingColors
	 * call anywhere in its body), not WorkingMesh.InstanceResults, not
	 * FVertexMaskForgePreviewComponentState::InstanceResults. Never calls ApplyComposedColorsRGB/
	 * RestoreFromBaseline/RestoreFromCommitted, never writes WorkingColors/CommittedColors/BaselineColors
	 * (or their SourceTopology* siblings) or any Accept-facing store.
	 *
	 * The overlay is fully rebuilt every call (cleared, then one AppendElement + SetTriangle per
	 * triangle corner, in the SAME Mesh.TriangleIndicesItr()-then-corner-0..2 order
	 * UpdateWorkingColorsSourceTopology uses to build the real WorkingColors this function never reads)
	 * -- so ElementID == CornerIndex by construction, needing no separate persisted lookup. Cheap
	 * relative to a raycast pass; simpler and less error-prone than maintaining a stable per-corner
	 * element mapping across updates.
	 *
	 * AUDITED (M16-K.6D-2 correction, Bloqueador 2): a RenderOrderDisplayColors shorter than the mesh's
	 * own corner count is still padded with FColor::White per corner (IsValidIndex guard below) --
	 * PRE-EXISTING, unmodified defensive behavior for this low-level primitive, deliberately left as-is
	 * per this correction's own "do not remove/alter the global fallback without evidence it is safe and
	 * necessary" instruction. This is now structurally UNREACHABLE through the real seam
	 * (ApplySuppliedSourceTopologyPreviewColors), which validates cardinality completely and returns
	 * false before ever calling this function with a mismatched count -- the fallback here only remains
	 * as a defensive floor for this internal primitive, not as this checkpoint's actual failure contract.
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
		const TArray<FColor>& RenderOrderDisplayColors,
		UMaterialInterface* DebugMaterial,
		const UStaticMeshComponent* SourceComponent,
		const bool bUseOriginalMaterials)
	{
		using namespace UE::Geometry;

		if (!IsValid(PreviewComponent))
		{
			return;
		}

		PreviewComponent->EditMesh([&RenderOrderDisplayColors](FDynamicMesh3& Mesh)
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
					const FColor& Color = RenderOrderDisplayColors.IsValidIndex(CornerIndex) ? RenderOrderDisplayColors[CornerIndex] : FColor::White;
					const FVector4f ColorF = VertexMaskForgeColorConversion::ToLinearColorF(Color);
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
			*PreviewComponent->GetName(), PreviewComponent->GetNumMaterials(), RenderOrderDisplayColors.Num(),
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
	 * M16-K.6D-2 (correction): defined here, declared non-static in VertexMaskForgeMaskTypes.h -- see
	 * that header's own doc comment for why (mirrors ComputeComposedColorsRGB[SourceTopology]'s own
	 * established "real production logic, directly testable" pattern). ApplySuppliedSourceTopologyPreviewColors
	 * below is this function's own (and today's only) caller.
	 */
	TOptional<TArray<FColor>> DeriveValidatedSourceTopologyPreviewColors(
		const TArray<FColor>& SemanticComposedColors,
		const int32 ExpectedCornerCount,
		const EVertexMaskForgePreviewMode PreviewMode)
	{
		if (SemanticComposedColors.Num() != ExpectedCornerCount)
		{
			return TOptional<TArray<FColor>>();
		}

		return VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors(SemanticComposedColors, PreviewMode);
	}

	/**
	 * M16-K.6D-2 (correction): THE real Source-Topology preview seam -- receives SemanticComposedColors,
	 * an explicit, read-only, caller-supplied array of ALREADY-COMPOSED (pre-Preview-Mode) corner colors,
	 * plus PreviewMode as an explicit part of its own contract, validates them completely, derives
	 * display colors via VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors ITSELF (never
	 * duplicating that math, never requiring the caller to have already called it), and applies the
	 * result visually. Returns bool so the caller can distinguish success from failure explicitly --
	 * `void` was the M16-K.6D-2 rejected attempt's own defect (see that checkpoint's audit).
	 *
	 * Corrects the M16-K.6D-2 rejected attempt's Bloqueador 1: that attempt's "seam" (formerly named
	 * ActivateSourceTopologyPreviewForComponent, taking a parameter it called ComposedColors) actually
	 * received colors the CALLER had already reduced through DeriveDisplayColors -- so the array was
	 * display-derived, not semantically composed, and calling it "ComposedColors" was factually wrong.
	 * This function's own SemanticComposedColors parameter is what Bloqueador 1 required: the semantic
	 * composition, BEFORE display reduction, with the reduction now performed inside this seam's own
	 * contract instead of by the caller.
	 *
	 * Corrects Bloqueador 2 (silent white-fill fallback): validates SemanticComposedColors.Num() against
	 * SourceMesh.TriangleCount() * 3 (the Source Topology corner-count authority -- see Architecture §3,
	 * the same count ApplyPreviewToEntry's own diagnostic log already uses) BEFORE calling
	 * EnsureSourceTopologyPreviewComponent, before touching any material, before touching the preview
	 * component's overlay, before touching ActorHideState, before touching State.bOverrideActive -- an
	 * invalid count returns false immediately with ZERO visual mutation of any kind (all-or-nothing).
	 * ApplySourceTopologyColorsToPreviewComponent's own IsValidIndex-guarded white-padding (its
	 * pre-existing, unrelated-caller-facing defensive behavior, left unmodified per this correction's own
	 * explicit "do not expand scope without evidence" instruction) becomes structurally unreachable
	 * through this seam specifically, because DeriveDisplayColors always returns exactly
	 * SemanticComposedColors.Num() elements (1:1, proven by its own implementation), which this function
	 * has already validated equals the exact required corner count before ever calling it.
	 *
	 * Corrects Bloqueador 3 (renaming-only, no real behavior): this function is genuinely new behavior --
	 * cardinality validation with an explicit bool failure contract, and PreviewMode/DeriveDisplayColors
	 * folded INTO the seam's own contract instead of left to the caller -- not a rename of the prior
	 * ActivateSourceTopologyPreviewForComponent (which is removed; see Architecture.md for the factual
	 * correction to the M16-K.6D-2 entry that previously mis-described this).
	 *
	 * This function never reads FVertexMaskForgeWorkingStateOwner, WorkingMesh.InstanceResults, or any
	 * other panel/owner state to obtain SemanticComposedColors -- the caller must supply it. Never calls
	 * ApplyComposedColorsRGB/RestoreFromBaseline/RestoreFromCommitted, never writes
	 * WorkingColors/CommittedColors/BaselineColors (or their SourceTopology* siblings) or any Accept-
	 * facing store -- see ADR-011 (Docs/VertexMaskForgeDecisionLog.md) for the non-persistible-preview
	 * contract this seam exists to satisfy. Today's one caller (ApplyPreviewToEntry's Legacy
	 * Source-Topology branch) passes its own just-written StateOwner->GetWorkingColors() (i.e. the exact
	 * semantic composition ApplyComposedColorsRGB just applied) and CurrentPreviewMode -- nothing about
	 * this function's own contract is Legacy-specific; a future Dynamic caller (not introduced by this
	 * checkpoint) may supply its own local/transitory semantically-composed array and PreviewMode through
	 * these exact same parameters, unchanged.
	 *
	 * On failure (invalid SourceComponent, or cardinality mismatch), this function does not touch the
	 * preview visual in any way -- it is the CALLER's own responsibility to call
	 * RestorePreviewVisualOnly/RestorePreviewForEntryVisualOnly explicitly (see ApplyPreviewToEntry's own
	 * call site), exactly as every other failure branch in ApplyPreviewToEntry already does. This
	 * function never falls back to WorkingColors, never falls back to Legacy, never silently preserves a
	 * stale prior visual.
	 *
	 * AUDITED (Original Textures fix): bUseOriginalMaterials forwarded verbatim to
	 * ApplySourceTopologyColorsToPreviewComponent -- see that function's own doc comment. This is the
	 * SAME transient PreviewComponent used for every Preview Mode (debug or original); Original
	 * Textures no longer tears it down (see ApplyPreviewToEntry's own doc comment on removing the old
	 * OriginalMaterial early-return). Same Actor-hide acquisition contract and same "known limitation"
	 * (Actor-level hide, not per-component; see ActivatePreviewForComponent's own doc comment) as before.
	 */
	static bool ApplySuppliedSourceTopologyPreviewColors(
		FVertexMaskForgePreviewComponentState& State,
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const TArray<FColor>& SemanticComposedColors,
		const EVertexMaskForgePreviewMode PreviewMode,
		UMaterialInterface* DebugMaterial,
		const bool bUseOriginalMaterials,
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates)
	{
		UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
		if (!IsValid(SourceComponent))
		{
			return false;
		}

		// AUDITED (Bloqueador 1 + Bloqueador 2 correction): the all-or-nothing cardinality gate and the
		// Preview Mode display derivation are delegated to the pure, directly-testable
		// DeriveValidatedSourceTopologyPreviewColors (declared in VertexMaskForgeMaskTypes.h, defined
		// below in this same file) -- no visual mutation of any kind has happened yet at this point.
		// SourceMesh.TriangleCount() * 3 is the authoritative Source Topology corner count (Architecture
		// §3 "Source Topology Domain"; the same count ApplyPreviewToEntry's own composed-corner-count log
		// line already uses).
		const TOptional<TArray<FColor>> RenderOrderDisplayColorsOpt =
			VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
				SemanticComposedColors, SourceMesh.TriangleCount() * 3, PreviewMode);
		if (!RenderOrderDisplayColorsOpt.IsSet())
		{
			// No padding, no truncation, no partial application, no fallback of any kind.
			return false;
		}
		const TArray<FColor>& RenderOrderDisplayColors = RenderOrderDisplayColorsOpt.GetValue();

		UDynamicMeshComponent* PreviewComponent = EnsureSourceTopologyPreviewComponent(State, SourceMesh);
		if (!PreviewComponent)
		{
			return false;
		}

		ApplySourceTopologyColorsToPreviewComponent(PreviewComponent, RenderOrderDisplayColors, DebugMaterial, SourceComponent, bUseOriginalMaterials);

		if (!State.bHasAcquiredActorHide)
		{
			AActor* Owner = SourceComponent->GetOwner();
			AcquireActorHidden(ActorHideStates, Owner);
			State.HiddenOwner = Owner;
			State.bHasAcquiredActorHide = true;
		}

		State.bOverrideActive = true;
		return true;
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

// M16-K.6B; corrected M16-K.6D-6: shared dropdown-row generator for every Dynamic Layer's Generator Type
// combo -- generic over TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>> only, mirroring
// OnGenerateDynamicLayerFillRow's own shape. InOption itself is always a VALID pointer (see
// DynamicLayerGeneratorTypeOptions' own doc comment for why); an UNSET TOptional (element 0) renders the
// "None" row, a SET one renders that generator type's label.
TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateDynamicLayerGeneratorTypeRow(TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>> InOption) const
{
	const bool bHasValue = InOption.IsValid() && InOption->IsSet();
	return SNew(STextBlock)
		.Text(VertexMaskForgePanel::GetDynamicLayerGeneratorTypeLabel(bHasValue ? &InOption->GetValue() : nullptr));
}

// M16-K.6C-2 / ADR-010: the exact single-asset gate -- SelectedMeshes.Num() == 1, identical to
// IsMaterialSlotMaskAvailableForSelection's own condition. Never picks an arbitrary entry from a
// multi-asset selection, never computes a union/intersection. Returns nullptr for zero or multiple
// assets, or if the one entry/its MeshOwner is somehow invalid. Does NOT check MaterialSlotOptions --
// callers distinguish "no eligible asset" from "eligible asset with zero slots" themselves.
const FVertexMaskForgeWorkingMesh* SVertexMaskForgePanel::GetSingleAssetWorkingMeshForDynamicMaterialSlot() const
{
	if (SelectedMeshes.Num() != 1)
	{
		return nullptr;
	}
	const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry = SelectedMeshes[0];
	if (!Entry.IsValid() || !Entry->MeshOwner.IsValid())
	{
		return nullptr;
	}
	return &Entry->MeshOwner->GetWorkingMesh();
}

// M16-K.6C-2: shared dropdown-row generator for the Dynamic Material Slot picker -- mirrors
// OnGenerateDynamicLayerFillRow's own shape, reusing the legacy picker's own label format verbatim.
TSharedRef<SWidget> SVertexMaskForgePanel::OnGenerateDynamicMaterialSlotPickerRow(TSharedPtr<FVertexMaskForgeMaterialSlotInfo> InOption) const
{
	return SNew(STextBlock)
		.Text(InOption.IsValid() ? VertexMaskForgePanel::GetMaterialSlotLabel(*InOption) : FText::GetEmpty());
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

// M18: GetActiveMaskSourceText (the Legacy-only "Active layers: ..." readout) was removed along with
// its widget -- see the Layers section's own doc comment above.

// ==================================================================================================
// M16-K.4: Dynamic Layers UI Prototype -- domain-only, deliberately disconnected from composition/
// preview. Every function below reads/writes ONLY DynamicLayerStack (via its own controlled, GUID-based
// mutation API); none of them call RecomposeWorkingColors, UpdateAllPreviews, invalidate any generator
// mask/cache, touch GeneratorLayerOrder, or call VertexMaskForgeDynamicLayerEvaluator. Structural changes
// (Add/Remove/Move) rebuild the row list; property changes (Enabled/Fill/BlendMode/Opacity/Channel
// Filter/Rename) mutate the stack only -- each control's own displayed value is bound live to the stack
// via a _Lambda accessor, so no rebuild is needed and no widget holds a duplicate copy of the data.
// ==================================================================================================

void SVertexMaskForgePanel::OnDynamicLayerStackMutated()
{
	// M18: Layers is now the sole workflow, so every mutation always requests a refresh -- the earlier
	// "no-op unless PreviewSource == Dynamic" gate (from when Legacy and Dynamic were separate, selectable
	// pipelines) is gone along with PreviewSource itself.
	RecomposeWorkingColors();
}

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
				.Text(LOCTEXT("NoDynamicLayers", "No layers"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		return;
	}

	// M16-K.6D-6 (defect fix, manual validation, D1): rows are added in REVERSE array order -- the
	// LAST element of DynamicLayerStack.GetLayers() (composited LAST by both
	// VertexMaskForgeDynamicLayerEvaluator::EvaluateColor and the K.6D-4 orchestrator, both of which
	// fold "strictly in array order, index 0 first" -- an established, tested, unmodified contract) is
	// therefore the layer with the HIGHEST Copy-mode composition priority ("wins" over earlier layers),
	// and is now the FIRST row added, so it renders at the TOP of the panel -- matching the standard
	// painter/layer-stack convention (top of the list = frontmost/highest priority) most artists already
	// expect. Root cause of the reported "visual layer priority is inverted" defect: this function
	// previously walked the array forward (index 0, the LOWEST-priority layer, at the top), directly
	// opposite of the evaluator's own priority order. DynamicLayerStack's own stored array order,
	// MoveLayerUp/MoveLayerDown, and the evaluator/orchestrator's fold direction are ALL unchanged by
	// this fix -- this is a display-order-only correction; see BuildDynamicLayerRow's Move Up/Down
	// buttons for the corresponding (also display-only) adjustment that keeps "Up" meaning "move toward
	// the top of the panel" under this new, reversed rendering order.
	const TArray<FVertexMaskForgeLayer>& Layers = DynamicLayerStack.GetLayers();
	for (int32 LayerIndex = Layers.Num() - 1; LayerIndex >= 0; --LayerIndex)
	{
		// AUDITED: LayerId captured BY VALUE into BuildDynamicLayerRow's own row-building lambdas below --
		// never a pointer/reference into this Layer (which may be relocated or destroyed by any later
		// Add/Remove/Move, all of which may reallocate DynamicLayerStack's internal TArray).
		DynamicLayersListContainer->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f))
			[
				BuildDynamicLayerRow(Layers[LayerIndex].LayerId)
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

	// M16-K.6C-2-FIX: captured ONCE, here, at the moment this specific row is built for LayerId's then-
	// current mask -- this is the widget's own fixed "identity contract" for the rest of its lifetime.
	// Never re-read inside a callback and substituted for this value: a stale-firing callback from an
	// earlier-built widget must compare against the id it was built for, not whatever id happens to be
	// current when it fires (that would be tautological and provide no protection at all -- see the
	// Material Slot section's OnSelectionChanged/OnCheckStateChanged below for the actual identity check).
	// This row -- and therefore this captured value -- is rebuilt from scratch by RebuildDynamicLayersList
	// whenever the Generator Type combo above changes what LayerId's mask actually is (see its
	// OnSelectionChanged), so a live row's captured value never goes stale while that row is the one
	// currently displayed for LayerId.
	const FVertexMaskForgeGeneratorMaskInstance* MaterialSlotMaskAtConstruction = DynamicLayerStack.GetLayerMask(LayerId);
	const FGuid MaterialSlotExpectedMaskInstanceId = MaterialSlotMaskAtConstruction ? MaterialSlotMaskAtConstruction->MaskInstanceId : FGuid();

	// M16-K.6D-6 (defect fix, manual validation): the Generator Type combo's InitiallySelectedItem must
	// match LayerId's ACTUAL current mask state at this row's construction, resolved from the SAME
	// MaterialSlotMaskAtConstruction captured above -- never unconditionally DynamicLayerGeneratorTypeOptions[0]
	// ("None"). Root cause of the reported "no UI route back to None once Material Slot is assigned"
	// defect: SComboBox::OnSelectionChanged_Internal (Widgets/Input/SComboBox.h) only invokes its
	// OnSelectionChanged delegate when the clicked item differs from its own internally tracked
	// SelectedItem (bReselectionTriggersOnSelectionChanged defaults false) -- with InitiallySelectedItem
	// hardcoded to "None", a row rebuilt for a layer that already has a Material Slot mask (e.g.
	// immediately after RebuildDynamicLayersList, called by this same combo's own OnSelectionChanged one
	// line below) silently desyncs the widget's internal SelectedItem to "None" while the domain's real
	// state remains Material Slot -- the button's own reactive Text_Lambda still displays "Material
	// Slot" correctly, masking the desync -- so clicking "None" again proposes the value the widget
	// already believes is selected and the delegate never fires, leaving ClearLayerMask uncalled.
	// Resolved by matching VALUE (never array position) against DynamicLayerGeneratorTypeOptions' own
	// elements, exactly mirroring how every other control in this row already resolves its own displayed
	// state from the stack, never from a cached/duplicated copy.
	TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>> GeneratorTypeInitialSelection = DynamicLayerGeneratorTypeOptions.IsValidIndex(0) ? DynamicLayerGeneratorTypeOptions[0] : nullptr;
	if (MaterialSlotMaskAtConstruction)
	{
		for (const TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>>& Option : DynamicLayerGeneratorTypeOptions)
		{
			if (Option.IsValid() && Option->IsSet() && Option->GetValue() == MaterialSlotMaskAtConstruction->GeneratorType)
			{
				GeneratorTypeInitialSelection = Option;
				break;
			}
		}
	}

	// M16-K.6D-6 (Correction 1): the Fill combo's InitiallySelectedItem, resolved from LayerId's ACTUAL
	// current Fill -- same rationale and same reselection-desync risk as GeneratorTypeInitialSelection
	// above. DynamicLayerFillOptions no longer offers EVertexMaskForgeLayerFill::None (see its own doc
	// comment), so if the layer's stored Fill is still None (the real, unchanged domain default
	// immediately after AddLayer, before OnAddDynamicLayerClicked's own follow-up SetLayerFill(White)
	// call -- or any other, currently nonexistent, future path that could leave it there), this falls
	// back to the first offered option (Black) deterministically -- never null/invalid, never silently
	// desynchronized. This is a display-selection fallback only; it never writes DynamicLayerStack.
	const FVertexMaskForgeLayer* LayerAtConstruction = DynamicLayerStack.FindLayerById(LayerId);
	TSharedPtr<EVertexMaskForgeLayerFill> FillInitialSelection = DynamicLayerFillOptions.IsValidIndex(0) ? DynamicLayerFillOptions[0] : nullptr;
	if (LayerAtConstruction)
	{
		for (const TSharedPtr<EVertexMaskForgeLayerFill>& Option : DynamicLayerFillOptions)
		{
			if (Option.IsValid() && *Option == LayerAtConstruction->Fill)
			{
				FillInitialSelection = Option;
				break;
			}
		}
	}

	// M16-K.6C-2-FIX: per-row-exclusive, stable-address, refcounted options container -- replaces the
	// removed panel-wide DynamicMaterialSlotPickerOptions member. Each call to BuildDynamicLayerRow (i.e.
	// each row/widget instance) gets its OWN TArray via its own TSharedRef, captured by value into this
	// row's OnComboBoxOpening/OnGenerateWidget closures below, so refreshing one Dynamic layer's Material
	// Slot dropdown can never repopulate, alias, or invalidate another layer's (or another instance's)
	// options array or the TSharedPtr<FVertexMaskForgeMaterialSlotInfo> elements it holds.
	const TSharedRef<TArray<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>>> MaterialSlotPickerOptions =
		MakeShared<TArray<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>>>();

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

		// --- Structural row: Drag Handle, Enabled, Name, Remove ----------------------------------
		// M16-K.6D-6 (Correction 2): Up/Down buttons removed -- reordering is now drag-and-drop only,
		// via this row's own outer SBorder being a slot of the panel's SDragAndDropVerticalBox
		// (DynamicLayersListContainer). See BuildDynamicLayerRow's own header doc comment (top of this
		// function is unchanged; this comment documents the row's new structure) and
		// OnDynamicLayerDragDetected/OnDynamicLayerCanAcceptDrop/OnDynamicLayerAcceptDrop for the actual
		// drag/drop handling. The Drag Handle glyph below is deliberately a plain, non-interactive
		// STextBlock (no OnClicked/OnMouseButtonDown of its own) -- it never consumes the mouse-down that
		// SDragAndDropVerticalBox's own OnMouseButtonDown needs to see to begin drag detection (see that
		// class's Construct/OnMouseButtonDown in Widgets/SBoxPanel.h/.cpp); it exists purely as a visual
		// affordance marking where to grab. Every OTHER control in this row (checkboxes, the editable
		// name box, combos, spin box, buttons) already consumes its own mouse-down before it could ever
		// reach the row's own drag-detection, so normal interaction with them is unaffected -- confirmed
		// by direct reading of SDragAndDropVerticalBox::OnMouseButtonDown, which only ever fires for
		// events that were NOT already handled by a child widget.
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
				.Text(LOCTEXT("DynamicLayerDragHandle", "≡"))
				.ToolTipText(LOCTEXT("DynamicLayerDragHandleTooltip", "Drag to reorder this layer (drag toward the top for higher composition priority)."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

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
					OnDynamicLayerStackMutated();
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
				.InitiallySelectedItem(FillInitialSelection)
				.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateDynamicLayerFillRow)
				.OnSelectionChanged(SComboBox<TSharedPtr<EVertexMaskForgeLayerFill>>::FOnSelectionChanged::CreateLambda(
					[this, LayerId](TSharedPtr<EVertexMaskForgeLayerFill> NewSelection, ESelectInfo::Type)
					{
						if (NewSelection.IsValid())
						{
							DynamicLayerStack.SetLayerFill(LayerId, *NewSelection);
							OnDynamicLayerStackMutated();
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

			// M16-K.6B: Generator Type assignment -- the ONLY control this checkpoint adds. Selecting
			// "None" calls ClearLayerMask; selecting "Material Slot" calls SetLayerMaskGeneratorType.
			// Neither branch touches any result store, generates anything, calls ComposeColors/
			// ApplyComposedColorsRGB/UpdateAllPreviews, or mutates WorkingColors/CommittedColors/
			// BaselineColors in any way -- this control edits DynamicLayerStack's own configuration only,
			// exactly like every other control in this row (Fill/BlendMode/Opacity/Channel Filter above).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				SNew(SComboBox<TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>>>)
				.OptionsSource(&DynamicLayerGeneratorTypeOptions)
				.InitiallySelectedItem(GeneratorTypeInitialSelection)
				.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateDynamicLayerGeneratorTypeRow)
				.OnSelectionChanged(SComboBox<TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>>>::FOnSelectionChanged::CreateLambda(
					[this, LayerId](TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>> NewSelection, ESelectInfo::Type)
					{
						// M16-K.6D-8G-C: captured BEFORE the mutation below, purely to detect whether this
						// callback represents a GENUINE generator-type transition (vs. an idempotent
						// same-type reselection, or clearing an already-unset mask) -- both
						// SetLayerMaskGeneratorType and ClearLayerMask are deliberately idempotent (see
						// their own doc comments in VertexMaskForgeDynamicLayerStack.cpp) and still return
						// true for a no-op call, so that return value alone cannot distinguish "nothing
						// changed" from "a real transition happened". A null result here means either "this
						// LayerId currently has no mask" or "this LayerId no longer exists" -- both cases
						// are resolved safely below without needing to tell them apart, because erasure is
						// additionally gated on the mutator's own success below.
						const FVertexMaskForgeGeneratorMaskInstance* ExistingMaskBeforeMutation = DynamicLayerStack.GetLayerMask(LayerId);

						bool bGeneratorTypeMutationSucceeded = false;
						bool bGenuineGeneratorTypeTransition = false;
						if (NewSelection.IsValid() && NewSelection->IsSet())
						{
							bGenuineGeneratorTypeTransition = !ExistingMaskBeforeMutation
								|| ExistingMaskBeforeMutation->GeneratorType != NewSelection->GetValue();
							bGeneratorTypeMutationSucceeded = DynamicLayerStack.SetLayerMaskGeneratorType(LayerId, NewSelection->GetValue());
						}
						else
						{
							bGenuineGeneratorTypeTransition = (ExistingMaskBeforeMutation != nullptr);
							bGeneratorTypeMutationSucceeded = DynamicLayerStack.ClearLayerMask(LayerId);
						}

						// M16-K.6D-8G-C: a generator-type change invalidates any retained Dynamic AO cache
						// entry for THIS LayerId -- erased ONLY on a genuine transition that the stack
						// mutation above actually completed (never on an idempotent no-op, which must
						// preserve a future AO layer's already-retained raw mask and tree unchanged, and
						// never for a stale/unknown LayerId, which the mutator's own false return already
						// rejects). Unrelated layers are never inspected or touched; LayerId itself is
						// never regenerated. The map is still entirely dormant this checkpoint -- see
						// FVertexMaskForgePreviewComponentState's own doc comment on
						// DynamicSourceTopologyAOCachesByLayerId -- so this erasure is currently always a
						// no-op in practice, establishing the lifecycle rule ahead of the backend that will
						// make it observable.
						if (bGeneratorTypeMutationSucceeded && bGenuineGeneratorTypeTransition)
						{
							for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
							{
								if (!Entry.IsValid())
								{
									continue;
								}
								for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
								{
									StateOwner->GetVisualSessionStateMutable().DynamicSourceTopologyAOCachesByLayerId.Remove(LayerId);
									// M17-TH-DL-B: sibling erasure for the Dynamic Thickness cache, same rule
									// (genuine generator-type transition only, never an idempotent no-op).
									StateOwner->GetVisualSessionStateMutable().DynamicSourceTopologyThicknessCachesByLayerId.Remove(LayerId);
								}
							}
						}

						OnDynamicLayerStackMutated();
						// M16-K.6C-2-FIX: rebuilds the row list so every per-generator configurational
						// section (e.g. the Material Slot editor below) is reconstructed against whatever
						// MaskInstanceId now actually exists -- without this, a Material Slot editor built
						// for an earlier instance would keep referring to that stale instance forever after
						// a Clear/reassignment, since nothing else in this row rebuilds on a Params-only or
						// generator-type change. Safe to call from within this very callback -- mirrors
						// OnAddDynamicLayerClicked/OnRemoveDynamicLayerClicked's own established pattern of
						// rebuilding the list (destroying the row this callback itself belongs to) from
						// inside their own handlers; Slate defers the actual widget destruction until after
						// this event finishes processing.
						RebuildDynamicLayersList();
					}))
				[
					SNew(STextBlock)
					.ToolTipText(LOCTEXT("DynamicLayerGeneratorTypeTooltip", "Which generator produces this layer's mask (assignment only, prototype -- not yet generated, composed, or previewed)."))
					.Text_Lambda([this, LayerId]()
					{
						const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
						return VertexMaskForgePanel::GetDynamicLayerGeneratorTypeLabel(Mask ? &Mask->GeneratorType : nullptr);
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
							OnDynamicLayerStackMutated();
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
					OnDynamicLayerStackMutated();
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 2.f, 0.f))
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("DynamicLayerAffectRedTooltip", "Affect Red Channel\nAlt-click to solo channel"))
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
					// AUDITED (M16-K.4C): Alt detection is UI-only (FSlateApplication's own modifier-key
					// state, queried synchronously inside this click callback) -- never stored on the
					// layer/stack. The pure decision (ResolveDynamicLayerChannelToggle) computes the final
					// RGB shape; SetLayerChannelFilter applies it as the single atomic mutation it already
					// was, unchanged from M16-K.4A/K.4B.
					const bool bAltDown = FSlateApplication::Get().GetModifierKeys().IsAltDown();
					const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
						Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue,
						EVertexMaskForgeDynamicLayerChannel::Red, NewState == ECheckBoxState::Checked, bAltDown);
					DynamicLayerStack.SetLayerChannelFilter(LayerId, Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue);
					OnDynamicLayerStackMutated();
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
				.ToolTipText(LOCTEXT("DynamicLayerAffectGreenTooltip", "Affect Green Channel\nAlt-click to solo channel"))
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
					const bool bAltDown = FSlateApplication::Get().GetModifierKeys().IsAltDown();
					const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
						Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue,
						EVertexMaskForgeDynamicLayerChannel::Green, NewState == ECheckBoxState::Checked, bAltDown);
					DynamicLayerStack.SetLayerChannelFilter(LayerId, Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue);
					OnDynamicLayerStackMutated();
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
				.ToolTipText(LOCTEXT("DynamicLayerAffectBlueTooltip", "Affect Blue Channel\nAlt-click to solo channel"))
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
					const bool bAltDown = FSlateApplication::Get().GetModifierKeys().IsAltDown();
					const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
						Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue,
						EVertexMaskForgeDynamicLayerChannel::Blue, NewState == ECheckBoxState::Checked, bAltDown);
					DynamicLayerStack.SetLayerChannelFilter(LayerId, Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue);
					OnDynamicLayerStackMutated();
				})
				.Content()
				[
					SNew(STextBlock).Text(LOCTEXT("DynamicLayerAffectBlueLabel", "B"))
				]
			]
		]

		// M16-K.6D-8C-C: "Generator Parameters" -- one expander per layer, visible only when the layer's
		// assigned generator actually has Dynamic-exposed parameters (today: MaterialSlot or BoundingBox;
		// a Fill-only/None layer shows no expander at all, never an empty one). Belongs to THIS layer --
		// never a shared panel-level control -- and is never the obsolete standalone Layers panel (removed
		// entirely in M16-K.6D-8C-B; this is a sibling section inside one Dynamic Layer's own row).
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(false)
			.Padding(FMargin(4.f))
			.Visibility_Lambda([this, LayerId]()
			{
				const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
				if (!Mask)
				{
					return EVisibility::Collapsed;
				}
				return (Mask->GeneratorType == EVertexMaskForgeGeneratorType::MaterialSlot
					|| Mask->GeneratorType == EVertexMaskForgeGeneratorType::BoundingBox
					|| Mask->GeneratorType == EVertexMaskForgeGeneratorType::AmbientOcclusion
					|| Mask->GeneratorType == EVertexMaskForgeGeneratorType::DirectionalNormal
					|| Mask->GeneratorType == EVertexMaskForgeGeneratorType::Curvature
					|| Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise
					|| Mask->GeneratorType == EVertexMaskForgeGeneratorType::Thickness)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.HeaderContent()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DynamicLayerGeneratorParametersTitle", "Generator Parameters"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]
			.BodyContent()
			[
				SNew(SVerticalBox)

				// M16-K.6C-2 / ADR-010: Material Slot configurational editor -- visible ONLY when this
				// layer's assigned generator is MaterialSlot. Editing is gated to exactly one selected
				// Static Mesh asset (SelectedMeshes.Num()==1, identical to the legacy
				// IsMaterialSlotMaskAvailableForSelection gate) with at least one real material slot on
				// that asset's own WorkingMesh.MaterialSlotOptions -- see
				// GetSingleAssetWorkingMeshForDynamicMaterialSlot. Zero, multiple, or slot-less assets
				// leave this section visible but non-editable, with an explicit inline message -- never a
				// silent clamp, fallback to index 0, arbitrary mesh pick, or intersection/union across
				// assets. A stale/out-of-range stored SelectedSlotIndex is preserved verbatim and surfaced
				// explicitly, never clamped or silently reselected -- the user repairs it by picking a
				// valid option themselves.
				//
				// M16-K.6C-2-FIX: the picker's and Invert's write callbacks below validate identity
				// against MaterialSlotExpectedMaskInstanceId -- the id captured ONCE when this row was
				// built (see above) -- never against a freshly re-read Mask->MaskInstanceId. A freshly
				// re-read id is always trivially equal to itself and therefore proves nothing about
				// whether THIS widget instance still corresponds to the mask instance currently in the
				// stack; only comparing against the construction-time capture can detect a stale-firing
				// callback from a widget built for an instance that was since cleared or replaced (see the
				// six-step check inside each callback below). Only SetLayerMaskParams is ever called, and
				// only from real user-driven selection events (SelectType != ESelectInfo::Direct guards
				// against a programmatic/reconstruction-driven event, e.g. from this row's own
				// OptionsSource being refreshed on dropdown-open). M16-K.6D-8C-C: unchanged below except
				// for the SExpandableArea now wrapping it -- mutation semantics are byte-for-byte identical
				// to before this checkpoint.
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SVerticalBox)
					.Visibility_Lambda([this, LayerId]()
					{
						const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
						return (Mask && Mask->GeneratorType == EVertexMaskForgeGeneratorType::MaterialSlot) ? EVisibility::Visible : EVisibility::Collapsed;
					})

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
				[
					SNew(STextBlock).Text(LOCTEXT("DynamicLayerMaterialSlotPickerLabel", "Material Slot:"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
				[
					SNew(SComboBox<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>>)
					.OptionsSource(&(*MaterialSlotPickerOptions))
					.IsEnabled_Lambda([this]()
					{
						// M18.1: also require bMaterialSlotResolutionValid -- see this generator's own
						// "resolution invalid" inline feedback message below for the full root-cause
						// rationale (previously checked only IsEmpty(), so a mesh with unresolvable/
						// duplicate/missing Material Slot names looked fully editable here even though
						// GenerateMaterialSlotMaskFromDynamicMesh always rejects it, silently failing the
						// whole Dynamic composition call every time regardless of Preview Mode).
						const FVertexMaskForgeWorkingMesh* WorkingMesh = GetSingleAssetWorkingMeshForDynamicMaterialSlot();
						return WorkingMesh && !WorkingMesh->MaterialSlotOptions.IsEmpty() && WorkingMesh->bMaterialSlotResolutionValid;
					})
					.OnComboBoxOpening_Lambda([this, MaterialSlotPickerOptions]()
					{
						// M16-K.6D-6 (Group E defect fix -- evidence-backed, see the checkpoint's own
						// instrumentation): CONFIRMED root cause was recreating brand-new
						// MakeShared<FVertexMaskForgeMaterialSlotInfo> option objects on EVERY open, even
						// when the mesh's own MaterialSlotOptions had not changed at all. SComboBox's own
						// internal SelectedItem (Widgets/Input/SComboBox.h) is a TSharedPtr captured at
						// selection time; discarding and recreating the whole options array orphans that
						// pointer's identity (it no longer matches ANYTHING in the new array), and the list
						// view's own attempt to reconcile/restore it on the next open fires a spurious
						// internal Direct+invalid OnSelectionChanged, which unconditionally calls
						// this->SetIsOpen(false) inside SComboBox::OnSelectionChanged_Internal -- closing the
						// popup that had JUST opened, in the same click. Direct evidence (temporary [MSDIAG]
						// UE_LOG instrumentation, since removed): on the failing "reopen" click,
						// OnComboBoxOpening fired normally (ruling out focus/capture/drag-swallow), followed
						// 12ms later by OnSelectionChanged(Valid=0, SelectType=Direct) -- exactly this
						// mechanism. The FOLLOWING click then succeeds because SelectedItem was ALREADY
						// reset to invalid by that failed attempt, so there is nothing left to fail to
						// reconcile.
						//
						// Fix: only Reset+repopulate when the live WorkingMesh->MaterialSlotOptions content
						// has ACTUALLY changed since this array was last populated (compared by value, since
						// FVertexMaskForgeMaterialSlotInfo has no operator==) -- if unchanged (the common
						// case: reopening the same picker on the same mesh selection), the array, and
						// therefore every existing TSharedPtr's identity including the currently selected
						// one, is left completely untouched, so SelectedItem never gets orphaned in the
						// first place. When the content genuinely changes (a real, if rare, case -- e.g. the
						// underlying selection context changed while this row stayed alive), a real refresh
						// still occurs, exactly as this control was designed to do.
						const FVertexMaskForgeWorkingMesh* WorkingMesh = GetSingleAssetWorkingMeshForDynamicMaterialSlot();
						const TArray<FVertexMaskForgeMaterialSlotInfo>& LiveOptions = WorkingMesh ? WorkingMesh->MaterialSlotOptions : TArray<FVertexMaskForgeMaterialSlotInfo>();

						bool bContentUnchanged = MaterialSlotPickerOptions->Num() == LiveOptions.Num();
						if (bContentUnchanged)
						{
							for (int32 Index = 0; Index < LiveOptions.Num(); ++Index)
							{
								const TSharedPtr<FVertexMaskForgeMaterialSlotInfo>& Existing = (*MaterialSlotPickerOptions)[Index];
								const FVertexMaskForgeMaterialSlotInfo& Live = LiveOptions[Index];
								if (!Existing.IsValid()
									|| Existing->SlotIndex != Live.SlotIndex
									|| Existing->MaterialSlotName != Live.MaterialSlotName
									|| Existing->MaterialAssetName != Live.MaterialAssetName)
								{
									bContentUnchanged = false;
									break;
								}
							}
						}

						if (bContentUnchanged)
						{
							return;
						}

						MaterialSlotPickerOptions->Reset();
						MaterialSlotPickerOptions->Reserve(LiveOptions.Num());
						for (const FVertexMaskForgeMaterialSlotInfo& Info : LiveOptions)
						{
							MaterialSlotPickerOptions->Add(MakeShared<FVertexMaskForgeMaterialSlotInfo>(Info));
						}
					})
					.OnGenerateWidget(this, &SVertexMaskForgePanel::OnGenerateDynamicMaterialSlotPickerRow)
					.OnSelectionChanged(SComboBox<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>>::FOnSelectionChanged::CreateLambda(
						[this, LayerId, MaterialSlotExpectedMaskInstanceId](TSharedPtr<FVertexMaskForgeMaterialSlotInfo> NewSelection, ESelectInfo::Type SelectType)
						{
							// AUDITED: ignores a null selection (nothing chosen) and any non-user-driven
							// SelectInfo (Direct -- fired when the combo's own internal selection is
							// reconciled against a freshly-rebuilt OptionsSource, not from a real click/key)
							// -- only a genuine mouse-click/keyboard-driven pick ever writes.
							if (!NewSelection.IsValid() || SelectType == ESelectInfo::Type::Direct)
							{
								return;
							}
							// M16-K.6C-2-FIX six-step identity-validated write path: (1) resolve the layer's
							// current mask fresh by LayerId; (2)-(3) confirm it exists; (4) confirm the
							// CURRENT generator is still Material Slot; (5) confirm the CURRENT Params variant
							// is still the Material Slot payload type; (6) confirm the CURRENT MaskInstanceId
							// equals the id THIS widget was built for (MaterialSlotExpectedMaskInstanceId,
							// captured by value at construction, never re-derived here) -- only if all six
							// hold do we (7) copy the current payload, (8) mutate only SelectedSlotIndex, and
							// (9) call SetLayerMaskParams with the CAPTURED expected id, not a freshly re-read
							// one. Any mismatch (cleared, reassigned to a different generator, or replaced by
							// a newer Material Slot instance with a different MaskInstanceId) makes this a
							// silent no-op, exactly as required.
							const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
							if (!Mask
								|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::MaterialSlot
								|| !Mask->Params.IsType<FVertexMaskForgeMaterialSlotParams>()
								|| Mask->MaskInstanceId != MaterialSlotExpectedMaskInstanceId)
							{
								return;
							}
							// AUDITED: records only NewSelection->SlotIndex (the option's own real index
							// field, never the array position it happened to occupy) -- never the name.
							FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
							NewParams.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = NewSelection->SlotIndex;
							DynamicLayerStack.SetLayerMaskParams(LayerId, MaterialSlotExpectedMaskInstanceId, NewParams);
							OnDynamicLayerStackMutated();
						}))
					[
						SNew(STextBlock)
						.ToolTipText(LOCTEXT("DynamicLayerMaterialSlotPickerTooltip", "Which material slot this layer's Material Slot mask selects."))
						.Text_Lambda([this, LayerId]() -> FText
						{
							const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
							if (!Mask || !Mask->Params.IsType<FVertexMaskForgeMaterialSlotParams>())
							{
								return FText::GetEmpty();
							}
							const int32 StoredIndex = Mask->Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex;
							// AUDITED: this lookup NEVER writes StoredIndex back -- it only decides which
							// text to display. A match is found by the option's own SlotIndex field, never
							// by array position.
							if (const FVertexMaskForgeWorkingMesh* WorkingMesh = GetSingleAssetWorkingMeshForDynamicMaterialSlot())
							{
								for (const FVertexMaskForgeMaterialSlotInfo& Info : WorkingMesh->MaterialSlotOptions)
								{
									if (Info.SlotIndex == StoredIndex)
									{
										return VertexMaskForgePanel::GetMaterialSlotLabel(Info);
									}
								}
							}
							return FText::Format(
								LOCTEXT("DynamicLayerMaterialSlotStaleFormat", "Slot {0} (not available for the current asset)"),
								FText::AsNumber(StoredIndex));
						})
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("DynamicLayerMaterialSlotInvertTooltip", "Invert coverage."))
					.IsEnabled_Lambda([this]()
					{
						// M18.1: mirrors the picker's own IsEnabled_Lambda above -- see its comment.
						const FVertexMaskForgeWorkingMesh* WorkingMesh = GetSingleAssetWorkingMeshForDynamicMaterialSlot();
						return WorkingMesh && !WorkingMesh->MaterialSlotOptions.IsEmpty() && WorkingMesh->bMaterialSlotResolutionValid;
					})
					.IsChecked_Lambda([this, LayerId]()
					{
						const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
						return (Mask && Mask->Params.IsType<FVertexMaskForgeMaterialSlotParams>() && Mask->Params.Get<FVertexMaskForgeMaterialSlotParams>().bInvert)
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this, LayerId, MaterialSlotExpectedMaskInstanceId](const ECheckBoxState NewState)
					{
						// M16-K.6C-2-FIX: identical six-step identity-validated write path as the picker's
						// OnSelectionChanged above -- see its comment for the full rationale. Compares the
						// CURRENT mask's MaskInstanceId against MaterialSlotExpectedMaskInstanceId (captured
						// by value when this row was built), never a freshly re-read id, and no-ops on any
						// mismatch (cleared, reassigned to a different generator, or replaced by a newer
						// Material Slot instance).
						const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
						if (!Mask
							|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::MaterialSlot
							|| !Mask->Params.IsType<FVertexMaskForgeMaterialSlotParams>()
							|| Mask->MaskInstanceId != MaterialSlotExpectedMaskInstanceId)
						{
							return;
						}
						// AUDITED: preserves SelectedSlotIndex exactly as stored, even if currently stale --
						// only bInvert is mutated.
						FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
						NewParams.Get<FVertexMaskForgeMaterialSlotParams>().bInvert = (NewState == ECheckBoxState::Checked);
						DynamicLayerStack.SetLayerMaskParams(LayerId, MaterialSlotExpectedMaskInstanceId, NewParams);
						OnDynamicLayerStackMutated();
					})
					.Content()
					[
						SNew(STextBlock).Text(LOCTEXT("DynamicLayerMaterialSlotInvertLabel", "Invert"))
					]
				]
			]

			// Inline feedback for the gate-failure cases that are independent of the picker/checkbox
			// above (which stay visible but disabled in all of them). Exactly one message is visible at
			// a time; none are visible when exactly one asset with at least one resolvable slot is
			// selected.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]()
				{
					return SelectedMeshes.Num() == 1 ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.Text_Lambda([this]() -> FText
				{
					if (SelectedMeshes.Num() == 0)
					{
						return LOCTEXT("DynamicLayerMaterialSlotZeroAssets", "Select exactly one Static Mesh asset to edit Material Slot parameters.");
					}
					return FText::Format(
						LOCTEXT("DynamicLayerMaterialSlotMultipleAssetsFormat", "Material Slot editing requires exactly one selected Static Mesh asset ({0} are currently selected)."),
						FText::AsNumber(SelectedMeshes.Num()));
				})
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]()
				{
					const FVertexMaskForgeWorkingMesh* WorkingMesh = GetSingleAssetWorkingMeshForDynamicMaterialSlot();
					return (WorkingMesh && WorkingMesh->MaterialSlotOptions.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text(LOCTEXT("DynamicLayerMaterialSlotNoSlots", "The selected Static Mesh asset has no material slots."))
			]

			// M18.1 (root-cause correction): a mesh whose Material Slots could not be resolved
			// unambiguously (duplicate or missing imported slot names -- WorkingMesh.
			// bMaterialSlotResolutionValid, set by BuildMaterialSlotLookups) previously produced NO
			// feedback at all here -- the picker/Invert above looked fully editable, but
			// GenerateMaterialSlotMaskFromDynamicMesh's own precheck (VertexMaskForgeMaterialSlotGenerator.cpp)
			// always rejects this case, failing the whole Dynamic composition call every time regardless
			// of Preview Mode -- exactly the "stays on Original Material, no visible effect" symptom.
			// Mirrors Legacy's own established diagnostic wording (GetMaterialSlotMaskDiagnosticText,
			// dead code since M18) -- never a new validity policy, just surfacing the SAME established
			// unavailable/error contract the generator has always enforced.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]()
				{
					const FVertexMaskForgeWorkingMesh* WorkingMesh = GetSingleAssetWorkingMeshForDynamicMaterialSlot();
					return (WorkingMesh && !WorkingMesh->MaterialSlotOptions.IsEmpty() && !WorkingMesh->bMaterialSlotResolutionValid)
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text(LOCTEXT("DynamicLayerMaterialSlotResolutionInvalid", "Material Slot unavailable: one or more Material Slots could not be resolved unambiguously (duplicate or missing slot names). Preview/Accept for this layer are blocked."))
			]
			]

			// M16-K.6D-8C-C: Bounding Box configurational editor -- sibling of the Material Slot block
			// above, inside the SAME "Generator Parameters" expander body. Visible/editable only when
			// this layer's assigned generator is BoundingBox (see BuildDynamicBoundingBoxLayerParamsBlock's
			// own doc comment for the full contract, including the Local-space-only enforcement and
			// incompatible-state warning).
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				BuildDynamicBoundingBoxLayerParamsBlock(LayerId, MaterialSlotExpectedMaskInstanceId)
				]

			// M16-K.6D-8G-F: Ambient Occlusion configurational editor -- sibling of the Material Slot and
			// Bounding Box blocks above, inside the SAME "Generator Parameters" expander body. Visible/
			// editable only when this layer's assigned generator is AmbientOcclusion (see
			// BuildDynamicAmbientOcclusionLayerParamsBlock's own doc comment for the full contract). Placed
			// immediately after Bounding Box, matching Legacy's own generator-library ordering (Bounding
			// Box, Ambient Occlusion, Curvature, Directional Normal, Noise, Material Slot) -- this expander
			// body's own placement is purely visual grouping (only one block is ever visible at a time,
			// gated by GeneratorType), never a functional ordering concern.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				BuildDynamicAmbientOcclusionLayerParamsBlock(LayerId, MaterialSlotExpectedMaskInstanceId)
				]

			// M16-K.6D-8D-C: Directional Normal configurational editor -- sibling of the Material Slot and
			// Bounding Box blocks above, inside the SAME "Generator Parameters" expander body. Visible/
			// editable only when this layer's assigned generator is DirectionalNormal (see
			// BuildDynamicDirectionalNormalLayerParamsBlock's own doc comment for the full contract,
			// including the Local-space-only enforcement and incompatible-state warning).
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				BuildDynamicDirectionalNormalLayerParamsBlock(LayerId, MaterialSlotExpectedMaskInstanceId)
				]

			// M16-K.6D-8E-C: Curvature configurational editor -- sibling of the Material Slot, Bounding
			// Box, and Directional Normal blocks above, inside the SAME "Generator Parameters" expander
			// body. Visible/editable only when this layer's assigned generator is Curvature (see
			// BuildDynamicCurvatureLayerParamsBlock's own doc comment for the full contract). Curvature has
			// no Space concept, so unlike its two siblings above there is no incompatible-state warning.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				BuildDynamicCurvatureLayerParamsBlock(LayerId, MaterialSlotExpectedMaskInstanceId)
				]

			// M16-K.6D-8F-C: Noise/Grunge configurational editor -- sibling of the Material Slot, Bounding
			// Box, Directional Normal, and Curvature blocks above, inside the SAME "Generator Parameters"
			// expander body. Visible/editable only when this layer's assigned generator is Noise (see
			// BuildDynamicNoiseLayerParamsBlock's own doc comment for the full contract). Noise has no
			// Space concept, so like Curvature there is no incompatible-state warning.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				BuildDynamicNoiseLayerParamsBlock(LayerId, MaterialSlotExpectedMaskInstanceId)
				]

			// M17-TH-DL-B: Thickness configurational editor -- sibling of the blocks above, inside the SAME
			// "Generator Parameters" expander body. Visible/editable only when this layer's assigned
			// generator is Thickness (see BuildDynamicThicknessLayerParamsBlock's own doc comment for the
			// full contract). Source Topology only this checkpoint; Thickness has no Space concept, so like
			// Curvature/Noise there is no incompatible-state warning.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				BuildDynamicThicknessLayerParamsBlock(LayerId, MaterialSlotExpectedMaskInstanceId)
				]
			]
		]
	];
}

bool SVertexMaskForgePanel::IsDynamicBoundingBoxLayerLocalSpaceCompatible(const FGuid LayerId) const
{
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask || Mask->GeneratorType != EVertexMaskForgeGeneratorType::BoundingBox)
	{
		return true;
	}
	const FVertexMaskForgeBoundingBoxParams* BBoxParams = Mask->Params.TryGet<FVertexMaskForgeBoundingBoxParams>();
	if (!BBoxParams)
	{
		return true;
	}
	if (BBoxParams->bUseUnifiedBounds)
	{
		return false;
	}
	for (const FVertexMaskForgeAxisMaskParams& AxisParams : BBoxParams->Axes)
	{
		if (AxisParams.bEnabled && AxisParams.bWorldSpace)
		{
			return false;
		}
	}
	return true;
}

void SVertexMaskForgePanel::MutateDynamicBoundingBoxAxisParam(
	const FGuid LayerId, const FGuid ExpectedMaskInstanceId, const int32 AxisIndex,
	TFunctionRef<void(FVertexMaskForgeAxisMaskParams&)> Mutator)
{
	// AUDITED (M16-K.6D-8C-C): mirrors the Material Slot picker/Invert callbacks' own six-step identity-
	// validated write path exactly -- mask exists, GeneratorType is still BoundingBox, Params is still the
	// BoundingBox payload type, and MaskInstanceId still matches what THIS widget was built for. Any
	// mismatch (cleared, reassigned to a different generator, or replaced by a newer instance) is a silent
	// no-op, never a fallback to another layer or a stale write.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask
		|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::BoundingBox
		|| !Mask->Params.IsType<FVertexMaskForgeBoundingBoxParams>()
		|| Mask->MaskInstanceId != ExpectedMaskInstanceId
		|| AxisIndex < 0 || AxisIndex >= Mask->Params.Get<FVertexMaskForgeBoundingBoxParams>().Axes.Num())
	{
		return;
	}
	// AUDITED: copies the CURRENT Params (every field byte/value-exact, including the other two axes and
	// bUseUnifiedBounds), mutates ONLY Axes[AxisIndex] via Mutator, then writes back through the existing
	// stack API -- never a second/cached copy of parameters retained across calls.
	FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
	Mutator(NewParams.Get<FVertexMaskForgeBoundingBoxParams>().Axes[AxisIndex]);
	DynamicLayerStack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, NewParams);
	OnDynamicLayerStackMutated();
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicBoundingBoxAxisRow(
	const FGuid LayerId, const FGuid ExpectedMaskInstanceId, const EVertexMaskForgeBoundsAxis Axis, const FText AxisLabel)
{
	const int32 AxisIndex = static_cast<int32>(Axis);

	// AUDITED: every Value_Lambda/IsChecked_Lambda below re-resolves LayerId's CURRENT stored Bounding Box
	// params fresh on every call (never a cached copy) -- if the layer/mask/params no longer match (e.g.
	// generator switched away), each accessor falls back to its own axis default rather than reading
	// garbage. Read-only; never mutates.
	auto GetAxisParams = [this, LayerId](const int32 InAxisIndex) -> FVertexMaskForgeAxisMaskParams
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		const FVertexMaskForgeBoundingBoxParams* BBoxParams = Mask ? Mask->Params.TryGet<FVertexMaskForgeBoundingBoxParams>() : nullptr;
		if (BBoxParams && InAxisIndex >= 0 && InAxisIndex < BBoxParams->Axes.Num())
		{
			return BBoxParams->Axes[InAxisIndex];
		}
		return FVertexMaskForgeAxisMaskParams();
	};

	return SNew(SVerticalBox)

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 6.f, 0.f, 2.f))
	[
		SNew(SCheckBox)
		.IsEnabled_Lambda([this, LayerId]() { return IsDynamicBoundingBoxLayerLocalSpaceCompatible(LayerId); })
		.IsChecked_Lambda([GetAxisParams, AxisIndex]()
		{
			return GetAxisParams(AxisIndex).bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId, AxisIndex](const ECheckBoxState NewState)
		{
			MutateDynamicBoundingBoxAxisParam(LayerId, ExpectedMaskInstanceId, AxisIndex, [NewState](FVertexMaskForgeAxisMaskParams& AxisParams)
			{
				AxisParams.bEnabled = (NewState == ECheckBoxState::Checked);
			});
		})
		.Content()
		[
			SNew(STextBlock)
			.Text(AxisLabel)
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
			SNew(STextBlock).Text(LOCTEXT("DynamicAxisPositionLabel", "Position"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.f, 0.f, 8.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicBoundingBoxLayerLocalSpaceCompatible(LayerId); })
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.Value_Lambda([GetAxisParams, AxisIndex]() { return GetAxisParams(AxisIndex).Position; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId, AxisIndex](const float NewValue)
			{
				MutateDynamicBoundingBoxAxisParam(LayerId, ExpectedMaskInstanceId, AxisIndex, [NewValue](FVertexMaskForgeAxisMaskParams& AxisParams)
				{
					AxisParams.Position = NewValue;
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			// Visible label only -- "Falloff", matching the Legacy control's own renamed label. The
			// underlying field remains TransitionWidth, unchanged (see that field's own doc comment).
			SNew(STextBlock).Text(LOCTEXT("DynamicAxisFalloffLabel", "Falloff"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.f, 0.f, 8.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicBoundingBoxLayerLocalSpaceCompatible(LayerId); })
			.MinDesiredWidth(52.f)
			.MinValue(0.001f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.Value_Lambda([GetAxisParams, AxisIndex]() { return GetAxisParams(AxisIndex).TransitionWidth; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId, AxisIndex](const float NewValue)
			{
				MutateDynamicBoundingBoxAxisParam(LayerId, ExpectedMaskInstanceId, AxisIndex, [NewValue](FVertexMaskForgeAxisMaskParams& AxisParams)
				{
					AxisParams.TransitionWidth = NewValue;
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.f, 0.f, 6.f, 0.f))
		[
			SNew(SCheckBox)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicBoundingBoxLayerLocalSpaceCompatible(LayerId); })
			.IsChecked_Lambda([GetAxisParams, AxisIndex]()
			{
				return GetAxisParams(AxisIndex).bInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId, AxisIndex](const ECheckBoxState NewState)
			{
				MutateDynamicBoundingBoxAxisParam(LayerId, ExpectedMaskInstanceId, AxisIndex, [NewState](FVertexMaskForgeAxisMaskParams& AxisParams)
				{
					AxisParams.bInvert = (NewState == ECheckBoxState::Checked);
				});
			})
			.Content()
			[
				SNew(STextBlock).Text(LOCTEXT("DynamicAxisInvertLabel", "Invert"))
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicBoundingBoxLayerLocalSpaceCompatible(LayerId); })
			.IsChecked_Lambda([GetAxisParams, AxisIndex]()
			{
				return GetAxisParams(AxisIndex).bMirror ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId, AxisIndex](const ECheckBoxState NewState)
			{
				MutateDynamicBoundingBoxAxisParam(LayerId, ExpectedMaskInstanceId, AxisIndex, [NewState](FVertexMaskForgeAxisMaskParams& AxisParams)
				{
					AxisParams.bMirror = (NewState == ECheckBoxState::Checked);
				});
			})
			.Content()
			[
				SNew(STextBlock).Text(LOCTEXT("DynamicAxisMirrorLabel", "Mirror"))
			]
		]
	];
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicBoundingBoxLayerParamsBlock(const FGuid LayerId, const FGuid ExpectedMaskInstanceId)
{
	return SNew(SVerticalBox)
	.Visibility_Lambda([this, LayerId]()
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		return (Mask && Mask->GeneratorType == EVertexMaskForgeGeneratorType::BoundingBox) ? EVisibility::Visible : EVisibility::Collapsed;
	})

	// AUDITED (M16-K.6D-8C-C): Local-space-only enforcement is NOT performed here or by any control below
	// -- it is enforced solely by VertexMaskForgeDynamicSourceTopologyComposition's own Pass 1 (rejects
	// World Space/Unified Bounds outright). This warning is a read-only, honest ECHO of that same
	// predicate (IsDynamicBoundingBoxLayerLocalSpaceCompatible), shown only when the layer's CURRENTLY
	// STORED data already requests something this checkpoint's UI cannot create through its own controls
	// (no World Space toggle, no Unified Bounds toggle exist below) -- never a data migration, never a
	// silent rewrite, never a reinterpretation as Local Space.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 0.f, 0.f, 2.f))
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Visibility_Lambda([this, LayerId]()
		{
			return IsDynamicBoundingBoxLayerLocalSpaceCompatible(LayerId) ? EVisibility::Collapsed : EVisibility::Visible;
		})
		.Text(LOCTEXT("DynamicLayerBoundingBoxUnsupportedStateWarning",
			"This layer's stored Bounding Box parameters request World Space or Unified Bounds. Dynamic Bounding Box currently supports Local Space only; axis controls below are disabled until this layer's data is edited back to a supported state (composition and Accept will reject this layer as-is)."))
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	[
		BuildDynamicBoundingBoxAxisRow(LayerId, ExpectedMaskInstanceId, EVertexMaskForgeBoundsAxis::X, LOCTEXT("DynamicAxisXLabel", "X"))
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	[
		BuildDynamicBoundingBoxAxisRow(LayerId, ExpectedMaskInstanceId, EVertexMaskForgeBoundsAxis::Y, LOCTEXT("DynamicAxisYLabel", "Y"))
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	[
		BuildDynamicBoundingBoxAxisRow(LayerId, ExpectedMaskInstanceId, EVertexMaskForgeBoundsAxis::Z, LOCTEXT("DynamicAxisZLabel", "Z"))
	];
}

bool SVertexMaskForgePanel::IsDynamicDirectionalNormalLayerLocalSpaceCompatible(const FGuid LayerId) const
{
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask || Mask->GeneratorType != EVertexMaskForgeGeneratorType::DirectionalNormal)
	{
		return true;
	}
	const FVertexMaskForgeDirectionalNormalParams* NormalParams = Mask->Params.TryGet<FVertexMaskForgeDirectionalNormalParams>();
	if (!NormalParams)
	{
		return true;
	}
	return NormalParams->Space == EVertexMaskForgeNormalSpace::Local;
}

void SVertexMaskForgePanel::MutateDynamicDirectionalNormalParam(
	const FGuid LayerId, const FGuid ExpectedMaskInstanceId,
	TFunctionRef<void(FVertexMaskForgeDirectionalNormalParams&)> Mutator)
{
	// AUDITED (M16-K.6D-8D-C): mirrors MutateDynamicBoundingBoxAxisParam's own six-step identity-validated
	// write path exactly -- mask exists, GeneratorType is still DirectionalNormal, Params is still the
	// DirectionalNormal payload type, and MaskInstanceId still matches what THIS widget was built for. Any
	// mismatch (cleared, reassigned to a different generator, or replaced by a newer instance) is a silent
	// no-op, never a fallback to another layer or a stale write.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask
		|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::DirectionalNormal
		|| !Mask->Params.IsType<FVertexMaskForgeDirectionalNormalParams>()
		|| Mask->MaskInstanceId != ExpectedMaskInstanceId)
	{
		return;
	}
	// AUDITED: copies the CURRENT Params (every field byte/value-exact, including Space -- never touched
	// here), mutates the copy via Mutator, then writes back through the existing stack API -- never a
	// second/cached copy of parameters retained across calls.
	FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
	Mutator(NewParams.Get<FVertexMaskForgeDirectionalNormalParams>());
	DynamicLayerStack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, NewParams);
	OnDynamicLayerStackMutated();
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicDirectionalNormalLayerParamsBlock(const FGuid LayerId, const FGuid ExpectedMaskInstanceId)
{
	// AUDITED: every Value_Lambda/IsChecked_Lambda/Text_Lambda below re-resolves LayerId's CURRENT stored
	// Directional Normal params fresh on every call (never a cached copy) -- if the layer/mask/params no
	// longer match (e.g. generator switched away), the accessor falls back to the authoritative default
	// (Local/PositiveZ/90/45/0/false) rather than reading garbage. Read-only; never mutates.
	auto GetParams = [this, LayerId]() -> FVertexMaskForgeDirectionalNormalParams
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		const FVertexMaskForgeDirectionalNormalParams* NormalParams = Mask ? Mask->Params.TryGet<FVertexMaskForgeDirectionalNormalParams>() : nullptr;
		return NormalParams ? *NormalParams : FVertexMaskForgeDirectionalNormalParams();
	};

	return SNew(SVerticalBox)
	.Visibility_Lambda([this, LayerId]()
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		return (Mask && Mask->GeneratorType == EVertexMaskForgeGeneratorType::DirectionalNormal) ? EVisibility::Visible : EVisibility::Collapsed;
	})

	// AUDITED (M16-K.6D-8D-C): Local-space-only enforcement is NOT performed here or by any control below
	// -- it is enforced solely by VertexMaskForgeDynamicSourceTopologyComposition's own Pass 1 (rejects
	// World Space outright). This warning is a read-only, honest ECHO of that same predicate
	// (IsDynamicDirectionalNormalLayerLocalSpaceCompatible), shown only when the layer's CURRENTLY STORED
	// data already requests something this checkpoint's UI cannot create through its own controls (no
	// Space toggle exists below) -- never a data migration, never a silent rewrite, never a
	// reinterpretation as Local Space.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 0.f, 0.f, 2.f))
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Visibility_Lambda([this, LayerId]()
		{
			return IsDynamicDirectionalNormalLayerLocalSpaceCompatible(LayerId) ? EVisibility::Collapsed : EVisibility::Visible;
		})
		.Text(LOCTEXT("DynamicLayerDirectionalNormalUnsupportedStateWarning",
			"This layer's stored Directional Normal parameters request World Space. Dynamic Directional Normal currently supports Local Space only; controls below are disabled until this layer's data is edited back to a supported state (composition and Accept will reject this layer as-is)."))
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicDirectionalNormalDirectionLabel", "Direction"))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SComboBox<TSharedPtr<EVertexMaskForgeNormalDirection>>)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicDirectionalNormalLayerLocalSpaceCompatible(LayerId); })
			.OptionsSource(&NormalDirectionOptions)
			.OnGenerateWidget_Lambda([](TSharedPtr<EVertexMaskForgeNormalDirection> InOption)
			{
				return SNew(STextBlock).Text(InOption.IsValid() ? VertexMaskForgePanel::GetNormalDirectionLabel(*InOption) : FText::GetEmpty());
			})
			.OnSelectionChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](TSharedPtr<EVertexMaskForgeNormalDirection> NewSelection, ESelectInfo::Type)
			{
				if (!NewSelection.IsValid())
				{
					return;
				}
				const EVertexMaskForgeNormalDirection NewDirection = *NewSelection;
				MutateDynamicDirectionalNormalParam(LayerId, ExpectedMaskInstanceId, [NewDirection](FVertexMaskForgeDirectionalNormalParams& Params)
				{
					Params.Direction = NewDirection;
				});
			})
			.Content()
			[
				SNew(STextBlock).Text_Lambda([GetParams]() { return VertexMaskForgePanel::GetNormalDirectionLabel(GetParams().Direction); })
			]
		]
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicDirectionalNormalAngleLabel", "Angle"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicDirectionalNormalLayerLocalSpaceCompatible(LayerId); })
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(180.0f)
			.Delta(1.0f)
			.Value_Lambda([GetParams]() { return GetParams().Angle; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				// Minimum invariant maintenance only: reducing Angle below the currently stored Falloff
				// clamps Falloff down to match; no other field is touched.
				MutateDynamicDirectionalNormalParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeDirectionalNormalParams& Params)
				{
					Params.Angle = NewValue;
					if (Params.Falloff > Params.Angle)
					{
						Params.Falloff = Params.Angle;
					}
				});
			})
		]
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicDirectionalNormalFalloffLabel", "Falloff"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicDirectionalNormalLayerLocalSpaceCompatible(LayerId); })
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue_Lambda([GetParams]() -> TOptional<float> { return GetParams().Angle; })
			.Delta(1.0f)
			.Value_Lambda([GetParams]() { return GetParams().Falloff; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				// Clamp a newly requested Falloff to the CURRENT authoritative Angle (Falloff <= Angle) --
				// no other field is touched.
				MutateDynamicDirectionalNormalParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeDirectionalNormalParams& Params)
				{
					Params.Falloff = FMath::Min(NewValue, Params.Angle);
				});
			})
		]
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicDirectionalNormalBlurLabel", "Blur"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.IsEnabled_Lambda([this, LayerId]() { return IsDynamicDirectionalNormalLayerLocalSpaceCompatible(LayerId); })
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(10.0f)
			.Delta(0.1f)
			.Value_Lambda([GetParams]() { return GetParams().Blur; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicDirectionalNormalParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeDirectionalNormalParams& Params)
				{
					Params.Blur = NewValue;
				});
			})
		]
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
	[
		SNew(SCheckBox)
		.IsEnabled_Lambda([this, LayerId]() { return IsDynamicDirectionalNormalLayerLocalSpaceCompatible(LayerId); })
		.IsChecked_Lambda([GetParams]()
		{
			return GetParams().bInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const ECheckBoxState NewState)
		{
			MutateDynamicDirectionalNormalParam(LayerId, ExpectedMaskInstanceId, [NewState](FVertexMaskForgeDirectionalNormalParams& Params)
			{
				Params.bInvert = (NewState == ECheckBoxState::Checked);
			});
		})
		.Content()
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicDirectionalNormalInvertLabel", "Invert"))
		]
	];
}

void SVertexMaskForgePanel::MutateDynamicCurvatureParam(
	const FGuid LayerId, const FGuid ExpectedMaskInstanceId,
	TFunctionRef<void(FVertexMaskForgeCurvatureParams&)> Mutator)
{
	// AUDITED (M16-K.6D-8E-C): mirrors MutateDynamicDirectionalNormalParam's own six-step identity-
	// validated write path exactly -- mask exists, GeneratorType is still Curvature, Params is still the
	// Curvature payload type, and MaskInstanceId still matches what THIS widget was built for. Any
	// mismatch (cleared, reassigned to a different generator, or replaced by a newer instance) is a silent
	// no-op, never a fallback to another layer or a stale write.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask
		|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::Curvature
		|| !Mask->Params.IsType<FVertexMaskForgeCurvatureParams>()
		|| Mask->MaskInstanceId != ExpectedMaskInstanceId)
	{
		return;
	}
	// AUDITED: copies the CURRENT Params (every field byte/value-exact), mutates the copy via Mutator
	// (including the Levels Min/Max coupled-invariant maintenance the Angle/Falloff callbacks below
	// perform entirely inside their own Mutator, exactly like Directional Normal's own established
	// pattern), then writes back through the existing stack API in ONE call -- never a second/cached copy
	// of parameters retained across calls, never a second callback-driven write.
	FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
	Mutator(NewParams.Get<FVertexMaskForgeCurvatureParams>());
	DynamicLayerStack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, NewParams);
	OnDynamicLayerStackMutated();
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicCurvatureLayerParamsBlock(const FGuid LayerId, const FGuid ExpectedMaskInstanceId)
{
	// AUDITED: every Value_Lambda/IsChecked_Lambda/Text_Lambda below re-resolves LayerId's CURRENT stored
	// Curvature params fresh on every call (never a cached copy) -- if the layer/mask/params no longer
	// match (e.g. generator switched away), the accessor falls back to the authoritative default
	// (Concave/Multiplier=1/Blur=0/Levels=[0,1]/not inverted) rather than reading garbage. Read-only;
	// never mutates.
	auto GetParams = [this, LayerId]() -> FVertexMaskForgeCurvatureParams
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		const FVertexMaskForgeCurvatureParams* CurvatureParams = Mask ? Mask->Params.TryGet<FVertexMaskForgeCurvatureParams>() : nullptr;
		return CurvatureParams ? *CurvatureParams : FVertexMaskForgeCurvatureParams();
	};

	return SNew(SVerticalBox)
	.Visibility_Lambda([this, LayerId]()
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		return (Mask && Mask->GeneratorType == EVertexMaskForgeGeneratorType::Curvature) ? EVisibility::Visible : EVisibility::Collapsed;
	})

	// Type -- reuses the existing, panel-wide, immutable CurvatureTypeOptions array (already populated in
	// Construct() for the Legacy Curvature combo) as pure presentation data, exactly how the Directional
	// Normal block reuses NormalDirectionOptions -- never a new UI-owned enum, never authoritative state.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicCurvatureTypeLabel", "Curvature Type:"))
			.ToolTipText(LOCTEXT("DynamicCurvatureTypeTooltip",
				"Convex: only outward edges/bulges. Concave: only cavities/creases. Both: both signs, without cancelling out."))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SComboBox<TSharedPtr<EVertexMaskForgeCurvatureType>>)
			.OptionsSource(&CurvatureTypeOptions)
			.OnGenerateWidget_Lambda([](TSharedPtr<EVertexMaskForgeCurvatureType> InOption)
			{
				return SNew(STextBlock).Text(InOption.IsValid() ? VertexMaskForgePanel::GetCurvatureTypeLabel(*InOption) : FText::GetEmpty());
			})
			.OnSelectionChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](TSharedPtr<EVertexMaskForgeCurvatureType> NewSelection, ESelectInfo::Type)
			{
				if (!NewSelection.IsValid())
				{
					return;
				}
				const EVertexMaskForgeCurvatureType NewType = *NewSelection;
				MutateDynamicCurvatureParam(LayerId, ExpectedMaskInstanceId, [NewType](FVertexMaskForgeCurvatureParams& Params)
				{
					Params.Type = NewType;
				});
			})
			.Content()
			[
				SNew(STextBlock).Text_Lambda([GetParams]() { return VertexMaskForgePanel::GetCurvatureTypeLabel(GetParams().Type); })
			]
		]
	]

	// Multiplier -- [0,10], Delta 0.01, matching the Legacy widget's own range/step exactly.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicCurvatureMultiplierLabel", "Multiplier"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(10.0f)
			.Delta(0.01f)
			.Value_Lambda([GetParams]() { return GetParams().Multiplier; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicCurvatureParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeCurvatureParams& Params)
				{
					Params.Multiplier = FMath::Max(NewValue, 0.0f);
				});
			})
		]
	]

	// Blur -- [0,10], Delta 0.01, retains full float precision (fractional iteration blend), matching the
	// Legacy widget's own range/step/tooltip exactly.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicCurvatureBlurLabel", "Blur"))
			.ToolTipText(LOCTEXT("DynamicCurvatureBlurTooltip",
				"Topological smoothing of the Curvature mask. Whole number = full iterations; fractional part blends toward one more."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(10.0f)
			.Delta(0.01f)
			.Value_Lambda([GetParams]() { return GetParams().Blur; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicCurvatureParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeCurvatureParams& Params)
				{
					Params.Blur = FMath::Clamp(NewValue, 0.0f, 10.0f);
				});
			})
		]
	]

	// Levels Min / Levels Max -- [0,1], Delta 0.01, matching the Legacy widgets' own range/step/tooltips
	// exactly. Coupled invariant (LevelsMin <= LevelsMax) is maintained entirely inside each callback's own
	// Mutator -- the edited field is always assigned first and preserved, the OTHER field is raised/lowered
	// only if the pair would otherwise become invalid -- committed through the SAME single
	// MutateDynamicCurvatureParam call (one SetLayerMaskParams write, one OnDynamicLayerStackMutated
	// notification), never a second callback-driven write. A malformed stored pair (LevelsMin > LevelsMax,
	// unreachable through this UI but not otherwise precluded) displays safely (ApplyCurvatureLevels' own
	// Max(LevelsMax-LevelsMin, Epsilon) denominator never divides by zero) and becomes valid the moment the
	// artist edits either control.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicCurvatureLevelsMinLabel", "Levels Min"))
			.ToolTipText(LOCTEXT("DynamicCurvatureLevelsMinTooltip", "Values at or below this threshold become black."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicCurvatureLevelsMinTooltip", "Values at or below this threshold become black."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMin; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicCurvatureParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeCurvatureParams& Params)
				{
					Params.LevelsMin = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMin > Params.LevelsMax)
					{
						Params.LevelsMax = Params.LevelsMin;
					}
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicCurvatureLevelsMaxLabel", "Levels Max"))
			.ToolTipText(LOCTEXT("DynamicCurvatureLevelsMaxTooltip", "Values at or above this threshold become white."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicCurvatureLevelsMaxTooltip", "Values at or above this threshold become white."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMax; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicCurvatureParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeCurvatureParams& Params)
				{
					Params.LevelsMax = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMax < Params.LevelsMin)
					{
						Params.LevelsMin = Params.LevelsMax;
					}
				});
			})
		]
	]

	// Invert
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([GetParams]()
		{
			return GetParams().bInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const ECheckBoxState NewState)
		{
			MutateDynamicCurvatureParam(LayerId, ExpectedMaskInstanceId, [NewState](FVertexMaskForgeCurvatureParams& Params)
			{
				Params.bInvert = (NewState == ECheckBoxState::Checked);
			});
		})
		.Content()
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicCurvatureInvertLabel", "Invert"))
		]
	];
}

void SVertexMaskForgePanel::MutateDynamicAmbientOcclusionParam(
	const FGuid LayerId, const FGuid ExpectedMaskInstanceId,
	TFunctionRef<void(FVertexMaskForgeAmbientOcclusionParams&)> Mutator)
{
	// AUDITED (M16-K.6D-8G-F): mirrors MutateDynamicCurvatureParam's own six-step identity-validated write
	// path exactly -- mask exists, GeneratorType is still AmbientOcclusion, Params is still the Ambient
	// Occlusion payload type, and MaskInstanceId still matches what THIS widget was built for. Any mismatch
	// (cleared, reassigned to a different generator, or replaced by a newer instance) is a silent no-op,
	// never a fallback to another layer or a stale write. Never touches
	// DynamicSourceTopologyAOCachesByLayerId -- the backend's own dispatch branch (M16-K.6D-8G-D) detects
	// raw-parameter changes and recomputes on its own; this widget only ever writes the recipe payload.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask
		|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::AmbientOcclusion
		|| !Mask->Params.IsType<FVertexMaskForgeAmbientOcclusionParams>()
		|| Mask->MaskInstanceId != ExpectedMaskInstanceId)
	{
		return;
	}
	// AUDITED: copies the CURRENT Params (every field byte/value-exact), mutates the copy via Mutator
	// (including the Levels Min/Max coupled-invariant maintenance the Levels callbacks below perform
	// entirely inside their own Mutator, exactly like Curvature's own established pattern), then writes
	// back through the existing stack API in ONE call -- never a second/cached copy of parameters retained
	// across calls, never a second callback-driven write.
	FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
	Mutator(NewParams.Get<FVertexMaskForgeAmbientOcclusionParams>());
	DynamicLayerStack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, NewParams);
	OnDynamicLayerStackMutated();
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicAmbientOcclusionLayerParamsBlock(const FGuid LayerId, const FGuid ExpectedMaskInstanceId)
{
	// AUDITED: every Value_Lambda/IsChecked_Lambda below re-resolves LayerId's CURRENT stored Ambient
	// Occlusion params fresh on every call (never a cached copy) -- if the layer/mask/params no longer
	// match (e.g. generator switched away), the accessor falls back to the authoritative committed default
	// (Samples=16/MaxDistance=100/Bias=0.1/Levels=[0,1]/not inverted -- FVertexMaskForgeAmbientOcclusionParams'
	// own default member initializers) rather than reading garbage. Read-only; never mutates.
	auto GetParams = [this, LayerId]() -> FVertexMaskForgeAmbientOcclusionParams
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		const FVertexMaskForgeAmbientOcclusionParams* AOParams = Mask ? Mask->Params.TryGet<FVertexMaskForgeAmbientOcclusionParams>() : nullptr;
		return AOParams ? *AOParams : FVertexMaskForgeAmbientOcclusionParams();
	};

	return SNew(SVerticalBox)
	.Visibility_Lambda([this, LayerId]()
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		return (Mask && Mask->GeneratorType == EVertexMaskForgeGeneratorType::AmbientOcclusion) ? EVisibility::Visible : EVisibility::Collapsed;
	})

	// Samples -- [8,256], Delta 1, matching Legacy AOSamples' own range/step exactly. RAW generation
	// parameter: the AO backend's own persistent cache (DynamicSourceTopologyAOCachesByLayerId, keyed by
	// this layer's stable LayerId) detects this change on the next recomposition and recomputes raw values
	// -- this control never touches the cache map directly.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicAOSamplesLabel", "Samples"))
			.ToolTipText(LOCTEXT("DynamicAOSamplesTooltip",
				"Number of hemisphere raycast samples per element. Higher values are smoother but slower; "
				"the live preview always uses this full value."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<int32>)
			.MinDesiredWidth(52.f)
			.MinValue(8)
			.MaxValue(256)
			.Delta(1)
			.ToolTipText(LOCTEXT("DynamicAOSamplesTooltip",
				"Number of hemisphere raycast samples per element. Higher values are smoother but slower; "
				"the live preview always uses this full value."))
			.Value_Lambda([GetParams]() { return GetParams().Samples; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const int32 NewValue)
			{
				MutateDynamicAmbientOcclusionParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeAmbientOcclusionParams& Params)
				{
					Params.Samples = FMath::Clamp(NewValue, 8, 256);
				});
			})
		]
	]

	// Max Distance -- (0.01,10000], Delta 1.0, matching Legacy AOMaxDistance's own range/step exactly. RAW
	// generation parameter -- see Samples' own comment above for the cache-ownership contract.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicAOMaxDistanceLabel", "Max Distance"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(64.f)
			.MinValue(0.01f)
			.MaxValue(10000.0f)
			.Delta(1.0f)
			.Value_Lambda([GetParams]() { return GetParams().MaxDistance; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicAmbientOcclusionParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeAmbientOcclusionParams& Params)
				{
					Params.MaxDistance = FMath::Clamp(NewValue, 0.01f, 10000.0f);
				});
			})
		]
	]

	// Bias -- [0.001,10.0], Delta 0.01, matching Legacy AOBias' own range/step exactly. RAW generation
	// parameter -- see Samples' own comment above for the cache-ownership contract.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicAOBiasLabel", "Bias"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.001f)
			.MaxValue(10.0f)
			.Delta(0.01f)
			.Value_Lambda([GetParams]() { return GetParams().Bias; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicAmbientOcclusionParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeAmbientOcclusionParams& Params)
				{
					Params.Bias = FMath::Clamp(NewValue, 0.001f, 10.0f);
				});
			})
		]
	]

	// Levels Min / Levels Max -- [0,1], Delta 0.01, matching Legacy AOLevelsMin/AOLevelsMax's own
	// range/step/tooltips exactly. PURELY COMPOSITIONAL (post-processing) -- reuses valid raw AO values,
	// never touches the cache map, mirrors Curvature's own coupled-invariant maintenance pattern exactly
	// (the edited field is always assigned first and preserved; the OTHER field is raised/lowered only if
	// the pair would otherwise become invalid, committed through the SAME single
	// MutateDynamicAmbientOcclusionParam call).
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicAOLevelsMinLabel", "Levels Min"))
			.ToolTipText(LOCTEXT("DynamicAOLevelsMinTooltip", "Values at or below this threshold become black."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicAOLevelsMinTooltip", "Values at or below this threshold become black."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMin; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicAmbientOcclusionParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeAmbientOcclusionParams& Params)
				{
					Params.LevelsMin = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMin > Params.LevelsMax)
					{
						Params.LevelsMax = Params.LevelsMin;
					}
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicAOLevelsMaxLabel", "Levels Max"))
			.ToolTipText(LOCTEXT("DynamicAOLevelsMaxTooltip", "Values at or above this threshold become white."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicAOLevelsMaxTooltip", "Values at or above this threshold become white."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMax; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicAmbientOcclusionParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeAmbientOcclusionParams& Params)
				{
					Params.LevelsMax = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMax < Params.LevelsMin)
					{
						Params.LevelsMin = Params.LevelsMax;
					}
				});
			})
		]
	]

	// Invert -- PURELY COMPOSITIONAL (post-processing), matching Legacy AOInvert's own label/style exactly.
	// Reuses valid raw AO values, applied exactly once inside the existing AO generator's own
	// ApplyAOLevelsAndInvert (never in this widget, never twice).
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([GetParams]()
		{
			return GetParams().bInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const ECheckBoxState NewState)
		{
			MutateDynamicAmbientOcclusionParam(LayerId, ExpectedMaskInstanceId, [NewState](FVertexMaskForgeAmbientOcclusionParams& Params)
			{
				Params.bInvert = (NewState == ECheckBoxState::Checked);
			});
		})
		.Content()
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicAOInvertLabel", "Invert"))
		]
	];
}

void SVertexMaskForgePanel::MutateDynamicNoiseParam(
	const FGuid LayerId, const FGuid ExpectedMaskInstanceId,
	TFunctionRef<void(FVertexMaskForgeNoiseParams&)> Mutator)
{
	// AUDITED (M16-K.6D-8F-C): mirrors MutateDynamicCurvatureParam's own six-step identity-validated write
	// path exactly -- mask exists, GeneratorType is still Noise, Params is still the Noise payload type,
	// and MaskInstanceId still matches what THIS widget was built for. Any mismatch (cleared, reassigned
	// to a different generator, or replaced by a newer instance) is a silent no-op, never a fallback to
	// another layer or a stale write.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask
		|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::Noise
		|| !Mask->Params.IsType<FVertexMaskForgeNoiseParams>()
		|| Mask->MaskInstanceId != ExpectedMaskInstanceId)
	{
		return;
	}
	// AUDITED: copies the CURRENT Params (every field byte/value-exact), mutates the copy via Mutator
	// (including the Levels Min/Max coupled-invariant maintenance the Levels callbacks below perform
	// entirely inside their own Mutator, exactly like Curvature's own established pattern), then writes
	// back through the existing stack API in ONE call -- never a second/cached copy of parameters retained
	// across calls, never a second callback-driven write.
	FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
	Mutator(NewParams.Get<FVertexMaskForgeNoiseParams>());
	DynamicLayerStack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, NewParams);
	OnDynamicLayerStackMutated();
}

bool SVertexMaskForgePanel::IsDynamicNoiseScaleAxesLocked(const FGuid LayerId) const
{
	// AUDITED (M16-K.6D-8F-C.1): pure lookup, never inserts -- FindRef defaults to false (unlocked) for a
	// missing entry, exactly matching Legacy's own bNoiseScaleAxesLocked=false default, without ever
	// growing the map merely because a control was rendered/queried.
	return DynamicNoiseScaleAxesLockedByLayerId.FindRef(LayerId);
}

void SVertexMaskForgePanel::OnDynamicNoiseScaleAxesLockChanged(const FGuid LayerId, const FGuid ExpectedMaskInstanceId, const ECheckBoxState NewState)
{
	// AUDITED: the flag itself is UI-only and harmless to write unconditionally -- unlike the Noise
	// payload, no identity validation is needed to record the artist's own checkbox intent for this
	// LayerId. Mirrors Legacy's OnNoiseScaleAxesLockChanged exactly: turning OFF only flips this flag,
	// never touches Scale, never invalidates/recomposes.
	const bool bNewLocked = (NewState == ECheckBoxState::Checked);
	DynamicNoiseScaleAxesLockedByLayerId.Add(LayerId, bNewLocked);

	if (!bNewLocked)
	{
		return;
	}

	// AUDITED (M16-K.6D-8F-D.1): turning ON snaps Y/Z to the CURRENT X immediately, per Legacy's own
	// contract -- via VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock, the SAME production
	// function this module's own automation tests call. Applied here FIRST to a plain local copy purely
	// to DECIDE whether a write is actually needed (already-equal axes must produce no payload write and
	// no recomposition, matching Legacy's own "no-op beyond the flag" case) -- this decision-read never
	// mutates the stack. When a write IS needed, the SAME idempotent function is applied again, this time
	// inside MutateDynamicNoiseParam's own six-step identity-validated Mutator, so an unknown/stale/
	// reassigned layer remains a safe no-op exactly like every other Noise control, and exactly one
	// SetLayerMaskParams + one OnDynamicLayerStackMutated() occur.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	const FVertexMaskForgeNoiseParams* CurrentParams = Mask ? Mask->Params.TryGet<FVertexMaskForgeNoiseParams>() : nullptr;
	if (!CurrentParams)
	{
		return;
	}
	FVertexMaskForgeNoiseParams DecisionCopy = *CurrentParams;
	if (!VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(DecisionCopy))
	{
		return;
	}

	MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [](FVertexMaskForgeNoiseParams& Params)
	{
		VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(Params);
	});
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicNoiseLayerParamsBlock(const FGuid LayerId, const FGuid ExpectedMaskInstanceId)
{
	// AUDITED: every Value_Lambda/IsChecked_Lambda/Text_Lambda/IsEnabled_Lambda below re-resolves LayerId's
	// CURRENT stored Noise params fresh on every call (never a cached copy) -- if the layer/mask/params no
	// longer match (e.g. generator switched away), the accessor falls back to the authoritative default
	// (FractalPerlin/Scale=1/Offset=0/Seed=0/Octaves=4/Roughness=0.5/Lacunarity=2/TurbulenceStrength=0.5/
	// Multiplier=1/Blur=0/Levels=[0,1]/not inverted -- FVertexMaskForgeNoiseParams' own default member
	// initializers) rather than reading garbage. Read-only; never mutates.
	auto GetParams = [this, LayerId]() -> FVertexMaskForgeNoiseParams
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		const FVertexMaskForgeNoiseParams* NoiseParams = Mask ? Mask->Params.TryGet<FVertexMaskForgeNoiseParams>() : nullptr;
		return NoiseParams ? *NoiseParams : FVertexMaskForgeNoiseParams();
	};

	return SNew(SVerticalBox)
	.Visibility_Lambda([this, LayerId]()
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		return (Mask && Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise) ? EVisibility::Visible : EVisibility::Collapsed;
	})

	// --- Noise Generation group -----------------------------------------------------------------------

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(STextBlock)
		.Text(LOCTEXT("DynamicNoiseGenerationGroupLabel", "Noise Generation"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
	]

	// Type -- reuses the existing, panel-wide, immutable NoiseTypeOptions array (already populated in
	// Construct() for the Legacy Noise combo) as pure presentation data, exactly how the Curvature block
	// reuses CurvatureTypeOptions -- never a new UI-owned enum, never authoritative state.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseTypeLabel", "Noise Type:"))
			.ToolTipText(LOCTEXT("DynamicNoiseTypeTooltip",
				"Perlin: a single noise octave. Fractal Perlin (FBM): several octaves summed for more detail."))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SComboBox<TSharedPtr<EVertexMaskForgeNoiseType>>)
			.OptionsSource(&NoiseTypeOptions)
			.OnGenerateWidget_Lambda([](TSharedPtr<EVertexMaskForgeNoiseType> InOption)
			{
				return SNew(STextBlock).Text(InOption.IsValid() ? VertexMaskForgePanel::GetNoiseTypeLabel(*InOption) : FText::GetEmpty());
			})
			.OnSelectionChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](TSharedPtr<EVertexMaskForgeNoiseType> NewSelection, ESelectInfo::Type)
			{
				if (!NewSelection.IsValid())
				{
					return;
				}
				const EVertexMaskForgeNoiseType NewType = *NewSelection;
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewType](FVertexMaskForgeNoiseParams& Params)
				{
					Params.Type = NewType;
				});
			})
			.Content()
			[
				SNew(STextBlock).Text_Lambda([GetParams]() { return VertexMaskForgePanel::GetNoiseTypeLabel(GetParams().Type); })
			]
		]
	]

	// Scale X/Y/Z -- [0.001,1000], Delta 0.01, matching the Legacy widgets' own range/step exactly.
	// M16-K.6D-8F-C.1: "Lock Axes" reproduces Legacy's own bNoiseScaleAxesLocked workflow convenience
	// (X is the sole master; absolute copy, not ratio-preserving; see
	// DynamicNoiseScaleAxesLockedByLayerId's own doc comment) at per-layer granularity via a panel-owned,
	// LayerId-keyed map -- never as an 18th authoritative field (FVertexMaskForgeNoiseParams' own doc
	// comment already excludes it for the same reason Legacy does).
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicNoiseScaleLabel", "Scale X/Y/Z"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 4.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.001f)
			.MaxValue(1000.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicNoiseScaleXTooltip", "Frequency multiplier along local X. 1.0 is approximately one noise unit per meter."))
			.Value_Lambda([GetParams]() { return GetParams().ScaleX; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				// AUDITED (M16-K.6D-8F-D.1): single atomic entry point for Scale X, mirroring Legacy's own
				// OnNoiseScaleXChanged -- delegates the clamp/assign/conditional-Y-Z-sync algorithm entirely
				// to VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit (the SAME production function this
				// module's own automation tests call), invoked exactly once inside MutateDynamicNoiseParam's
				// own Mutator, so exactly one SetLayerMaskParams write and one OnDynamicLayerStackMutated()
				// fire per edit, never three, and no second handwritten copy of this branching exists here.
				const bool bLocked = IsDynamicNoiseScaleAxesLocked(LayerId);
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue, bLocked](FVertexMaskForgeNoiseParams& Params)
				{
					VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit(Params, NewValue, bLocked);
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.001f)
			.MaxValue(1000.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicNoiseScaleYTooltip", "Frequency multiplier along local Y."))
			.IsEnabled_Lambda([this, LayerId]() { return !IsDynamicNoiseScaleAxesLocked(LayerId); })
			.Value_Lambda([GetParams]() { return GetParams().ScaleY; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				// Defensive: Y is disabled while locked, so this is unreachable through the real UI -- but a
				// stale/programmatic callback must not violate the locked invariant.
				if (IsDynamicNoiseScaleAxesLocked(LayerId))
				{
					return;
				}
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.ScaleY = FMath::Max(NewValue, 0.001f);
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.001f)
			.MaxValue(1000.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicNoiseScaleZTooltip", "Frequency multiplier along local Z."))
			.IsEnabled_Lambda([this, LayerId]() { return !IsDynamicNoiseScaleAxesLocked(LayerId); })
			.Value_Lambda([GetParams]() { return GetParams().ScaleZ; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				// Defensive: Z is disabled while locked, so this is unreachable through the real UI -- but a
				// stale/programmatic callback must not violate the locked invariant.
				if (IsDynamicNoiseScaleAxesLocked(LayerId))
				{
					return;
				}
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.ScaleZ = FMath::Max(NewValue, 0.001f);
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("DynamicNoiseScaleAxesLockedTooltip", "Use Scale X for all three axes."))
			.IsChecked_Lambda([this, LayerId]()
			{
				return IsDynamicNoiseScaleAxesLocked(LayerId) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const ECheckBoxState NewState)
			{
				OnDynamicNoiseScaleAxesLockChanged(LayerId, ExpectedMaskInstanceId, NewState);
			})
			.Content()
			[
				SNew(STextBlock).Text(LOCTEXT("DynamicNoiseScaleAxesLockedLabel", "Lock Axes"))
			]
		]
	]

	// Offset X/Y/Z -- [-100000,100000], Delta 0.1, matching the Legacy widgets' own range/step exactly.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicNoiseOffsetLabel", "Offset X/Y/Z"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 4.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(-100000.0f)
			.MaxValue(100000.0f)
			.Delta(0.1f)
			.ToolTipText(LOCTEXT("DynamicNoiseOffsetXTooltip", "Domain offset along X, in noise space."))
			.Value_Lambda([GetParams]() { return GetParams().OffsetX; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.OffsetX = NewValue;
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(-100000.0f)
			.MaxValue(100000.0f)
			.Delta(0.1f)
			.ToolTipText(LOCTEXT("DynamicNoiseOffsetYTooltip", "Domain offset along Y, in noise space."))
			.Value_Lambda([GetParams]() { return GetParams().OffsetY; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.OffsetY = NewValue;
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(-100000.0f)
			.MaxValue(100000.0f)
			.Delta(0.1f)
			.ToolTipText(LOCTEXT("DynamicNoiseOffsetZTooltip", "Domain offset along Z, in noise space."))
			.Value_Lambda([GetParams]() { return GetParams().OffsetZ; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.OffsetZ = NewValue;
				});
			})
		]
	]

	// Seed -- int32, [-2147483647,2147483647], Delta 1, matching the Legacy widget's own range/step exactly.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicNoiseSeedLabel", "Seed"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<int32>)
			.MinDesiredWidth(64.f)
			.MinValue(-2147483647)
			.MaxValue(2147483647)
			.Delta(1)
			.Value_Lambda([GetParams]() { return GetParams().Seed; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const int32 NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.Seed = NewValue;
				});
			})
		]
	]

	// Octaves/Roughness/Lacunarity -- multi-octave types only (FractalPerlin, Billow, Ridged, Turbulence,
	// Alligator), gated via NoiseTypeUsesFractalParameters (a parameterized extraction of the Legacy
	// panel's own UsesFractalParameters() member) -- disabled, not hidden, for single-sample/cellular
	// types, matching the Legacy widgets' own gating contract exactly. Ranges/steps also match the Legacy
	// widgets exactly: Octaves int32 [1,8] Delta 1; Roughness float [0,1] Delta 0.01; Lacunarity float
	// [1,10] Delta 0.01.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseOctavesLabel", "Octaves"))
			.ToolTipText(LOCTEXT("DynamicNoiseOctavesTooltip", "Multi-octave types only (Fractal Perlin, Billow, Ridged, Turbulence, Alligator)."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
		[
			SNew(SSpinBox<int32>)
			.MinDesiredWidth(44.f)
			.MinValue(1)
			.MaxValue(8)
			.Delta(1)
			.IsEnabled_Lambda([GetParams]() { return VertexMaskForgePanel::NoiseTypeUsesFractalParameters(GetParams().Type); })
			.Value_Lambda([GetParams]() { return GetParams().Octaves; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const int32 NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.Octaves = FMath::Clamp(NewValue, 1, 8);
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseRoughnessLabel", "Roughness"))
			.ToolTipText(LOCTEXT("DynamicNoiseRoughnessTooltip", "Per-octave amplitude multiplier. Multi-octave types only."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.IsEnabled_Lambda([GetParams]() { return VertexMaskForgePanel::NoiseTypeUsesFractalParameters(GetParams().Type); })
			.Value_Lambda([GetParams]() { return GetParams().Roughness; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.Roughness = FMath::Clamp(NewValue, 0.0f, 1.0f);
				});
			})
		]
	]

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseLacunarityLabel", "Lacunarity"))
			.ToolTipText(LOCTEXT("DynamicNoiseLacunarityTooltip", "Per-octave frequency multiplier. Multi-octave types only."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(1.0f)
			.MaxValue(10.0f)
			.Delta(0.01f)
			.IsEnabled_Lambda([GetParams]() { return VertexMaskForgePanel::NoiseTypeUsesFractalParameters(GetParams().Type); })
			.Value_Lambda([GetParams]() { return GetParams().Lacunarity; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.Lacunarity = FMath::Max(NewValue, 1.0f);
				});
			})
		]

		// Turbulence Strength -- [0,5], Delta 0.01, matching the Legacy widget's own range/step exactly.
		// Enabled only when Type == Turbulence, same contract as the Legacy widget.
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(12.f, 0.f, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseTurbulenceStrengthLabel", "Turbulence Strength"))
			.ToolTipText(LOCTEXT("DynamicNoiseTurbulenceStrengthTooltip", "Domain-warp displacement strength, in noise space. Turbulence only."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(5.0f)
			.Delta(0.01f)
			.IsEnabled_Lambda([GetParams]() { return GetParams().Type == EVertexMaskForgeNoiseType::Turbulence; })
			.Value_Lambda([GetParams]() { return GetParams().TurbulenceStrength; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.TurbulenceStrength = FMath::Clamp(NewValue, 0.0f, 5.0f);
				});
			})
		]
	]

	// Blur -- [0,1], Delta 0.01. NOTE: this range intentionally matches Legacy Noise's OWN Blur range
	// ([0,1]), NOT Curvature's Dynamic Blur range ([0,10]) -- the two generators' Blur semantics/scales
	// differ (confirmed via Legacy Noise's own widget, SVertexMaskForgePanel.cpp NoiseBlurLabel row).
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseBlurLabel", "Blur"))
			.ToolTipText(LOCTEXT("DynamicNoiseBlurTooltip", "Smooths the procedural noise field before Multiplier and Levels are applied."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicNoiseBlurTooltip", "Smooths the procedural noise field before Multiplier and Levels are applied."))
			.Value_Lambda([GetParams]() { return GetParams().Blur; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.Blur = FMath::Clamp(NewValue, 0.0f, 1.0f);
				});
			})
		]
	]

	// --- Artistic Adjustment group ---------------------------------------------------------------------

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 6.f, 0.f, 2.f))
	[
		SNew(STextBlock)
		.Text(LOCTEXT("DynamicNoiseArtisticAdjustmentGroupLabel", "Artistic Adjustment"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
	]

	// Multiplier -- [0,10], Delta 0.01, matching the Legacy widget's own range/step exactly.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicNoiseMultiplierLabel", "Multiplier"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(10.0f)
			.Delta(0.01f)
			.Value_Lambda([GetParams]() { return GetParams().Multiplier; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.Multiplier = FMath::Max(NewValue, 0.0f);
				});
			})
		]
	]

	// Levels Min / Levels Max -- [0,1], Delta 0.01, matching the Legacy widgets' own range/step/tooltips
	// exactly. Coupled invariant (LevelsMin <= LevelsMax) is maintained entirely inside each callback's own
	// Mutator, mirroring Curvature's own established pattern exactly -- the edited field is always assigned
	// first and preserved, the OTHER field is raised/lowered only if the pair would otherwise become
	// invalid -- committed through the SAME single MutateDynamicNoiseParam call (one SetLayerMaskParams
	// write, one OnDynamicLayerStackMutated notification), never a second callback-driven write.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseLevelsMinLabel", "Levels Min"))
			.ToolTipText(LOCTEXT("DynamicNoiseLevelsMinTooltip", "Values at or below this threshold become black."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicNoiseLevelsMinTooltip", "Values at or below this threshold become black."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMin; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.LevelsMin = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMin > Params.LevelsMax)
					{
						Params.LevelsMax = Params.LevelsMin;
					}
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicNoiseLevelsMaxLabel", "Levels Max"))
			.ToolTipText(LOCTEXT("DynamicNoiseLevelsMaxTooltip", "Values at or above this threshold become white."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicNoiseLevelsMaxTooltip", "Values at or above this threshold become white."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMax; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeNoiseParams& Params)
				{
					Params.LevelsMax = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMax < Params.LevelsMin)
					{
						Params.LevelsMin = Params.LevelsMax;
					}
				});
			})
		]
	]

	// Invert
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([GetParams]()
		{
			return GetParams().bInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const ECheckBoxState NewState)
		{
			MutateDynamicNoiseParam(LayerId, ExpectedMaskInstanceId, [NewState](FVertexMaskForgeNoiseParams& Params)
			{
				Params.bInvert = (NewState == ECheckBoxState::Checked);
			});
		})
		.Content()
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicNoiseInvertLabel", "Invert"))
		]
	];
}

void SVertexMaskForgePanel::MutateDynamicThicknessParam(
	const FGuid LayerId, const FGuid ExpectedMaskInstanceId,
	TFunctionRef<void(FVertexMaskForgeThicknessParams&)> Mutator)
{
	// AUDITED (M17-TH-DL-B): mirrors MutateDynamicCurvatureParam's own six-step identity-validated write
	// path exactly -- mask exists, GeneratorType is still Thickness, Params is still the Thickness payload
	// type, and MaskInstanceId still matches what THIS widget was built for. Any mismatch (cleared,
	// reassigned to a different generator, or replaced by a newer instance) is a silent no-op, never a
	// fallback to another layer or a stale write. This function never touches
	// DynamicSourceTopologyThicknessCachesByLayerId itself -- cache freshness (raycast vs. remap-only) is
	// entirely the backend's own responsibility, decided inside GenerateThicknessMaskFromDynamicMesh's own
	// SearchDistance/Bias comparison (see the .cpp's own Thickness dispatch branch).
	const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
	if (!Mask
		|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::Thickness
		|| !Mask->Params.IsType<FVertexMaskForgeThicknessParams>()
		|| Mask->MaskInstanceId != ExpectedMaskInstanceId)
	{
		return;
	}
	FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
	Mutator(NewParams.Get<FVertexMaskForgeThicknessParams>());
	DynamicLayerStack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, NewParams);
	OnDynamicLayerStackMutated();
}

TSharedRef<SWidget> SVertexMaskForgePanel::BuildDynamicThicknessLayerParamsBlock(const FGuid LayerId, const FGuid ExpectedMaskInstanceId)
{
	// AUDITED (M17-TH-DL-B): every Value_Lambda/IsChecked_Lambda below re-resolves LayerId's CURRENT stored
	// Thickness params fresh on every call (never a cached copy) -- if the layer/mask/params no longer
	// match (e.g. generator switched away), the accessor falls back to the authoritative default
	// (MinThickness=0/MaxThickness=50/SearchDistance=100/not inverted -- FVertexMaskForgeThicknessParams'
	// own default member initializers, MakeVertexMaskForgeGeneratorParams' own construction path) rather
	// than reading garbage. Read-only; never mutates. Bias and Blur are deliberately never read or exposed
	// here -- see this function's own header doc comment.
	auto GetParams = [this, LayerId]() -> FVertexMaskForgeThicknessParams
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		const FVertexMaskForgeThicknessParams* ThicknessParams = Mask ? Mask->Params.TryGet<FVertexMaskForgeThicknessParams>() : nullptr;
		return ThicknessParams ? *ThicknessParams : FVertexMaskForgeThicknessParams();
	};

	return SNew(SVerticalBox)
	.Visibility_Lambda([this, LayerId]()
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = DynamicLayerStack.GetLayerMask(LayerId);
		return (Mask && Mask->GeneratorType == EVertexMaskForgeGeneratorType::Thickness) ? EVisibility::Visible : EVisibility::Collapsed;
	})

	// Min Thickness / Max Thickness -- [0,10000], matching the Legacy widgets' own range exactly (see
	// SVertexMaskForgePanel.h's ThicknessMinThickness/ThicknessMaxThickness own doc comments). No stronger
	// Min<=Max coupling is introduced here than the Legacy panel's own controls already perform (neither
	// currently clamps against the other) -- the backend itself remains tolerant of an inverted pair.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicThicknessMinThicknessLabel", "Min Thickness"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(10000.0f)
			.Delta(0.01f)
			.Value_Lambda([GetParams]() { return GetParams().MinThickness; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicThicknessParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeThicknessParams& Params)
				{
					Params.MinThickness = FMath::Clamp(NewValue, 0.0f, 10000.0f);
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicThicknessMaxThicknessLabel", "Max Thickness"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(10000.0f)
			.Delta(0.01f)
			.Value_Lambda([GetParams]() { return GetParams().MaxThickness; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicThicknessParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeThicknessParams& Params)
				{
					Params.MaxThickness = FMath::Clamp(NewValue, 0.0f, 10000.0f);
				});
			})
		]
	]

	// Search Distance -- [0,10000], matching the Legacy widget's own range exactly (see
	// SVertexMaskForgePanel.h's ThicknessSearchDistance own doc comment). The Legacy panel's own tooltip
	// claims Search Distance "must be at least Max Thickness -- enforced automatically", but no such
	// clamp/coupling code exists for that pair in the Legacy panel today (confirmed by direct inspection);
	// this control therefore matches the ACTUAL current production behavior (tolerant, not enforced), never
	// a stronger invariant invented for this checkpoint alone.
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicThicknessSearchDistanceLabel", "Search Distance"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(10000.0f)
			.Delta(0.01f)
			.Value_Lambda([GetParams]() { return GetParams().SearchDistance; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicThicknessParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeThicknessParams& Params)
				{
					Params.SearchDistance = FMath::Clamp(NewValue, 0.0f, 10000.0f);
				});
			})
		]
	]

	// Levels Min / Levels Max -- [0,1], Delta 0.01, matching Ambient Occlusion's own Dynamic Levels
	// widgets exactly (M17-TH-DL-E): same range/step/tooltips/coupled-invariant maintenance pattern
	// (the edited field is always assigned first and preserved; the OTHER field is raised/lowered
	// only if the pair would otherwise become invalid, committed through the SAME single
	// MutateDynamicThicknessParam call). PURELY COMPOSITIONAL -- reuses valid raw Thickness values,
	// never touches the raycast cache map (see this generator's own dispatch branch in
	// VertexMaskForgeDynamicSourceTopologyComposition.cpp for the exact postprocess order relative
	// to Invert).
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 2.f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicThicknessLevelsMinLabel", "Levels Min"))
			.ToolTipText(LOCTEXT("DynamicThicknessLevelsMinTooltip", "Values at or below this threshold become black."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicThicknessLevelsMinTooltip", "Values at or below this threshold become black."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMin; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicThicknessParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeThicknessParams& Params)
				{
					Params.LevelsMin = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMin > Params.LevelsMax)
					{
						Params.LevelsMax = Params.LevelsMin;
					}
				});
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DynamicThicknessLevelsMaxLabel", "Levels Max"))
			.ToolTipText(LOCTEXT("DynamicThicknessLevelsMaxTooltip", "Values at or above this threshold become white."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SSpinBox<float>)
			.MinDesiredWidth(52.f)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Delta(0.01f)
			.ToolTipText(LOCTEXT("DynamicThicknessLevelsMaxTooltip", "Values at or above this threshold become white."))
			.Value_Lambda([GetParams]() { return GetParams().LevelsMax; })
			.OnValueChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const float NewValue)
			{
				MutateDynamicThicknessParam(LayerId, ExpectedMaskInstanceId, [NewValue](FVertexMaskForgeThicknessParams& Params)
				{
					Params.LevelsMax = FMath::Clamp(NewValue, 0.0f, 1.0f);
					if (Params.LevelsMax < Params.LevelsMin)
					{
						Params.LevelsMin = Params.LevelsMax;
					}
				});
			})
		]
	]

	// Invert
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([GetParams]()
		{
			return GetParams().bInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, LayerId, ExpectedMaskInstanceId](const ECheckBoxState NewState)
		{
			MutateDynamicThicknessParam(LayerId, ExpectedMaskInstanceId, [NewState](FVertexMaskForgeThicknessParams& Params)
			{
				Params.bInvert = (NewState == ECheckBoxState::Checked);
			});
		})
		.Content()
		[
			SNew(STextBlock).Text(LOCTEXT("DynamicThicknessInvertLabel", "Invert"))
		]
	];
}

namespace
{
	// M16-K.6D-6 (Correction 2): the panel's own drag-drop payload for reordering Dynamic Layers rows --
	// derives from FDragAndDropVerticalBoxOp (SlateCore, Widgets/SBoxPanel.h) so
	// SDragAndDropVerticalBox's own OnDragOver/OnDrop (which cast via
	// DragDropEvent.GetOperationAs<FDragAndDropVerticalBoxOp>()) recognize it. Carries LayerId -- a
	// stable domain identifier -- alongside the base class's own SlotIndexBeingDragged/SlotBeingDragged
	// (populated so the box's own internal bookkeeping/Children.Move stays consistent), specifically so
	// this panel's own drop handling never needs to dereference a row/widget pointer that could have been
	// invalidated by an intervening rebuild.
	class FDynamicLayerDragDropOp : public FDragAndDropVerticalBoxOp
	{
	public:
		DRAG_DROP_OPERATOR_TYPE(FDynamicLayerDragDropOp, FDragAndDropVerticalBoxOp)

		FGuid LayerId;

		// AUDITED: FDragDropOperation::Construct() is protected -- only reachable from within a derived
		// class's own scope, hence this static factory (the standard Slate pattern, e.g.
		// FExternalDragOperation::New() in Input/DragAndDrop.h).
		static TSharedRef<FDynamicLayerDragDropOp> New()
		{
			const TSharedRef<FDynamicLayerDragDropOp> Op = MakeShared<FDynamicLayerDragDropOp>();
			Op->Construct();
			return Op;
		}
	};
}

FReply SVertexMaskForgePanel::OnDynamicLayerDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent, const int32 SlotIndexBeingDragged, SVerticalBox::FSlot* Slot)
{
	// AUDITED: SlotIndexBeingDragged is a VISUAL slot index (0 == top of the panel) -- translated to
	// DynamicLayerStack's own array index via the SAME reversed mapping RebuildDynamicLayersList uses
	// (top of panel == highest array index, see that function's own doc comment), so the captured LayerId
	// always names the layer actually under the pointer at drag-start, regardless of display order.
	const TArray<FVertexMaskForgeLayer>& Layers = DynamicLayerStack.GetLayers();
	const int32 ArrayIndex = (Layers.Num() - 1) - SlotIndexBeingDragged;
	if (!Layers.IsValidIndex(ArrayIndex))
	{
		return FReply::Unhandled();
	}

	const TSharedRef<FDynamicLayerDragDropOp> DragDropOp = FDynamicLayerDragDropOp::New();
	DragDropOp->SlotIndexBeingDragged = SlotIndexBeingDragged;
	DragDropOp->SlotBeingDragged = Slot;
	DragDropOp->LayerId = Layers[ArrayIndex].LayerId;

	return FReply::Handled().BeginDragDrop(DragDropOp);
}

TOptional<SDragAndDropVerticalBox::EItemDropZone> SVertexMaskForgePanel::OnDynamicLayerCanAcceptDrop(const FDragDropEvent& DragDropEvent, const SDragAndDropVerticalBox::EItemDropZone DropZone, int32, SVerticalBox::FSlot*)
{
	// AUDITED: rejects (returns unset -- SDragAndDropVerticalBox treats this as "no drop indicator, drop
	// not accepted here") any drag operation that is not this panel's own FDynamicLayerDragDropOp --
	// e.g. a stray external drag somehow entering this box's bounds. Otherwise echoes DropZone back
	// unchanged, which only affects the box's own Above/Below drop-indicator rendering during the drag;
	// it never affects DynamicLayerStack, which is read-only until OnDynamicLayerAcceptDrop actually
	// commits a drop.
	if (!DragDropEvent.GetOperationAs<FDynamicLayerDragDropOp>().IsValid())
	{
		return TOptional<SDragAndDropVerticalBox::EItemDropZone>();
	}
	return DropZone;
}

FReply SVertexMaskForgePanel::OnDynamicLayerAcceptDrop(const FDragDropEvent& DragDropEvent, SDragAndDropVerticalBox::EItemDropZone, const int32 SlotIndex, SVerticalBox::FSlot*)
{
	const TSharedPtr<FDynamicLayerDragDropOp> DragDropOp = DragDropEvent.GetOperationAs<FDynamicLayerDragDropOp>();
	if (!DragDropOp.IsValid())
	{
		// AUDITED: not this panel's own drag operation -- reject, DynamicLayerStack completely untouched.
		return FReply::Unhandled();
	}

	// AUDITED: SlotIndex here matches EXACTLY what SDragAndDropVerticalBox::OnDrop itself will use for
	// its own internal Children.Move(DragDropOp->SlotIndexBeingDragged, SlotIndex) call immediately after
	// this delegate returns (see Widgets/SBoxPanel.cpp) -- translating it via the SAME reversed mapping
	// used everywhere else in this row (top of panel == highest array index) and calling MoveLayer with
	// that exact target keeps the domain's own array order byte-for-byte consistent with whatever the
	// box's own internal widget reorder is about to produce. TPanelChildren::Move's own semantics
	// (RemoveAt then Insert at the destination index, i.e. the FINAL index after removal) already match
	// FVertexMaskForgeDynamicLayerStack::MoveLayer's own documented "NewIndex is the final desired index
	// after the move" contract exactly -- no adjustment needed.
	const int32 Num = DynamicLayerStack.Num();
	const int32 TargetArrayIndex = (Num - 1) - SlotIndex;

	// AUDITED: MoveLayer's own contract already makes an unknown LayerId, an out-of-range index, or a
	// same-position request safe no-ops that leave the stack completely unmodified -- no additional
	// validation is duplicated here. Never calls RebuildDynamicLayersList() -- see this function's own
	// header doc comment for why (the box's own imminent Children.Move would then operate on a Children
	// array already replaced out from under it).
	DynamicLayerStack.MoveLayer(DragDropOp->LayerId, TargetArrayIndex);
	OnDynamicLayerStackMutated();

	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnAddDynamicLayerClicked()
{
	// AUDITED: display name only -- "Layer N" from the CURRENT count, not a persistent counter. Not
	// guaranteed unique (e.g. removing "Layer 2" then adding again can produce another "Layer 2") --
	// deliberately fine, since identity is LayerId, never Name (see DuplicateNamesDoNotConfuseRows).
	const FString NewLayerName = FString::Printf(TEXT("Layer %d"), DynamicLayerStack.Num() + 1);
	const FGuid NewLayerId = DynamicLayerStack.AddLayer(NewLayerName);

	// M16-K.6D-6 (Correction 1): AddLayer's own domain default (Fill=None) is deliberately UNCHANGED --
	// see DynamicLayerFillOptions' own doc comment -- but None is no longer offered by the Fill combo, so
	// the panel's own Add-Layer handler resolves a newly added layer to the intended user-facing default
	// (White) immediately, via the existing, already-tested SetLayerFill mutator -- never by changing
	// FVertexMaskForgeDynamicLayerStack::AddLayer itself.
	DynamicLayerStack.SetLayerFill(NewLayerId, EVertexMaskForgeLayerFill::White);

	RebuildDynamicLayersList();
	OnDynamicLayerStackMutated();
	return FReply::Handled();
}

FReply SVertexMaskForgePanel::OnRemoveDynamicLayerClicked(const FGuid LayerId)
{
	// AUDITED: RemoveLayer is a safe no-op for an unknown LayerId (already-removed/stale callback) --
	// nothing else needs to guard against that case here. Removing the stack's only/last layer is
	// explicitly supported and safe (IsEmpty() stack is a valid stack) -- RebuildDynamicLayersList's own
	// empty-state branch handles the resulting empty list.
	DynamicLayerStack.RemoveLayer(LayerId);

	// M16-K.6D-8F-C.1: permanent removal also drops this layer's ephemeral Lock Axes entry (if any) --
	// Remove() is itself a safe no-op if LayerId never had an entry -- so a stale key can never later
	// influence a different, unrelated layer.
	DynamicNoiseScaleAxesLockedByLayerId.Remove(LayerId);

	// M16-K.6D-8G-C: permanent removal also drops this layer's persistent Dynamic AO cache entry (if
	// any) from EVERY selected component's own state -- unconditionally, mirroring the Lock Axes
	// cleanup immediately above: Remove() is a safe no-op if this LayerId never had an entry (the map is
	// still entirely dormant this checkpoint -- see FVertexMaskForgePreviewComponentState's own doc
	// comment on DynamicSourceTopologyAOCachesByLayerId), and a stale key can never later be
	// misattributed to a different, unrelated layer that happens to reuse this LayerId (LayerIds are
	// never reused -- see FVertexMaskForgeDynamicLayerStack::AddLayer's own FGuid::NewGuid() contract).
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
		{
			StateOwner->GetVisualSessionStateMutable().DynamicSourceTopologyAOCachesByLayerId.Remove(LayerId);
			// M17-TH-DL-B: sibling erasure for the Dynamic Thickness cache -- mirrors the AO cleanup
			// immediately above exactly (Remove() is a safe no-op if this LayerId never had an entry).
			StateOwner->GetVisualSessionStateMutable().DynamicSourceTopologyThicknessCachesByLayerId.Remove(LayerId);
		}
	}

	RebuildDynamicLayersList();
	OnDynamicLayerStackMutated();
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

	// M16-K.4; corrected M16-K.6D-6: Dynamic Layers' own Fill options -- Black/White ONLY, deliberately
	// excluding EVertexMaskForgeLayerFill::None from the user-facing combo (manual-validation correction:
	// Fill=None redundantly acted like disabling the layer, when the layer's own Enabled checkbox is
	// already the one clear disable mechanism). EVertexMaskForgeLayerFill::None itself is UNCHANGED --
	// still the real domain default (FVertexMaskForgeDynamicLayerStack::AddLayer's own default, protected
	// by VertexMaskForgeDynamicLayerStackTests.cpp) and still fully valid input to SetLayerFill/the
	// evaluator -- only this ONE combo's own OptionsSource stops offering it. See
	// OnAddDynamicLayerClicked for how a newly added layer's domain Fill is resolved away from None
	// immediately (via the existing SetLayerFill mutator, not a change to AddLayer's own default) and
	// this row's own InitiallySelectedItem resolution below for the deterministic fallback if a layer's
	// stored Fill is ever still None at row-construction time.
	DynamicLayerFillOptions.Add(MakeShared<EVertexMaskForgeLayerFill>(EVertexMaskForgeLayerFill::Black));
	DynamicLayerFillOptions.Add(MakeShared<EVertexMaskForgeLayerFill>(EVertexMaskForgeLayerFill::White));

	// M16-K.6B; corrected M16-K.6D-6; extended M16-K.6D-8C-C, M16-K.6D-8D-C, M16-K.6D-8E-C, M16-K.6D-8F-C,
	// M16-K.6D-8G-F, M17-TH-DL-B: element 0 is a VALID TSharedPtr to an UNSET TOptional (None/Unassigned)
	// -- NOT a null TSharedPtr, which SListView's own row-generation loop unconditionally skips (see this
	// array's own doc comment in SVertexMaskForgePanel.h for the confirmed root cause). All seven
	// EVertexMaskForgeGeneratorType enumerators are offered; the orchestrator dispatches every one of
	// them too, so there is no unsupported/rejected value.
	//
	// M18: the seven real entries are ordered ALPHABETICALLY BY ARTIST-FACING LABEL (never by enum
	// numeric value, which has no presentation/compatibility meaning here) -- Ambient Occlusion,
	// Curvature, Directional Normal, Material Slot, Noise/Grunge, Position (artist-facing name for
	// BoundingBox -- see GetDynamicLayerGeneratorTypeLabel), Thickness. This is the single authoritative
	// list every generator picker in the panel reads from; there is no second list to keep in sync.
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>());
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>(EVertexMaskForgeGeneratorType::AmbientOcclusion));
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>(EVertexMaskForgeGeneratorType::Curvature));
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>(EVertexMaskForgeGeneratorType::DirectionalNormal));
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>(EVertexMaskForgeGeneratorType::MaterialSlot));
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>(EVertexMaskForgeGeneratorType::Noise));
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>(EVertexMaskForgeGeneratorType::BoundingBox));
	DynamicLayerGeneratorTypeOptions.Add(MakeShared<TOptional<EVertexMaskForgeGeneratorType>>(EVertexMaskForgeGeneratorType::Thickness));

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

			// M18: the "Active layers" readout (GetActiveMaskSourceText) was removed -- it reported only
			// the old Legacy composition stack (Bounding Box/AO/Curvature/.../Thickness enabled flags),
			// which can no longer be set by any UI, so it would only ever have shown "Active layers:
			// None -- enable a Bounding Box axis, ..." forever. The Layers section's own status line
			// (GetMaskActionStatusText/GetPreviewStatusText below) is the live, accurate readout now.

			// M18: "Layers" -- backed entirely by DynamicLayerStack, now the SOLE artist-facing workflow
			// (the old Legacy fixed-generator UI and the Legacy/Dynamic PreviewSource selector were both
			// removed in this checkpoint). This section's stack IS what Preview/Accept use -- the earlier
			// "Prototype/disconnected" framing no longer applies and has been removed.
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
							.Text(LOCTEXT("DynamicLayersSectionTitle", "Layers"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ContentPadding(FMargin(8.f, 1.f))
							.ToolTipText(LOCTEXT("AddDynamicLayerTooltip", "Add a new, empty (Fill=None) layer at the end of the list."))
							.Text(LOCTEXT("AddDynamicLayerButton", "+ Add Layer"))
							.OnClicked(FOnClicked::CreateLambda([this]() { return OnAddDynamicLayerClicked(); }))
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						// M16-K.6D-6 (Correction 2): SDragAndDropVerticalBox instead of SVerticalBox -- a
					// drop-in subclass adding drag-reorder support to its own slots. See
					// OnDynamicLayerDragDetected/OnDynamicLayerCanAcceptDrop/OnDynamicLayerAcceptDrop's own
					// doc comments (SVertexMaskForgePanel.h) for the full drag/drop contract.
					SAssignNew(DynamicLayersListContainer, SDragAndDropVerticalBox)
					.OnDragDetected(this, &SVertexMaskForgePanel::OnDynamicLayerDragDetected)
					.OnCanAcceptDropAdvanced(this, &SVertexMaskForgePanel::OnDynamicLayerCanAcceptDrop)
					.OnAcceptDrop(this, &SVertexMaskForgePanel::OnDynamicLayerAcceptDrop)
					]
				]
			]

			// AUDITED (pre-modularization UI/defaults pass): moved here, immediately below the Layers
			// section and directly above Preview Mode -- the sole entry point into an editing
			// session now sits next to session-level controls (Preview Mode) rather than above every
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

					// M18: the Legacy/Dynamic Preview Source selector, the global Channel Filter, and the
					// Fill White/Fill Black buttons were all removed -- Layers is now the sole workflow, so
					// there is no mode to pick, per-layer R/G/B channels replace the global filter, and
					// per-layer Fill Value replaces the global constant-fill actions. Preview Mode remains.
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

	// AUDITED (M16-K.4): DynamicLayersListContainer now exists (assigned via SAssignNew above) --
	// populate its rows from DynamicLayerStack's own member-initializer default
	// (FVertexMaskForgeDynamicLayerStack::MakeInitialStack(), set once when this panel instance's
	// fields were constructed) -- a single, one-time initial population. DynamicLayerStack itself is
	// never re-initialized here or anywhere else in Construct().
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

// M18: OnGeneratePreviewSourceRow/OnPreviewSourceSelectionChanged/GetPreviewSourceButtonText (the
// Legacy/Dynamic Preview Source combo) and OnChannelFilterR/G/BChanged (the global Channel Filter
// checkboxes) were removed along with their widgets -- Layers is the sole workflow now (see
// EVertexMaskForgePreviewSource's own doc comment) and per-layer R/G/B channel controls replace the
// global filter.

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
	// M18: Layers is now the sole workflow -- the earlier Legacy GeneratorState.*.Ready branch is gone
	// along with PreviewSource. Pending-ness is a shallow UI-eligibility signal only: at least one
	// selected entry has a live PreviewComponent (the normal session/selection prerequisite) AND the
	// layer stack has at least one enabled layer (FVertexMaskForgeDynamicLayerStack::HasAnyEnabledLayer()
	// -- deliberately NOT just !DynamicLayerStack.IsEmpty(), since a non-empty stack with every layer
	// disabled must also read as non-pending). Authoritative validation (unsupported generator, stale
	// topology, a masked layer that fails to compose) is never performed here -- that is solely
	// VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets' own responsibility,
	// invoked only when Accept is actually pressed.
	bool bHasSelection = false;
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid() && !Entry->PreviewComponents.IsEmpty())
		{
			bHasSelection = true;
			break;
		}
	}
	const bool bHasPending = bHasSelection && DynamicLayerStack.HasAnyEnabledLayer();

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

	// M18: Layers is now the sole workflow -- the earlier Legacy-only World-Space Directional Normal
	// pre-Accept resync guard is gone along with PreviewSource and the Legacy generator UI that produced
	// bDirectionalNormalMaskEnabled/DirectionalNormalSpace as panel-level state.
	//
	// AUDITED (M16-K.6D-7B): Targets (render-vertex) stays empty -- Layers' ONLY accept-target source is
	// VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets (ADR-011); Layers is
	// Source-Topology only (see FVertexMaskForgeSelectedMesh::bUseSourceTopology's own doc comment).
	// Targets is retained (always empty) so the combined "nothing eligible" / "confirm N assets" / write
	// section below, which already treats Targets/SourceTopologyTargets generically, needs no further
	// changes.
	TArray<VertexMaskForgeAcceptTargetBuilder::FAcceptTarget> Targets;
	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> SourceTopologyTargets;
	FText ErrorText;
	if (!VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets(SelectedMeshes, DynamicLayerStack, SourceTopologyTargets, ErrorText))
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
			// M18 (TECHNICAL DEBT -- see EVertexMaskForgePreviewSource's own doc comment): the ONLY
			// remaining functional read of PreviewSource anywhere in production code. PreviewSource has
			// no write site left (its combo box was removed and defaults permanently to Dynamic), so
			// this condition is now provably always true and the Legacy composition code below it
			// (byte-for-byte unchanged from before this checkpoint) is unreachable dead code, deliberately
			// retained rather than excised -- removing it safely would require restructuring this deeply
			// nested, dual-domain function beyond this UI-consolidation checkpoint's own scope.
			if (PreviewSource == EVertexMaskForgePreviewSource::Dynamic)
			{
				// AUDITED: EnsureBaselineCaptured is idempotent (a no-op once already initialized, see
				// its own doc comment) -- calling it here, before any Legacy Layers work, is safe and
				// correct for both branches; the Legacy branch further below calls it again (harmless
				// no-op) for entries that reach it directly.
				if (!StateOwner->AreColorsInitialized())
				{
					StateOwner->EnsureBaselineCaptured(VertexMaskForgePanel::CaptureBaselineColorsSourceTopology(*WorkingMesh.Mesh));
				}

				// M16-K.6D-5: Dynamic reads only StateOwner's own already-captured, read-only
				// BaselineColors as the orchestrator's BaseColors -- mirroring the exact same base the
				// proven M16-K.5J real-object integration test itself used
				// (VertexMaskForge.DynamicCompositionSourceTopologyIntegration.*). Never
				// StateOwner->GetWorkingColors()/GetCommittedColors() (those belong to the Legacy
				// lifecycle only), never ApplyComposedColorsRGB, never any store. The orchestrator's
				// output is a fresh, local, caller-owned TArray<FColor> that lives only for the
				// remainder of this one component's own update -- never retained on State/StateOwner/
				// the panel, never cached, never given a generation counter.
				TArray<FColor> DynamicComposedColors;
				// M16-K.6D-8G-B: the real transform of THIS component (SourceComponent, resolved above at
				// the top of this per-component loop) -- never Identity, never another component's
				// transform, never cached elsewhere.
				// M16-K.6D-8G-D: State.DynamicSourceTopologyAOCachesByLayerId is THIS component's own
				// persistent Model D Ambient Occlusion cache map (established M16-K.6D-8G-C) -- State is
				// the SAME FVertexMaskForgePreviewComponentState& already resolved above for this loop
				// iteration, never a fresh/local/temporary map, so any AO layer's cache genuinely persists
				// across recompositions exactly as Model D requires.
				const bool bDynamicComposed = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
					WorkingMesh, DynamicLayerStack, StateOwner->GetBaselineColors(), SourceComponent->GetComponentTransform(),
					State.DynamicSourceTopologyAOCachesByLayerId, State.DynamicSourceTopologyThicknessCachesByLayerId, DynamicComposedColors);
				if (!bDynamicComposed)
				{
					// AUDITED: an explicit, non-destructive failure -- the orchestrator's own contract
					// guarantees DynamicComposedColors was never touched, so nothing here reads a
					// partial result. The component instead falls back to its REAL, original appearance
					// (never a stale WorkingColors, never a white/black placeholder), and the status
					// line reports the failure factually.
					LastOperationErrorText = LOCTEXT("DynamicPreviewComposeFailed",
						"Preview: composition failed for one or more components (unsupported layer, or invalid input) -- showing the original appearance instead.");
					VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
					continue;
				}

				// AUDITED: feeds the M16-K.6D-2 visual-only seam -- Preview Mode (via the seam's own
				// DeriveValidatedSourceTopologyPreviewColors) is the only display transform applied here.
				// Never ApplyComposedColorsRGB, never WorkingColors/SourceTopologyWorkingColors, never
				// BuildAcceptTargets -- this is presentation only.
				if (!VertexMaskForgePanel::ApplySuppliedSourceTopologyPreviewColors(
					State, *WorkingMesh.Mesh, DynamicComposedColors, CurrentPreviewMode,
					DebugMaterial, bUseOriginalMaterials, ActorHideStates))
				{
					LastOperationErrorText = LOCTEXT("DynamicPreviewSeamFailed",
						"Preview: could not apply the composed result to the viewport for one or more components.");
					VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
				}
				continue;
			}

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

			// AUDITED (M16-K.6D-2 correction): the Legacy caller supplies its own just-written
			// StateOwner->GetWorkingColors() -- the exact semantic composition ApplyComposedColorsRGB
			// just applied above -- as SemanticComposedColors, plus CurrentPreviewMode; the seam itself
			// (ApplySuppliedSourceTopologyPreviewColors) performs Preview Mode display reduction
			// internally via DeriveDisplayColors, validates cardinality, and applies visually. This is
			// the read explicitly authorized for a Legacy caller by this checkpoint's own contract ("O
			// caller Legacy pode obter SourceTopologyWorkingColors antes da chamada e fornecê-las
			// explicitamente como parâmetro. O seam em si não pode buscá-las.") -- the seam function
			// itself never calls GetWorkingColors/GetSourceTopologyWorkingColors. DISPLAY-ONLY: Accept
			// (BuildSourceTopologyAcceptTargets/WriteSourceTopologyAcceptTargets) reads WorkingColors
			// verbatim, never the seam's internally-derived reduced copy -- same "Preview Mode never
			// affects Accept" guarantee the render-vertex path already has. On failure (cardinality
			// mismatch or invalid component -- neither of which a valid Legacy composition can produce,
			// since ApplyComposedColorsRGB already validated cardinality against Baseline above), the
			// visual is explicitly restored, never left showing a stale prior result, never falling back
			// to WorkingColors or to Legacy (there is no Dynamic caller to fall back FROM here).
			if (!VertexMaskForgePanel::ApplySuppliedSourceTopologyPreviewColors(
				State, *WorkingMesh.Mesh, StateOwner->GetWorkingColors(), CurrentPreviewMode,
				DebugMaterial, bUseOriginalMaterials, ActorHideStates))
			{
				VertexMaskForgePanel::RestorePreviewVisualOnly(State, ActorHideStates);
			}
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
