#include "SVertexMaskForgePanel.h"

#include "AssetRegistry/AssetData.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Set.h"
#include "ContentBrowserModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "Logging/LogMacros.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
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
		const EVertexMaskForgeSelectionSource Source)
	{
		if (const int32* ExistingIndex = InOutPathToIndex.Find(AssetPathString))
		{
			InOutMeshes[*ExistingIndex]->Sources |= Source;
			return;
		}

		TSharedPtr<FVertexMaskForgeSelectedMesh> NewEntry = MakeShared<FVertexMaskForgeSelectedMesh>();
		NewEntry->Mesh = SoftMesh;
		NewEntry->AssetName = AssetName;
		NewEntry->AssetPathString = AssetPathString;
		NewEntry->Sources = Source;

		InOutPathToIndex.Add(AssetPathString, InOutMeshes.Num());
		InOutMeshes.Add(MoveTemp(NewEntry));
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
}

void SVertexMaskForgePanel::Construct(const FArguments& InArgs)
{
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
		]
	];

	RefreshSelection();
}

SVertexMaskForgePanel::~SVertexMaskForgePanel() = default;

FReply SVertexMaskForgePanel::OnRefreshSelectionClicked()
{
	RefreshSelection();
	return FReply::Handled();
}

void SVertexMaskForgePanel::RefreshSelection()
{
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

		for (const UStaticMeshComponent* Component : Components)
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
				EVertexMaskForgeSelectionSource::Viewport);
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

#undef LOCTEXT_NAMESPACE
