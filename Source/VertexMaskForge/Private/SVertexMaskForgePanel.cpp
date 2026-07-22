#include "SVertexMaskForgePanel.h"

#include "AssetRegistry/AssetData.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Set.h"
#include "ContentBrowserModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "Logging/LogMacros.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Math/NumericLimits.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Modules/ModuleManager.h"
#include "RenderResource.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Selection.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

DEFINE_LOG_CATEGORY_STATIC(LogVertexMaskForge, Log, All);

// FDynamicMesh3 is only forward-declared in the header; these special member functions are
// defined here, now that it is a complete type, so FVertexMaskForgeWorkingMesh itself owns the
// responsibility for being safe to destroy/move -- no other class's destructor is relied upon.
FVertexMaskForgeWorkingMesh::~FVertexMaskForgeWorkingMesh() = default;
FVertexMaskForgeWorkingMesh::FVertexMaskForgeWorkingMesh(FVertexMaskForgeWorkingMesh&&) = default;
FVertexMaskForgeWorkingMesh& FVertexMaskForgeWorkingMesh::operator=(FVertexMaskForgeWorkingMesh&&) = default;

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace VertexMaskForgePanel
{
	static FText GetSourceLabel(const EVertexMaskForgeSelectionSource Sources)
	{
		const bool bViewport = EnumHasAnyFlags(Sources, EVertexMaskForgeSelectionSource::Viewport);
		const bool bContentBrowser = EnumHasAnyFlags(Sources, EVertexMaskForgeSelectionSource::ContentBrowser);

		if (bViewport && bContentBrowser)
		{
			return LOCTEXT("SourceBoth", "Viewport + Content Browser");
		}
		if (bViewport)
		{
			return LOCTEXT("SourceViewport", "Viewport");
		}
		if (bContentBrowser)
		{
			return LOCTEXT("SourceContentBrowser", "Content Browser");
		}
		return FText::GetEmpty();
	}

	/** Adds a mesh to the collected list, or merges its source flags if already present. */
	static void AddOrUpdateSelectedMesh(
		TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
		TMap<FString, int32>& InOutPathToIndex,
		const FString& AssetPathString,
		const FString& AssetName,
		const TSoftObjectPtr<UStaticMesh>& SoftMesh,
		const EVertexMaskForgeSelectionSource Source,
		UStaticMeshComponent* SourceComponent = nullptr)
	{
		int32 EntryIndex;
		if (const int32* ExistingIndex = InOutPathToIndex.Find(AssetPathString))
		{
			InOutMeshes[*ExistingIndex]->Sources |= Source;
			EntryIndex = *ExistingIndex;
		}
		else
		{
			TSharedPtr<FVertexMaskForgeSelectedMesh> NewEntry = MakeShared<FVertexMaskForgeSelectedMesh>();
			NewEntry->Mesh = SoftMesh;
			NewEntry->AssetName = AssetName;
			NewEntry->AssetPathString = AssetPathString;
			NewEntry->Sources = Source;

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
		//     ComposeRenderOrderPreviewColors).
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

	static FText GetVertexColorStateLabel(const EVertexMaskForgeVertexColorState State)
	{
		switch (State)
		{
		case EVertexMaskForgeVertexColorState::Present:
			return LOCTEXT("VertexColorPresent", "Present");
		case EVertexMaskForgeVertexColorState::PartialOrInvalid:
			return LOCTEXT("VertexColorPartial", "Partial/Invalid");
		case EVertexMaskForgeVertexColorState::None:
		default:
			return LOCTEXT("VertexColorNone", "None");
		}
	}

	static FText GetEnabledDisabledLabel(const bool bEnabled)
	{
		return bEnabled ? LOCTEXT("StateEnabled", "Enabled") : LOCTEXT("StateDisabled", "Disabled");
	}

	/** Builds the compact diagnostics line shown under each mesh row. */
	static FText GetDiagnosticsSummaryText(const FVertexMaskForgeMeshDiagnostics& Diagnostics)
	{
		if (!Diagnostics.bValid)
		{
			return LOCTEXT("DiagnosticsUnavailable", "Render data unavailable");
		}

		return FText::Format(
			LOCTEXT("DiagnosticsFormat",
				"LODs: {0}   LOD 0 Vertices: {1}   LOD 0 Triangles: {2}   Material Slots: {3}   Vertex Colors: {4} ({5} / {6})   Nanite: {7}   Allow CPU Access: {8}"),
			FText::AsNumber(Diagnostics.NumLODs),
			FText::AsNumber(Diagnostics.LOD0NumVertices),
			FText::AsNumber(Diagnostics.LOD0NumTriangles),
			FText::AsNumber(Diagnostics.NumMaterialSlots),
			GetVertexColorStateLabel(Diagnostics.VertexColorState),
			FText::AsNumber(Diagnostics.LOD0NumColorVertices),
			FText::AsNumber(Diagnostics.LOD0NumVertices),
			GetEnabledDisabledLabel(Diagnostics.bNaniteEnabled),
			GetEnabledDisabledLabel(Diagnostics.bAllowCPUAccess));
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

	static FText GetWorkingMeshStateLabel(const EVertexMaskForgeWorkingMeshState State)
	{
		switch (State)
		{
		case EVertexMaskForgeWorkingMeshState::Ready:
			return LOCTEXT("WorkingMeshReady", "Ready");
		case EVertexMaskForgeWorkingMeshState::SourceMeshDescriptionUnavailable:
			return LOCTEXT("WorkingMeshNoSourceMeshDescription", "Source MeshDescription Unavailable");
		case EVertexMaskForgeWorkingMeshState::ConversionFailed:
			return LOCTEXT("WorkingMeshConversionFailed", "Conversion Failed");
		case EVertexMaskForgeWorkingMeshState::InvalidSource:
		default:
			return LOCTEXT("WorkingMeshInvalidSource", "Invalid Source");
		}
	}

	/**
	 * Material IDs summary: communicates both the state and, for Preserved, whether every ID fell
	 * within the source's Material Slot range plus the distinct-ID count. IDs are never remapped
	 * or corrected here -- "Out of Range" is only ever reported, not fixed.
	 */
	static FText GetMaterialIDSummaryText(const FVertexMaskForgeWorkingMesh& WorkingMesh)
	{
		switch (WorkingMesh.MaterialIDState)
		{
		case EVertexMaskForgeMaterialIDState::Preserved:
			if (WorkingMesh.bMaterialIDsInRange)
			{
				return FText::Format(LOCTEXT("MaterialIDPreservedFormat", "Preserved ({0})"),
					FText::AsNumber(WorkingMesh.DistinctMaterialIDCount));
			}
			return FText::Format(LOCTEXT("MaterialIDPreservedOutOfRangeFormat", "Preserved (Out of Range) ({0})"),
				FText::AsNumber(WorkingMesh.DistinctMaterialIDCount));
		case EVertexMaskForgeMaterialIDState::Missing:
			return LOCTEXT("MaterialIDMissing", "Missing");
		case EVertexMaskForgeMaterialIDState::Unavailable:
		default:
			return LOCTEXT("MaterialIDUnavailable", "Unavailable");
		}
	}

	static FText GetWorkingVertexColorStateLabel(const EVertexMaskForgeWorkingVertexColorState State)
	{
		switch (State)
		{
		case EVertexMaskForgeWorkingVertexColorState::Present:
			return LOCTEXT("WorkingVertexColorPresent", "Present");
		case EVertexMaskForgeWorkingVertexColorState::Missing:
			// Vertex Instance Colors is a Mandatory MeshDescription attribute and defaults to
			// white, and the Static Mesh build pipeline omits the Color Vertex Buffer entirely
			// when every color equals that default. So "no buffer" only proves the effective
			// result is white -- it does not prove whether white was ever explicitly painted vs.
			// simply never touched. The internal state name (Missing) is kept as-is; only this
			// user-facing label is softened to reflect that ambiguity.
			return LOCTEXT("WorkingVertexColorMissing", "Not Stored (Defaults to White)");
		case EVertexMaskForgeWorkingVertexColorState::Invalid:
			return LOCTEXT("WorkingVertexColorInvalid", "Invalid");
		case EVertexMaskForgeWorkingVertexColorState::Unavailable:
		default:
			return LOCTEXT("WorkingVertexColorUnavailable", "Unavailable");
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

		WorkingMesh.State = EVertexMaskForgeWorkingMeshState::Ready;
		return WorkingMesh;
	}

	/** Builds the compact working-copy diagnostics line shown under each mesh row. */
	static FText GetWorkingMeshSummaryText(const FVertexMaskForgeWorkingMesh& WorkingMesh)
	{
		if (WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready)
		{
			return FText::Format(
				LOCTEXT("WorkingMeshUnavailableFormat", "Working Copy: {0}"),
				GetWorkingMeshStateLabel(WorkingMesh.State));
		}

		FText Summary = FText::Format(
			LOCTEXT("WorkingMeshReadyFormat",
				"Working Copy: Ready   Dynamic Verts: {0}   Tris: {1}   Material IDs: {2}   Vertex Color Attribute: {3}   Color Elements: {4}"),
			FText::AsNumber(WorkingMesh.DynamicVertexCount),
			FText::AsNumber(WorkingMesh.DynamicTriangleCount),
			GetMaterialIDSummaryText(WorkingMesh),
			GetWorkingVertexColorStateLabel(WorkingMesh.VertexColorState),
			FText::AsNumber(WorkingMesh.ColorStats.NumElements));

		// Only surface discarded/degenerate triangles when there actually were any.
		if (WorkingMesh.DiscardedTriangleCount > 0)
		{
			Summary = FText::Format(
				LOCTEXT("WorkingMeshDiscardedTrisFormat", "{0}   Discarded Tris: {1}"),
				Summary,
				FText::AsNumber(WorkingMesh.DiscardedTriangleCount));
		}

		return Summary;
	}

	/** Formats a 0-1 color channel value with a fixed two-decimal precision. */
	static FText FormatColorChannel(const float Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = 2;
		Options.MaximumFractionalDigits = 2;
		return FText::AsNumber(Value, &Options);
	}

	/** Builds the RGBA range / non-white / non-black summary line, or an empty text if not applicable. */
	static FText GetColorStatsSummaryText(const FVertexMaskForgeWorkingMesh& WorkingMesh)
	{
		if (WorkingMesh.VertexColorState != EVertexMaskForgeWorkingVertexColorState::Present)
		{
			return FText::GetEmpty();
		}

		const FVertexMaskForgeColorStats& Stats = WorkingMesh.ColorStats;

		return FText::Format(
			LOCTEXT("ColorStatsFormat",
				"RGBA Range: R {0}-{1} | G {2}-{3} | B {4}-{5} | A {6}-{7}   Non-white: {8}   Non-black: {9}"),
			FormatColorChannel(Stats.MinColor.X), FormatColorChannel(Stats.MaxColor.X),
			FormatColorChannel(Stats.MinColor.Y), FormatColorChannel(Stats.MaxColor.Y),
			FormatColorChannel(Stats.MinColor.Z), FormatColorChannel(Stats.MaxColor.Z),
			FormatColorChannel(Stats.MinColor.W), FormatColorChannel(Stats.MaxColor.W),
			FText::AsNumber(Stats.NumNonWhite),
			FText::AsNumber(Stats.NumNonBlack));
	}

	// --- Bounding Box Z Mask prototype (Local Z, Bottom to Top) -----------------------------

	/**
	 * Generates the Bounding Box Z mask directly in RENDER VERTEX order for one Static Mesh's LOD 0.
	 *
	 * AUDITED ARCHITECTURAL CORRECTION: this mask used to be computed over FDynamicMesh3's (welded,
	 * non-compact) vertex domain and then mapped onto render vertices by position-matching
	 * (FindMatchingVertexID). That undercounted values whenever the conversion welded render
	 * vertices together (UV/normal seams, split vertices) -- e.g. 20,990 render vertices but only
	 * 11,874 Dynamic Mesh vertices on one tested asset, leaving ~9,116 render vertices with no
	 * matched value and therefore white. A spatial, Local-Z-only mask has no reason to depend on
	 * Dynamic Mesh topology at all, so it is now computed directly from
	 * LOD0.VertexBuffers.PositionVertexBuffer -- ONE value per RenderVertexIndex, guaranteeing
	 * Mask.Values.Num() == PositionVertexBuffer.GetNumVertices() exactly (see the invariant check
	 * below). Render vertices that share a position (a seam) each still get their own array slot,
	 * but since the mask value is a pure function of position, they necessarily compute to the same
	 * value -- consistent with baseline colors remaining independent per render vertex.
	 *
	 * The FDynamicMesh3-based position-matching machinery (BuildPositionBuckets/FindMatchingVertexID)
	 * is intentionally left in this file, unused by this function, for future generators that
	 * genuinely need Dynamic Mesh topology/source-vertex correspondence (e.g. per-triangle data);
	 * it must not be reintroduced here.
	 *
	 * Formula (documented per checkpoint spec):
	 *   NormalizedZ = (VertexZ - MinZ) / (MaxZ - MinZ)          -- local-space, from LOD0's own
	 *                                                               render vertex positions only
	 *   Lower       = Position - TransitionWidth * 0.5
	 *   Mask        = clamp((NormalizedZ - Lower) / TransitionWidth, 0, 1)
	 *   if bInvert:  Mask = 1 - Mask
	 *
	 * With defaults (Position = 0.5, TransitionWidth = 1.0) this reduces exactly to NormalizedZ,
	 * i.e. Bottom = 0, Top = 1. TransitionWidth is floored to a small epsilon to avoid division by
	 * zero; Mask is clamped to [0,1] before Invert is applied, so Invert always yields Bottom = 1,
	 * Top = 0 with the same defaults. MinZ/MaxZ are taken over ALL render vertices of the LOD, so
	 * disconnected pieces correctly share the mesh's single global bounding box.
	 *
	 * Never touches the Primary Color Overlay, MeshDescription, FDynamicMesh3, RenderData, or the
	 * source asset -- only reads FPositionVertexBuffer positions (read-only) and writes into the
	 * returned FVertexMaskForgeScalarMask.
	 */
	static FVertexMaskForgeScalarMask GenerateBoundingBoxZMask(
		const FStaticMeshLODResources& LOD0,
		const float Position,
		const float TransitionWidth,
		const bool bInvert)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Position = Position;
		Mask.TransitionWidth = TransitionWidth;
		Mask.bInvert = bInvert;

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		// Pass 1: local-space Z bounds over EVERY render vertex, and which Render Vertex Index holds
		// each (for Bottom/Top lookup) -- the global bounding box of the whole LOD, not per-piece.
		double MinZ = TNumericLimits<double>::Max();
		double MaxZ = TNumericLimits<double>::Lowest();
		int32 BottomVertexIndex = INDEX_NONE;
		int32 TopVertexIndex = INDEX_NONE;

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const double Z = static_cast<double>(RenderPositions.VertexPosition(i).Z);
			if (Z < MinZ)
			{
				MinZ = Z;
				BottomVertexIndex = i;
			}
			if (Z > MaxZ)
			{
				MaxZ = Z;
				TopVertexIndex = i;
			}
		}

		Mask.BoundsMinZ = MinZ;
		Mask.BoundsMaxZ = MaxZ;

		// A small, explicitly documented epsilon: below this local-space Z extent, normalizing
		// would amplify floating-point noise into a meaningless gradient, so we refuse instead.
		constexpr double MinZExtent = 1e-5;
		const double ZExtent = MaxZ - MinZ;
		if (!FMath::IsFinite(ZExtent) || ZExtent <= MinZExtent || BottomVertexIndex == INDEX_NONE || TopVertexIndex == INDEX_NONE)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::DegenerateBounds;
			return Mask;
		}

		// Dense by construction: render vertex indices are already compact (0..NumRenderVerts-1), so
		// every slot is written below -- unlike the old Dynamic-Mesh-domain version, bHasValue is
		// uniformly true rather than sparse.
		Mask.Values.SetNumZeroed(NumRenderVerts);
		Mask.bHasValue.Init(true, NumRenderVerts);

		// Epsilon guard against a zero (or near-zero) Transition Width, per the checkpoint spec.
		const float SafeTransitionWidth = FMath::Max(TransitionWidth, 1e-4f);
		const float Lower = Position - SafeTransitionWidth * 0.5f;

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;
		bool bAllFinite = true;
		bool bAllInRange = true;

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const double Z = static_cast<double>(RenderPositions.VertexPosition(i).Z);
			const float NormalizedZ = static_cast<float>((Z - MinZ) / ZExtent);

			float Value = FMath::Clamp((NormalizedZ - Lower) / SafeTransitionWidth, 0.f, 1.f);
			if (bInvert)
			{
				Value = 1.f - Value;
			}

			if (!FMath::IsFinite(Value))
			{
				bAllFinite = false;
			}
			if (Value < 0.f || Value > 1.f)
			{
				bAllInRange = false;
			}

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

		float BottomValue = 0.f;
		float TopValue = 0.f;
		const bool bFoundBottom = Mask.TryGetValue(BottomVertexIndex, BottomValue);
		const bool bFoundTop = Mask.TryGetValue(TopVertexIndex, TopValue);
		Mask.BottomValue = BottomValue;
		Mask.TopValue = TopValue;

		// Integrity checks: never silently hide inconsistent output. The mandatory invariant
		// (Mask.Values.Num() == PositionVertexBuffer.GetNumVertices()) is enforced by construction
		// above (dense SetNumZeroed(NumRenderVerts)); NumValidValues == RenderVertexCount is checked
		// explicitly here as well so a future edit that reintroduces sparsity is caught immediately.
		if (!bAllFinite || !bAllInRange || !bFoundBottom || !bFoundTop
			|| Mask.NumValidValues != NumRenderVerts || Mask.Values.Num() != NumRenderVerts)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Invalid;
			return Mask;
		}

		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}

	static FText GetScalarMaskStateLabel(const EVertexMaskForgeScalarMaskState State)
	{
		switch (State)
		{
		case EVertexMaskForgeScalarMaskState::Ready:
			return LOCTEXT("ScalarMaskReady", "Ready");
		case EVertexMaskForgeScalarMaskState::Unavailable:
			return LOCTEXT("ScalarMaskUnavailable", "Unavailable");
		case EVertexMaskForgeScalarMaskState::DegenerateBounds:
			return LOCTEXT("ScalarMaskDegenerateBounds", "Degenerate Bounds (Local Z Extent insufficient)");
		case EVertexMaskForgeScalarMaskState::Invalid:
			return LOCTEXT("ScalarMaskInvalid", "Invalid");
		case EVertexMaskForgeScalarMaskState::NotGenerated:
		default:
			return LOCTEXT("ScalarMaskNotGenerated", "Not Generated");
		}
	}

	/** Formats a mask scalar value (0-1) with a fixed three-decimal precision. */
	static FText FormatMaskValue(const float Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = 3;
		Options.MaximumFractionalDigits = 3;
		return FText::AsNumber(Value, &Options);
	}

	/**
	 * Builds the compact "BBox Z Mask: ..." diagnostics line shown under each mesh row.
	 *
	 * TEMPORARY (audited render-vertex-order fix): "Render Verts" and "Bounds Z" are added
	 * specifically so the Values.Num() == PositionVertexBuffer.GetNumVertices() invariant is
	 * directly checkable from the panel UI (Mask Values must now always equal Render Verts, and
	 * never fall short of it the way the old Dynamic-Mesh-domain version could).
	 */
	static FText GetBoundingBoxMaskSummaryText(const FVertexMaskForgeScalarMask& Mask)
	{
		if (Mask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			return FText::Format(
				LOCTEXT("BBoxMaskUnreadyFormat", "BBox Z Mask: {0}"),
				GetScalarMaskStateLabel(Mask.State));
		}

		return FText::Format(
			LOCTEXT("BBoxMaskReadyFormat",
				"BBox Z Mask: Ready   Render Verts: {0}   Mask Values: {1}   Bounds Z: {2} to {3}   Mask Range: {4}-{5}   Mean: {6}   Bottom → Top: {7} → {8}   Near Zero: {9}   Near One: {10}"),
			FText::AsNumber(Mask.RenderVertexCount),
			FText::AsNumber(Mask.NumValidValues),
			FText::AsNumber(Mask.BoundsMinZ), FText::AsNumber(Mask.BoundsMaxZ),
			FormatMaskValue(Mask.MinValue), FormatMaskValue(Mask.MaxValue),
			FormatMaskValue(Mask.MeanValue),
			FormatMaskValue(Mask.BottomValue), FormatMaskValue(Mask.TopValue),
			FText::AsNumber(Mask.NumNearZero),
			FText::AsNumber(Mask.NumNearOne));
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
		default:
			return FText::GetEmpty();
		}
	}

	/**
	 * Requirement 3's composition formula: Channel Filter channels take the mask value, every
	 * other channel keeps the original color. Always computed from OriginalColor + MaskValue
	 * directly -- never from a previously composed result -- so repeated toggling cannot accumulate.
	 */
	static FVector4f ComposeFilteredColor(
		const FVector4f& OriginalColor,
		const float MaskValue,
		const bool bFilterR, const bool bFilterG, const bool bFilterB, const bool bFilterA)
	{
		return FVector4f(
			bFilterR ? MaskValue : OriginalColor.X,
			bFilterG ? MaskValue : OriginalColor.Y,
			bFilterB ? MaskValue : OriginalColor.Z,
			bFilterA ? MaskValue : OriginalColor.W);
	}

	/** Reduces a composed RGBA color to what the given Preview Mode should actually display. */
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
	// AUDITED: no longer used by the Bounding Box Z mask (see GenerateBoundingBoxZMask and
	// ComposeRenderOrderPreviewColors), since a purely spatial, Local-Z-only mask has no need for
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
	 * Composes the render-order preview color buffer for FStaticMeshComponentLODInfo::OverrideVertexColors.
	 *
	 * The baseline for EVERY render vertex is that render vertex's OWN original, EFFECTIVE color --
	 * never a value collapsed from the Primary Color Overlay onto a single "color per Dynamic Mesh
	 * vertex". This is what preserves color seams: two render vertices that share a position but
	 * have different original colors (a seam) each keep their own distinct baseline. "Effective"
	 * means priority order (audited, Problem 3):
	 *   1. InstanceOverrideColors (SourceComponent's own, PRE-EXISTING per-instance
	 *      FStaticMeshComponentLODInfo::OverrideVertexColors, e.g. from a prior Mesh Paint session on
	 *      this placed instance) IF non-null and its vertex count matches LOD0's -- this is what the
	 *      artist actually sees in the level, and it is read-only here: never written to.
	 *   2. Otherwise, the asset's own LOD0 ColorVertexBuffer (RenderData), if its count matches.
	 *   3. Otherwise, white -- consistent with the rest of the panel's "no original colors" fallback.
	 * A buffer present but with a mismatched vertex count (partial/invalid) is treated exactly like
	 * "absent" and safely falls through to the next priority; it is never partially applied or
	 * index-clamped.
	 *
	 * AUDITED ARCHITECTURAL CORRECTION: Mask is now indexed DIRECTLY by render vertex index (see
	 * GenerateBoundingBoxZMask), so the mask value for render vertex i is simply Mask.TryGetValue(i,
	 * ...) -- no FDynamicMesh3, no position matching, no Unmatched/Ambiguous outcome is possible for
	 * this mask. OutNumComposed reports how many render vertices actually got a mask value applied
	 * (i.e. Mask.TryGetValue succeeded); with a Ready mask generated for the SAME LOD0 this is always
	 * exactly NumRenderVerts (100%) -- only a genuinely stale mask (generated against a different
	 * vertex count, e.g. the asset was rebuilt since) would ever make this less than NumRenderVerts,
	 * and such vertices safely keep their baseline color unchanged rather than being guessed.
	 * Every channel not enabled in the Channel Filter is copied byte-for-byte from that render
	 * vertex's own effective baseline color, independent of whether the mask applied.
	 */
	static TArray<FColor> ComposeRenderOrderPreviewColors(
		const FVertexMaskForgeScalarMask& Mask,
		const FStaticMeshLODResources& LOD0,
		const FColorVertexBuffer* InstanceOverrideColors,
		const EVertexMaskForgePreviewMode Mode,
		const bool bFilterR, const bool bFilterG, const bool bFilterB, const bool bFilterA,
		int32& OutNumComposed)
	{
		OutNumComposed = 0;

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const FColorVertexBuffer& AssetRenderColors = LOD0.VertexBuffers.ColorVertexBuffer;
		const uint32 NumRenderVerts = RenderPositions.GetNumVertices();

		// Priority 1: SourceComponent's own per-instance override (read-only). Priority 2: the
		// asset's own LOD0 colors. Priority 3 (neither branch taken below): white.
		const bool bHasInstanceOverride =
			InstanceOverrideColors != nullptr && InstanceOverrideColors->GetNumVertices() == NumRenderVerts;
		const bool bHasAssetColors = !bHasInstanceOverride && AssetRenderColors.GetNumVertices() == NumRenderVerts;

		TArray<FColor> Result;
		Result.SetNumUninitialized(NumRenderVerts);

		for (uint32 i = 0; i < NumRenderVerts; ++i)
		{
			// Baseline: this render vertex's OWN effective original color -- never a value borrowed
			// from a different render vertex, even one at the identical position (preserves seams).
			FColor OriginalRenderColor = FColor::White;
			if (bHasInstanceOverride)
			{
				OriginalRenderColor = InstanceOverrideColors->VertexColor(i);
			}
			else if (bHasAssetColors)
			{
				OriginalRenderColor = AssetRenderColors.VertexColor(i);
			}

			float MaskValue = 0.f;
			if (!Mask.TryGetValue(static_cast<int32>(i), MaskValue))
			{
				Result[i] = OriginalRenderColor;
				continue;
			}
			++OutNumComposed;

			const FVector4f OriginalColorF(
				OriginalRenderColor.R / 255.f, OriginalRenderColor.G / 255.f,
				OriginalRenderColor.B / 255.f, OriginalRenderColor.A / 255.f);
			const FVector4f Composite = ComposeFilteredColor(OriginalColorF, MaskValue, bFilterR, bFilterG, bFilterB, bFilterA);
			Result[i] = ToDisplayFColor(ApplyPreviewModeDisplay(Composite, Mode));
		}

		return Result;
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

		NewPreviewComponent->SetupAttachment(SourceComponent);
		NewPreviewComponent->SetRelativeTransform(FTransform::Identity);

		NewPreviewComponent->RegisterComponentWithWorld(SourceComponent->GetWorld());
		if (!NewPreviewComponent->IsRegistered())
		{
			// Partial failure (Problem 5): registration did not take. Undo the attachment and destroy
			// outright rather than leaving an unregistered, attached, strongly-referenced component.
			NewPreviewComponent->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
			NewPreviewComponent->DestroyComponent();
			return nullptr;
		}

		State.PreviewComponent = MoveTemp(StrongPreviewComponent);
		return NewPreviewComponent;
	}

	/**
	 * Writes render-order preview colors into the transient PreviewComponent's own
	 * OverrideVertexColors and swaps every material slot to the debug material. PreviewComponent is
	 * always our own freshly-created object, so there is no pre-existing state to snapshot or
	 * preserve here -- unlike SourceComponent, which this function never touches.
	 */
	static void ApplyPreviewColorsToPreviewComponent(
		UStaticMeshComponent* PreviewComponent,
		const TArray<FColor>& RenderOrderColors,
		UMaterialInterface* DebugMaterial)
	{
		if (!IsValid(PreviewComponent) || !DebugMaterial)
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
			PreviewComponent->SetMaterial(SlotIndex, DebugMaterial);
		}

		PreviewComponent->MarkRenderStateDirty();
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

		ApplyPreviewColorsToPreviewComponent(PreviewComponent, RenderOrderColors, DebugMaterial);
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
	 * Restores one component's preview state, in a fixed, documented order:
	 *   1. Mark not active immediately (so the "not active" observation is atomic with the start of
	 *      cleanup, independent of how far the rest of this function gets).
	 *   2. Copy out the raw pointers this function needs locally, while State's own fields still hold
	 *      them -- each field is only read once, here.
	 *   3. If a PreviewComponent existed: detach it (so no reference lingers in
	 *      SourceComponent->GetAttachChildren() -- DestroyComponent() alone does not detach, see the
	 *      audit note on EnsurePreviewComponent), then destroy it. UStaticMeshComponent::
	 *      DestroyComponent() (ActorComponent.cpp) already calls UnregisterComponent() internally
	 *      whenever IsRegistered() is true, so no separate explicit Unregister call is correct or
	 *      needed here. Then release the strong reference.
	 *   4. Release this State's Actor hide-reference (see ReleaseActorHidden), via the HiddenOwner
	 *      copied in step 2 rather than re-deriving the Actor from SourceComponent, so the release is
	 *      correct even if SourceComponent (or its owning Actor) has since been destroyed.
	 *   5. Clear the remaining fields.
	 * Idempotent by construction: a second call finds PreviewComponent already reset and
	 * bHasAcquiredActorHide already false, so steps 3 and 4 both no-op.
	 */
	static void RestoreComponentOriginal(
		FVertexMaskForgePreviewComponentState& State,
		TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState>& ActorHideStates)
	{
		// Step 1.
		State.bOverrideActive = false;

		// Step 2.
		UStaticMeshComponent* PreviewComponentPtr = State.PreviewComponent.Get();
		AActor* HiddenOwnerPtr = State.HiddenOwner.Get();
		const bool bHadAcquiredActorHide = State.bHasAcquiredActorHide;

		// Step 3.
		if (PreviewComponentPtr)
		{
			PreviewComponentPtr->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
			PreviewComponentPtr->DestroyComponent();
		}
		State.PreviewComponent.Reset();

		// Step 4.
		if (bHadAcquiredActorHide)
		{
			ReleaseActorHidden(ActorHideStates, HiddenOwnerPtr);
		}

		// Step 5.
		State.bHasAcquiredActorHide = false;
		State.HiddenOwner.Reset();
	}
}

void SVertexMaskForgePanel::Construct(const FArguments& InArgs)
{
	// AddRaw (not AddSP): registered/removed explicitly via WorldCleanupDelegateHandle in the
	// destructor, so there is no reliance on AsShared()/SharedThis() being valid at this point in
	// construction, and no ambiguity about callback lifetime -- the handle is always removed before
	// this object finishes destructing.
	WorldCleanupDelegateHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &SVertexMaskForgePanel::OnWorldCleanup);

	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::OriginalMaterial));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::RGBVertexColor));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::RedChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::GreenChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::BlueChannel));

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

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				SAssignNew(SummaryText, STextBlock)
				.Text(this, &SVertexMaskForgePanel::GetSummaryText)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				SNew(SBox)
				.MinDesiredHeight(160.f)
				[
					SNew(SOverlay)

					+ SOverlay::Slot()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("EmptyState", "No Static Meshes selected"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Visibility(this, &SVertexMaskForgePanel::GetEmptyStateVisibility)
					]

					+ SOverlay::Slot()
					[
						SAssignNew(ListView, SListView<TSharedPtr<FVertexMaskForgeSelectedMesh>>)
						.ListItemsSource(&SelectedMeshes)
						.SelectionMode(ESelectionMode::None)
						.OnGenerateRow(this, &SVertexMaskForgePanel::OnGenerateMeshRow)
						.Visibility(this, &SVertexMaskForgePanel::GetListVisibility)
					]
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RefreshSelection", "Refresh Selection"))
					.OnClicked(this, &SVertexMaskForgePanel::OnRefreshSelectionClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SelectionRefreshed", "Selection refreshed"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Visibility(this, &SVertexMaskForgePanel::GetRefreshedMessageVisibility)
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
					.Padding(FMargin(0.f, 0.f, 0.f, 2.f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("BBoxMaskSectionTitle", "Bounding Box Mask — Prototype"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("BBoxMaskSectionSubtitle", "Local Z — Bottom to Top"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
					]

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
							.Text(LOCTEXT("BBoxMaskPositionLabel", "Position"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(80.f)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Delta(0.01f)
							.Value(this, &SVertexMaskForgePanel::GetBoundingBoxMaskPosition)
							.OnValueChanged(this, &SVertexMaskForgePanel::OnBoundingBoxMaskPositionChanged)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("BBoxMaskTransitionWidthLabel", "Transition Width"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(4.f, 0.f, 12.f, 0.f))
						[
							SNew(SSpinBox<float>)
							.MinDesiredWidth(80.f)
							.MinValue(0.001f)
							.MaxValue(1.0f)
							.Delta(0.01f)
							.Value(this, &SVertexMaskForgePanel::GetBoundingBoxMaskTransitionWidth)
							.OnValueChanged(this, &SVertexMaskForgePanel::OnBoundingBoxMaskTransitionWidthChanged)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetBoundingBoxMaskInvertState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnBoundingBoxMaskInvertChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("BBoxMaskInvertLabel", "Invert"))
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Left)
					[
						SNew(SButton)
						.Text(LOCTEXT("GenerateMask", "Generate Mask"))
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
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("PreviewModeLabel", "Preview Mode"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
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

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetChannelFilterAState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnChannelFilterAChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ChannelFilterA", "A"))
							]
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SVertexMaskForgePanel::GetPreviewStatusText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
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

	// Idempotent: safe even if a preview was never activated, was already restored, or was already
	// cleaned up by a prior OnWorldCleanup call for its World.
	DestroyAllPreviews();
}

FReply SVertexMaskForgePanel::OnRefreshSelectionClicked()
{
	RefreshSelection();
	return FReply::Handled();
}

void SVertexMaskForgePanel::RefreshSelection()
{
	// Restore/destroy any active preview on the entries about to be discarded, before they (and
	// their PreviewComponents) are replaced. Rebuilding working meshes always resets
	// BoundingBoxZMask to NotGenerated on the new entries, so nothing here needs to "invalidate" a
	// mask -- there is no old one left to invalidate once SelectedMeshes is replaced.
	DestroyAllPreviews();

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> NewSelection;
	TMap<FString, int32> PathToIndex;

	CollectViewportSelection(NewSelection, PathToIndex);
	CollectContentBrowserSelection(NewSelection, PathToIndex);
	UpdateMeshDiagnostics(NewSelection);
	BuildWorkingMeshes(NewSelection);

	SelectedMeshes = MoveTemp(NewSelection);

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}

	bHasRefreshedOnce = true;

	UE_LOG(LogVertexMaskForge, Log, TEXT("Refreshed selection: %d unique Static Mesh asset(s)"), SelectedMeshes.Num());

	// New entries always start with BoundingBoxZMask == NotGenerated; if a Vertex Color preview
	// mode is still selected, this shows original colors + "Mask Not Ready" rather than anything stale.
	UpdateAllPreviews();
}

void SVertexMaskForgePanel::CollectViewportSelection(
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
	TMap<FString, int32>& InOutPathToIndex) const
{
	if (!GEditor)
	{
		return;
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		return;
	}

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
			if (!IsValid(Component))
			{
				continue;
			}

			UStaticMesh* Mesh = Component->GetStaticMesh();
			if (!IsValid(Mesh))
			{
				continue;
			}

			VertexMaskForgePanel::AddOrUpdateSelectedMesh(
				InOutMeshes,
				InOutPathToIndex,
				FSoftObjectPath(Mesh).ToString(),
				Mesh->GetName(),
				TSoftObjectPtr<UStaticMesh>(Mesh),
				EVertexMaskForgeSelectionSource::Viewport,
				Component);
		}
	}
}

void SVertexMaskForgePanel::CollectContentBrowserSelection(
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
	TMap<FString, int32>& InOutPathToIndex) const
{
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	const FTopLevelAssetPath StaticMeshClassPath = UStaticMesh::StaticClass()->GetClassPathName();

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (!AssetData.IsValid())
		{
			continue;
		}

		if (AssetData.AssetClassPath != StaticMeshClassPath)
		{
			continue;
		}

		const TSoftObjectPtr<UStaticMesh> SoftMesh(AssetData.GetSoftObjectPath());

		VertexMaskForgePanel::AddOrUpdateSelectedMesh(
			InOutMeshes,
			InOutPathToIndex,
			AssetData.GetSoftObjectPath().ToString(),
			AssetData.AssetName.ToString(),
			SoftMesh,
			EVertexMaskForgeSelectionSource::ContentBrowser);
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

TSharedRef<ITableRow> SVertexMaskForgePanel::OnGenerateMeshRow(
	TSharedPtr<FVertexMaskForgeSelectedMesh> InItem,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	const FText SourceLabel = InItem.IsValid()
		? VertexMaskForgePanel::GetSourceLabel(InItem->Sources)
		: FText::GetEmpty();

	return SNew(STableRow<TSharedPtr<FVertexMaskForgeSelectedMesh>>, OwnerTable)
		.Padding(FMargin(4.f, 3.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(InItem.IsValid() ? FText::FromString(InItem->AssetName) : FText::GetEmpty())
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text(SourceLabel)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(InItem.IsValid() ? FText::FromString(InItem->AssetPathString) : FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(InItem.IsValid()
					? VertexMaskForgePanel::GetDiagnosticsSummaryText(InItem->Diagnostics)
					: FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(InItem.IsValid()
					? VertexMaskForgePanel::GetWorkingMeshSummaryText(InItem->WorkingMesh)
					: FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(InItem.IsValid()
					? VertexMaskForgePanel::GetColorStatsSummaryText(InItem->WorkingMesh)
					: FText::GetEmpty())
				.Visibility(InItem.IsValid()
					&& InItem->WorkingMesh.VertexColorState == EVertexMaskForgeWorkingVertexColorState::Present
					? EVisibility::Visible : EVisibility::Collapsed)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				// Bound as a live lambda, not a plain FText: SListView::RequestListRefresh()
				// reuses an existing row widget for an unchanged item identity (see
				// SListView::GenerateWidgetForItem, which only calls OnRefreshRow -- never
				// OnGenerateRow again -- when a widget already exists for that TSharedPtr). Since
				// Generate Mask mutates InItem->WorkingMesh.BoundingBoxZMask in place rather than
				// replacing the entry, a plain FText captured once at row-creation time would stay
				// frozen at whatever it was when the row was first generated (right after Refresh
				// Selection, i.e. NotGenerated). A lambda re-evaluates every time Slate paints it.
				.Text_Lambda([InItem]()
				{
					return InItem.IsValid()
						? VertexMaskForgePanel::GetBoundingBoxMaskSummaryText(InItem->WorkingMesh.BoundingBoxZMask)
						: FText::GetEmpty();
				})
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.AutoWrapText(true)
			]
		];
}

FText SVertexMaskForgePanel::GetSummaryText() const
{
	return FText::Format(LOCTEXT("SummaryFormat", "Selected Static Meshes: {0}"), FText::AsNumber(SelectedMeshes.Num()));
}

EVisibility SVertexMaskForgePanel::GetEmptyStateVisibility() const
{
	return SelectedMeshes.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SVertexMaskForgePanel::GetListVisibility() const
{
	return SelectedMeshes.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SVertexMaskForgePanel::GetRefreshedMessageVisibility() const
{
	return bHasRefreshedOnce ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SVertexMaskForgePanel::OnGenerateBoundingBoxMaskClicked()
{
	int32 NumReady = 0;
	int32 NumUnavailable = 0;
	int32 NumDegenerate = 0;
	int32 NumInvalid = 0;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		// The working mesh (FDynamicMesh3) itself is no longer the source for this generator (see
		// GenerateBoundingBoxZMask's audit note), but Ready is kept as the entry-level precondition
		// for consistency with the rest of the panel's pipeline/UX (an entry whose working mesh
		// failed to build is flagged Unavailable across the board).
		if (Entry->WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready)
		{
			Entry->WorkingMesh.BoundingBoxZMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.BoundingBoxZMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			++NumUnavailable;
			continue;
		}

		// Resolved only for the duration of this call, consistent with the rest of the panel.
		const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
		if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
		{
			Entry->WorkingMesh.BoundingBoxZMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.BoundingBoxZMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			++NumUnavailable;
			continue;
		}

		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			Entry->WorkingMesh.BoundingBoxZMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.BoundingBoxZMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			++NumUnavailable;
			continue;
		}

		Entry->WorkingMesh.BoundingBoxZMask = VertexMaskForgePanel::GenerateBoundingBoxZMask(
			RenderData->LODResources[0], BoundingBoxMaskPosition, BoundingBoxMaskTransitionWidth, bBoundingBoxMaskInvert);

		switch (Entry->WorkingMesh.BoundingBoxZMask.State)
		{
		case EVertexMaskForgeScalarMaskState::Ready:
			++NumReady;
			break;
		case EVertexMaskForgeScalarMaskState::DegenerateBounds:
			++NumDegenerate;
			break;
		case EVertexMaskForgeScalarMaskState::Invalid:
			++NumInvalid;
			break;
		case EVertexMaskForgeScalarMaskState::Unavailable:
		case EVertexMaskForgeScalarMaskState::NotGenerated:
		default:
			++NumUnavailable;
			break;
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Built Bounding Box Z masks: %d ready; %d unavailable; %d degenerate; %d invalid"),
		NumReady, NumUnavailable, NumDegenerate, NumInvalid);

	// If a Vertex Color preview mode is active, recompose and reapply immediately using the
	// mask(s) just persisted -- the user should not have to reselect the dropdown.
	UpdateAllPreviews();

	return FReply::Handled();
}

void SVertexMaskForgePanel::OnBoundingBoxMaskPositionChanged(const float NewValue)
{
	BoundingBoxMaskPosition = NewValue;
	InvalidateBoundingBoxMasks();
}

void SVertexMaskForgePanel::OnBoundingBoxMaskTransitionWidthChanged(const float NewValue)
{
	BoundingBoxMaskTransitionWidth = NewValue;
	InvalidateBoundingBoxMasks();
}

ECheckBoxState SVertexMaskForgePanel::GetBoundingBoxMaskInvertState() const
{
	return bBoundingBoxMaskInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SVertexMaskForgePanel::OnBoundingBoxMaskInvertChanged(const ECheckBoxState NewState)
{
	bBoundingBoxMaskInvert = (NewState == ECheckBoxState::Checked);
	InvalidateBoundingBoxMasks();
}

void SVertexMaskForgePanel::InvalidateBoundingBoxMasks()
{
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			// Reset only the mask; the working mesh (FDynamicMesh3) itself is left untouched.
			Entry->WorkingMesh.BoundingBoxZMask = FVertexMaskForgeScalarMask();
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}

	// Any preview color derived from the now-stale mask must stop being shown immediately.
	UpdateAllPreviews();
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
	UpdateAllPreviews();
}

FText SVertexMaskForgePanel::GetPreviewModeButtonText() const
{
	return VertexMaskForgePanel::GetPreviewModeLabel(CurrentPreviewMode);
}

void SVertexMaskForgePanel::OnChannelFilterRChanged(const ECheckBoxState NewState)
{
	bChannelFilterR = (NewState == ECheckBoxState::Checked);
	UpdateAllPreviews();
}

void SVertexMaskForgePanel::OnChannelFilterGChanged(const ECheckBoxState NewState)
{
	bChannelFilterG = (NewState == ECheckBoxState::Checked);
	UpdateAllPreviews();
}

void SVertexMaskForgePanel::OnChannelFilterBChanged(const ECheckBoxState NewState)
{
	bChannelFilterB = (NewState == ECheckBoxState::Checked);
	UpdateAllPreviews();
}

void SVertexMaskForgePanel::OnChannelFilterAChanged(const ECheckBoxState NewState)
{
	bChannelFilterA = (NewState == ECheckBoxState::Checked);
	UpdateAllPreviews();
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
		if (Entry->WorkingMesh.BoundingBoxZMask.State == EVertexMaskForgeScalarMaskState::Ready)
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

void SVertexMaskForgePanel::RestorePreviewForEntry(FVertexMaskForgeSelectedMesh& Entry)
{
	for (FVertexMaskForgePreviewComponentState& State : Entry.PreviewComponents)
	{
		VertexMaskForgePanel::RestoreComponentOriginal(State, ActorHideStates);
	}
}

void SVertexMaskForgePanel::ApplyPreviewToEntry(const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry)
{
	if (!Entry.IsValid())
	{
		return;
	}

	if (CurrentPreviewMode == EVertexMaskForgePreviewMode::OriginalMaterial)
	{
		RestorePreviewForEntry(*Entry);
		return;
	}

	if (Entry->PreviewComponents.IsEmpty())
	{
		// Content-Browser-only entry: nothing in the viewport to visualize. Not an error --
		// GetPreviewStatusText() communicates this explicitly.
		return;
	}

	const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->WorkingMesh;
	if (WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready
		|| !WorkingMesh.Mesh.IsValid()
		|| WorkingMesh.BoundingBoxZMask.State != EVertexMaskForgeScalarMaskState::Ready)
	{
		// Nothing safe to preview yet (mask NotGenerated/Unavailable/DegenerateBounds/Invalid):
		// show the original colors/materials rather than a stale or fabricated result.
		RestorePreviewForEntry(*Entry);
		return;
	}

	// Resolved only for the duration of this call; consistent with the rest of the panel's
	// pattern of never storing a raw UStaticMesh pointer.
	const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
	if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
	{
		RestorePreviewForEntry(*Entry);
		return;
	}

	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
	{
		RestorePreviewForEntry(*Entry);
		return;
	}

	UMaterialInterface* DebugMaterial = GetPreviewDebugMaterial();
	if (!DebugMaterial)
	{
		RestorePreviewForEntry(*Entry);
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

		// Read-only: this buffer belongs to SourceComponent and is never modified by the plugin.
		const FColorVertexBuffer* InstanceOverrideColors =
			SourceComponent->LODData.IsValidIndex(0) ? SourceComponent->LODData[0].OverrideVertexColors : nullptr;

		// TEMPORARY diagnostic (audited render-vertex-order fix): NumComposed vs the LOD's render
		// vertex count directly proves the 1:1 correspondence in the log, per-component, every time
		// the preview is (re)applied.
		int32 NumComposed = 0;
		const TArray<FColor> RenderOrderColors = VertexMaskForgePanel::ComposeRenderOrderPreviewColors(
			WorkingMesh.BoundingBoxZMask, RenderData->LODResources[0], InstanceOverrideColors,
			CurrentPreviewMode, bChannelFilterR, bChannelFilterG, bChannelFilterB, bChannelFilterA,
			NumComposed);

		const int32 NumRenderVertsForLog = static_cast<int32>(RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices());
		UE_LOG(LogVertexMaskForge, Verbose,
			TEXT("Vertex Mask Forge: composed %d/%d render vertices for '%s' on component '%s'."),
			NumComposed, NumRenderVertsForLog, *Entry->AssetName, *SourceComponent->GetName());

		VertexMaskForgePanel::ActivatePreviewForComponent(State, RenderOrderColors, DebugMaterial, ActorHideStates);
	}
}

void SVertexMaskForgePanel::UpdateAllPreviews()
{
	int32 NumReady = 0;
	int32 NumFallback = 0;
	int32 NumUnavailable = 0;
	int32 NumInvalid = 0;

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		ApplyPreviewToEntry(Entry);

		if (Entry->PreviewComponents.IsEmpty())
		{
			continue; // Content-Browser-only; not counted in the viewport preview tally.
		}

		switch (Entry->WorkingMesh.BoundingBoxZMask.State)
		{
		case EVertexMaskForgeScalarMaskState::Ready:
			++NumReady;
			break;
		case EVertexMaskForgeScalarMaskState::Invalid:
			++NumInvalid;
			break;
		case EVertexMaskForgeScalarMaskState::Unavailable:
		case EVertexMaskForgeScalarMaskState::DegenerateBounds:
			++NumUnavailable;
			break;
		case EVertexMaskForgeScalarMaskState::NotGenerated:
		default:
			++NumFallback;
			break;
		}
	}

	if (CurrentPreviewMode != EVertexMaskForgePreviewMode::OriginalMaterial)
	{
		UE_LOG(LogVertexMaskForge, Log,
			TEXT("Updated Vertex Mask Forge preview: %d ready; %d original-color fallback; %d unavailable; %d invalid"),
			NumReady, NumFallback, NumUnavailable, NumInvalid);
	}
}

void SVertexMaskForgePanel::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (!World)
	{
		return;
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
