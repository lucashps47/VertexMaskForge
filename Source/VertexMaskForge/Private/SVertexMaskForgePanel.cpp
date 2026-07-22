#include "SVertexMaskForgePanel.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Set.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
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
	 * in every space), so it feeds the exact same downstream composition
	 * (ComposeRenderOrderPreviewColors) and Accept path (BuildAcceptTargets/WriteAcceptTargets) as
	 * the Bounding Box Mask, with no parallel code path.
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

	/**
	 * Reduces a composed RGBA color to what the given Preview Mode should actually display.
	 *
	 * AUDITED (Alpha checkpoint): AlphaChannel follows the EXACT same pattern already established
	 * for Red/Green/Blue -- Composite.W (the real, fully-composed Alpha, never forced to 1 upstream
	 * in ComposeFilteredColor/ComposeRenderOrderPreviewColors) is read out as grayscale (A=0 -> black,
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
	 * GenerateBoundingBoxMask), so the mask value for render vertex i is simply Mask.TryGetValue(i,
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
	 *     white -- see ComposeRenderOrderPreviewColors);
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
	 */
	static bool BuildAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const EVertexMaskForgePreviewMode CurrentPreviewMode,
		const bool bFilterR, const bool bFilterG, const bool bFilterB, const bool bFilterA,
		const bool bUseUnifiedBounds,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& BoundingBoxAxisParams,
		TArray<FVertexMaskForgeAcceptTarget>& OutTargets,
		FText& OutErrorText)
	{
		OutTargets.Reset();

		if (CurrentPreviewMode == EVertexMaskForgePreviewMode::OriginalMaterial)
		{
			OutErrorText = LOCTEXT("AcceptNoActivePreview", "No active Preview to accept -- select a Preview Mode other than Original Material.");
			return false;
		}

		// Computed ONCE for the whole Accept operation, exactly mirroring UpdateAllPreviews(), so
		// Accept validates and writes against the SAME collective domain the Preview just showed --
		// never a separately-recomputed one. Blocks the ENTIRE Accept atomically on failure (per the
		// "validate everything before any write" contract), not just the entries using it.
		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> CollectiveBounds;
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr = nullptr;
		if (bUseUnifiedBounds)
		{
			if (!ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/false, CollectiveBounds, OutErrorText))
			{
				return false;
			}
			CollectiveBoundsPtr = &CollectiveBounds;
		}

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid() || Entry->PreviewComponents.IsEmpty()
				|| Entry->WorkingMesh.BoundingBoxMask.State != EVertexMaskForgeScalarMaskState::Ready)
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

				// AUDITED (World Space checkpoint): re-evaluate the mask with THIS component's own
				// ComponentTransform when Source == BoundingBox -- exactly mirroring
				// SVertexMaskForgePanel::ApplyPreviewToEntry, so Accept's divergence check operates
				// on the SAME per-instance results the Preview actually showed. A component whose
				// per-instance evaluation is not Ready (e.g. degenerate only in World Space for this
				// specific transform) is skipped here exactly as its Preview already fell back to
				// this component's original appearance -- it contributes nothing to compare or write.
				FVertexMaskForgeScalarMask PerComponentMask;
				const FVertexMaskForgeScalarMask* EffectiveMask = &Entry->WorkingMesh.BoundingBoxMask;
				if (Entry->WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::BoundingBox)
				{
					PerComponentMask = GenerateBoundingBoxMask(
						LOD0, Entry->WorkingMesh.BoundingBoxMask.UsedAxisParams, SourceComponent->GetComponentTransform(),
						CollectiveBoundsPtr);
					if (PerComponentMask.State != EVertexMaskForgeScalarMaskState::Ready)
					{
						continue;
					}
					EffectiveMask = &PerComponentMask;
				}

				const FColorVertexBuffer* InstanceOverrideColors =
					SourceComponent->LODData.IsValidIndex(0) ? SourceComponent->LODData[0].OverrideVertexColors : nullptr;

				int32 NumComposedUnused = 0;
				TArray<FColor> ComponentColors = ComposeRenderOrderPreviewColors(
					*EffectiveMask, LOD0, InstanceOverrideColors, CurrentPreviewMode,
					bFilterR, bFilterG, bFilterB, bFilterA, NumComposedUnused);

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
			const FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			if (!MeshDescription
				|| LOD0.WedgeMap.Num() == 0
				|| LOD0.WedgeMap.Num() != MeshDescription->VertexInstances().Num()
				|| ReferenceColors.Num() != static_cast<int32>(LOD0.GetNumVertices()))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptNoWedgeMapFormat",
						"'{0}': no deterministic wedge-to-render-vertex mapping is available (FStaticMeshLODResources::WedgeMap missing or stale for LOD 0). Refusing to write -- an approximate position-based remap could paint seams incorrectly. Try rebuilding this Static Mesh (Build) and Accept again."),
					FText::FromString(Entry->AssetName));
				return false;
			}

			FVertexMaskForgeAcceptTarget Target;
			Target.Mesh = Mesh;
			Target.AssetName = Entry->AssetName;
			Target.FinalColors = MoveTemp(ReferenceColors);
			OutTargets.Add(MoveTemp(Target));
		}

		if (OutTargets.IsEmpty())
		{
			OutErrorText = LOCTEXT("AcceptNothingEligible", "No eligible pending changes to accept.");
			return false;
		}

		return true;
	}

	/**
	 * Writes every target's FinalColors into its Static Mesh asset's LOD 0 MeshDescription (via the
	 * SAME WedgeMap-based approach as UMeshPaintingSubsystem::PropagateColorsToRawMesh -- see the
	 * audit note on BuildAcceptTargets), inside a single FScopedTransaction so Accept is one coherent
	 * Undo step. Re-validates every target BEFORE opening the transaction or modifying anything --
	 * once the write loop starts, every step is expected to succeed by construction (validated moments
	 * earlier, synchronously, nothing else runs in between), so there is no mid-loop rollback path:
	 * if this function ever returns false, NOTHING has been modified and no transaction was opened.
	 *
	 * Mesh->Modify() is called before editing so Undo restores the previous colors (UStaticMesh::
	 * PostEditUndo() already triggers Build() automatically via PostEditChangeProperty() on Undo/Redo
	 * -- see StaticMesh.cpp -- so no extra Undo/Redo handling is needed here). Mesh->Build() is called
	 * once per asset AFTER all of its colors are committed, to regenerate RenderData (including a
	 * fresh FColorVertexBuffer) from the edited MeshDescription -- the same order
	 * UMeshPaintModeSubsystem::PropagateVertexColors uses.
	 */
	static bool WriteAcceptTargets(const TArray<FVertexMaskForgeAcceptTarget>& Targets, FText& OutErrorText)
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

		FScopedTransaction Transaction(LOCTEXT("AcceptVertexMaskForgeChanges", "Accept Vertex Mask Forge Changes"));

		TArray<UStaticMesh*> ModifiedMeshes;
		ModifiedMeshes.Reserve(Targets.Num());

		for (const FVertexMaskForgeAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];

			Mesh->Modify();

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
			ModifiedMeshes.Add(Mesh);
		}

		// Rebuild once per asset, after all of its colors are committed -- regenerates RenderData
		// (including a fresh FColorVertexBuffer) from the edited MeshDescription.
		for (UStaticMesh* Mesh : ModifiedMeshes)
		{
			Mesh->Build(/*bInSilent=*/true);
		}

		return true;
	}

	// --- Accept as Instance Override: permanent write to component(s), Source Static Mesh untouched --

	/**
	 * One selected UStaticMeshComponent instance targeted by "Accept as Instance Override", with its
	 * final, ready-to-write colors. NEVER deduplicated by UStaticMesh (unlike
	 * FVertexMaskForgeAcceptTarget, which is 1-per-asset) -- two components that happen to reference
	 * the same asset are independent targets here and can legitimately end up with different
	 * FinalColors (divergent per-instance baselines, or divergent per-instance World Space
	 * evaluation), since each writes only to its own component and never to the shared asset.
	 */
	struct FVertexMaskForgeInstanceOverrideTarget
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		/** Diagnostic-only label ("ComponentName (ActorLabel)"), used in error/status messages. */
		FString ComponentLabel;
		/** Render-vertex-order (LOD0), exactly as shown in Preview -- the data actually written. */
		TArray<FColor> FinalColors;
	};

	/**
	 * Validates every SelectedMeshes entry/component eligible for "Accept as Instance Override" and,
	 * only if ALL of them pass, returns one target PER live PreviewComponent. Mirrors
	 * BuildAcceptTargets' eligibility gate (Ready mask, at least one live PreviewComponent) and its
	 * per-component World Space mask re-evaluation exactly, but deliberately omits two things
	 * BuildAcceptTargets needs and this path does not:
	 *   - the cross-component "all instances must agree" check, since that check exists ONLY because
	 *     BuildAcceptTargets writes ONE shared asset from possibly-divergent per-instance baselines;
	 *     here every component gets its own independent write target, so divergence is expected and
	 *     harmless;
	 *   - any MeshDescription/WedgeMap validation, since this path never touches the asset's
	 *     MeshDescription at all -- only LOD0's render-vertex COUNT (from the asset's RenderData,
	 *     read-only) is needed to size/validate FinalColors.
	 */
	static bool BuildInstanceOverrideTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const EVertexMaskForgePreviewMode CurrentPreviewMode,
		const bool bFilterR, const bool bFilterG, const bool bFilterB, const bool bFilterA,
		const bool bUseUnifiedBounds,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& BoundingBoxAxisParams,
		TArray<FVertexMaskForgeInstanceOverrideTarget>& OutTargets,
		FText& OutErrorText)
	{
		OutTargets.Reset();

		if (CurrentPreviewMode == EVertexMaskForgePreviewMode::OriginalMaterial)
		{
			OutErrorText = LOCTEXT("InstanceOverrideNoActivePreview", "No active Preview to accept -- select a Preview Mode other than Original Material.");
			return false;
		}

		// Computed ONCE for the whole operation, exactly mirroring UpdateAllPreviews() / BuildAcceptTargets,
		// so this validates and writes against the SAME collective domain the Preview just showed.
		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> CollectiveBounds;
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr = nullptr;
		if (bUseUnifiedBounds)
		{
			if (!ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/false, CollectiveBounds, OutErrorText))
			{
				return false;
			}
			CollectiveBoundsPtr = &CollectiveBounds;
		}

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid() || Entry->PreviewComponents.IsEmpty()
				|| Entry->WorkingMesh.BoundingBoxMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				continue;
			}

			UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
			{
				OutErrorText = FText::Format(
					LOCTEXT("InstanceOverrideInvalidMeshFormat", "'{0}': Static Mesh could not be resolved or has no valid LOD 0 render data."),
					FText::FromString(Entry->AssetName));
				return false;
			}

			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				OutErrorText = FText::Format(
					LOCTEXT("InstanceOverrideNoRenderDataFormat", "'{0}': no LOD 0 render data available."),
					FText::FromString(Entry->AssetName));
				return false;
			}
			const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];

			for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
			{
				UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}

				// AUDITED (mirrors BuildAcceptTargets): re-evaluate the mask with THIS component's own
				// ComponentTransform when Source == BoundingBox, so this operates on the SAME per-instance
				// result the Preview actually showed for this component.
				FVertexMaskForgeScalarMask PerComponentMask;
				const FVertexMaskForgeScalarMask* EffectiveMask = &Entry->WorkingMesh.BoundingBoxMask;
				if (Entry->WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::BoundingBox)
				{
					PerComponentMask = GenerateBoundingBoxMask(
						LOD0, Entry->WorkingMesh.BoundingBoxMask.UsedAxisParams, SourceComponent->GetComponentTransform(),
						CollectiveBoundsPtr);
					if (PerComponentMask.State != EVertexMaskForgeScalarMaskState::Ready)
					{
						continue;
					}
					EffectiveMask = &PerComponentMask;
				}

				const FColorVertexBuffer* InstanceOverrideColors =
					SourceComponent->LODData.IsValidIndex(0) ? SourceComponent->LODData[0].OverrideVertexColors : nullptr;

				int32 NumComposedUnused = 0;
				TArray<FColor> ComponentColors = ComposeRenderOrderPreviewColors(
					*EffectiveMask, LOD0, InstanceOverrideColors, CurrentPreviewMode,
					bFilterR, bFilterG, bFilterB, bFilterA, NumComposedUnused);

				if (ComponentColors.Num() != static_cast<int32>(LOD0.GetNumVertices()))
				{
					// Defensive only: ComposeRenderOrderPreviewColors always sizes its result to
					// LOD0's own NumRenderVerts, so this cannot actually diverge in practice.
					continue;
				}

				FVertexMaskForgeInstanceOverrideTarget Target;
				Target.Component = SourceComponent;
				Target.ComponentLabel = FString::Printf(TEXT("%s (%s)"), *SourceComponent->GetName(),
					SourceComponent->GetOwner() ? *SourceComponent->GetOwner()->GetActorLabel() : TEXT("?"));
				Target.FinalColors = MoveTemp(ComponentColors);
				OutTargets.Add(MoveTemp(Target));
			}
		}

		if (OutTargets.IsEmpty())
		{
			OutErrorText = LOCTEXT("InstanceOverrideNothingEligible", "No eligible pending changes to accept.");
			return false;
		}

		return true;
	}

	/**
	 * Writes every target's FinalColors as permanent OverrideVertexColors (LOD0 only) directly onto
	 * its real UStaticMeshComponent, inside a single FScopedTransaction so this is one coherent Undo
	 * step. NEVER touches the component's Static Mesh asset -- no Mesh->Modify(),
	 * GetMeshDescription(), CommitMeshDescription(), Build(), or MarkPackageDirty() on the Static
	 * Mesh anywhere in this function. Only Component->Modify() is called, which (per UObject::Modify())
	 * marks the COMPONENT's own package (the level/Actor package) dirty -- the Static Mesh asset and
	 * its package are left completely untouched.
	 *
	 * AUDITED against the Engine's own Mesh Paint Editor Mode source (not improvised):
	 *   - The buffer-replace sequence -- ReleaseOverrideVertexColorsAndBlock() then `new
	 *     FColorVertexBuffer` + InitFromColorArray() + BeginInitResource() -- mirrors the "no existing
	 *     buffer / vertex count differs" branch of FMeshPaintStaticMeshComponentAdapter::PreEdit()
	 *     (Engine/Plugins/MeshPainting/Source/MeshPaintingToolset/Private/MeshPaintStaticMeshAdapter.cpp).
	 *     FStaticMeshComponentLODInfo::ReleaseOverrideVertexColorsAndBlock() (StaticMeshComponent.cpp)
	 *     nulls the member immediately, enqueues the OLD buffer's render-thread resource release, then
	 *     calls FlushRenderingCommands() to block until that release has actually completed -- so
	 *     assigning a brand-new buffer right after it returns can never race the render thread or leak
	 *     the old one.
	 *   - PaintedVertices.Empty() before the replace, and Component->CachePaintedDataIfNecessary()
	 *     after, mirror the same PreEdit() function: CachePaintedDataIfNecessary() (StaticMeshComponent.cpp)
	 *     only repopulates PaintedVertices (the position/normal/color cache
	 *     FixupOverrideColorsIfNecessary later uses to detect a stale override after the source mesh is
	 *     rebuilt) when PaintedVertices.Num() == 0; without the Empty() call here, a component that
	 *     already had override colors from an earlier session would keep its OLD (now-mismatched)
	 *     cache instead of one describing the colors just written.
	 *   - SetLODDataCount(1, Component->LODData.Num()) mirrors the exact
	 *     SetLODDataCount(MinSize, CurrentCount) call pattern used throughout StaticMeshComponent.cpp
	 *     (e.g. UStaticMeshComponent::ApplyComponentInstanceData): MinSize=1 only guarantees LOD0's
	 *     entry exists (creating it if the component never had one), MaxSize=current count never trims
	 *     -- so any pre-existing LOD1+ entries (if this component was ever painted through the Editor's
	 *     own Mesh Paint tool with per-LOD overrides) are left completely untouched, matching the
	 *     explicit LOD0-only scope of this checkpoint.
	 *   - MarkRenderStateDirty() (rather than a heavier FComponentReregisterContext) is the standard,
	 *     official trigger every UPrimitiveComponent property setter uses to recreate the scene proxy
	 *     from current state -- sufficient here since nothing else about the component (bounds,
	 *     collision, attachment) changes, only its override color buffer.
	 *
	 * Undo/Redo (audited, not assumed): UStaticMeshComponent::Serialize() unconditionally serializes
	 * LODData (`Ar << LODData` in StaticMeshComponent.cpp) regardless of LODData's own
	 * UPROPERTY(Transient) tag -- specifically so a full-object transaction (FScopedTransaction +
	 * Component->Modify(), both used below) can snapshot and later restore it. On Undo, TArray's
	 * element reconstruction for a transacted reload destructs the CURRENT FStaticMeshComponentLODInfo
	 * first (its destructor calls CleanUp(), which safely releases/deletes whatever OverrideVertexColors
	 * is live at that moment -- see FStaticMeshComponentLODInfo::~FStaticMeshComponentLODInfo()) before
	 * the archive reloads the OLD snapshot, whose own Ar.IsLoading() branch (operator<<(FArchive&,
	 * FStaticMeshComponentLODInfo&)) allocates a fresh FColorVertexBuffer and calls BeginInitResource()
	 * itself. So Undo safely restores "no override" if none existed before this write, and Redo safely
	 * reapplies these exact colors, with no leak and no dangling render-thread pointer either way. The
	 * viewport refresh after Undo/Redo is handled by the engine's own UActorComponent::PostEditUndo()
	 * (ActorComponent.cpp), which re-registers the component -- nothing extra is required here beyond
	 * calling Component->Modify() before mutating LODData, which this function does first.
	 */
	static bool WriteInstanceOverrideTargets(const TArray<FVertexMaskForgeInstanceOverrideTarget>& Targets, FText& OutErrorText)
	{
		for (const FVertexMaskForgeInstanceOverrideTarget& Target : Targets)
		{
			UStaticMeshComponent* Component = Target.Component.Get();
			UStaticMesh* Mesh = IsValid(Component) ? Component->GetStaticMesh() : nullptr;
			const FStaticMeshRenderData* RenderData = IsValid(Mesh) ? Mesh->GetRenderData() : nullptr;
			const bool bLODValid = RenderData && RenderData->LODResources.IsValidIndex(0);

			if (!IsValid(Component) || !bLODValid
				|| Target.FinalColors.Num() != static_cast<int32>(RenderData->LODResources[0].GetNumVertices()))
			{
				OutErrorText = FText::Format(
					LOCTEXT("InstanceOverrideWriteRevalidationFailedFormat", "'{0}' failed re-validation immediately before writing; aborting (nothing was modified)."),
					FText::FromString(Target.ComponentLabel));
				return false;
			}
		}

		FScopedTransaction Transaction(LOCTEXT("AcceptVertexMaskForgeInstanceOverride", "Accept Vertex Mask Forge Changes as Instance Override"));

		for (const FVertexMaskForgeInstanceOverrideTarget& Target : Targets)
		{
			UStaticMeshComponent* Component = Target.Component.Get();

			Component->Modify();
			Component->SetLODDataCount(1, Component->LODData.Num());

			FStaticMeshComponentLODInfo& LODInfo = Component->LODData[0];
			LODInfo.ReleaseOverrideVertexColorsAndBlock();
			LODInfo.PaintedVertices.Empty();
			LODInfo.OverrideVertexColors = new FColorVertexBuffer();
			LODInfo.OverrideVertexColors->InitFromColorArray(Target.FinalColors.GetData(), Target.FinalColors.Num());
			BeginInitResource(LODInfo.OverrideVertexColors);

			Component->CachePaintedDataIfNecessary();
			Component->MarkRenderStateDirty();
		}

		return true;
	}

	// --- Remove Instance Override: undo per-instance overrides, Source Static Mesh untouched --------

	/**
	 * True if Component's LOD0 currently has a non-empty Instance Vertex Color override -- i.e.
	 * something an "Accept as Instance Override" (or the Editor's own Mesh Paint tool) previously
	 * wrote that "Remove Instance Override" can meaningfully remove. False for a null/invalid
	 * Component, a Component with no LODData[0] entry yet, or an entry whose OverrideVertexColors is
	 * null or has zero vertices.
	 */
	static bool HasRemovableLOD0Override(const UStaticMeshComponent* Component)
	{
		if (!IsValid(Component) || !Component->LODData.IsValidIndex(0))
		{
			return false;
		}
		const FColorVertexBuffer* Override = Component->LODData[0].OverrideVertexColors;
		return Override != nullptr && Override->GetNumVertices() > 0;
	}

	/**
	 * One selected UStaticMeshComponent instance targeted by "Remove Instance Override". Unlike
	 * FVertexMaskForgeInstanceOverrideTarget, carries no color payload -- removal needs only the
	 * component identity. Never deduplicated by UStaticMesh; each selected component with a
	 * removable override is its own independent target.
	 */
	struct FVertexMaskForgeRemoveOverrideTarget
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		/** Diagnostic-only label ("ComponentName (ActorLabel)"), used in error messages. */
		FString ComponentLabel;
	};

	/**
	 * Collects one target per SELECTED component (from SelectedMeshes[*].PreviewComponents, exactly
	 * the same "real, currently-tracked selection" source BuildInstanceOverrideTargets reads) that
	 * currently HasRemovableLOD0Override(). Deliberately does NOT gate on
	 * WorkingMesh.BoundingBoxMask.State (Ready/NotGenerated/etc.) or on CurrentPreviewMode -- Remove
	 * Instance Override never depends on a generated mask or an active Preview, only on whether the
	 * component already carries an override from an earlier session. Never touches any Static Mesh
	 * asset or its RenderData -- HasRemovableLOD0Override reads only the component's own LODData.
	 */
	static bool BuildRemoveInstanceOverrideTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		TArray<FVertexMaskForgeRemoveOverrideTarget>& OutTargets,
		FText& OutErrorText)
	{
		OutTargets.Reset();

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
			{
				UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
				if (!HasRemovableLOD0Override(SourceComponent))
				{
					continue;
				}

				FVertexMaskForgeRemoveOverrideTarget Target;
				Target.Component = SourceComponent;
				Target.ComponentLabel = FString::Printf(TEXT("%s (%s)"), *SourceComponent->GetName(),
					SourceComponent->GetOwner() ? *SourceComponent->GetOwner()->GetActorLabel() : TEXT("?"));
				OutTargets.Add(MoveTemp(Target));
			}
		}

		if (OutTargets.IsEmpty())
		{
			OutErrorText = LOCTEXT("RemoveOverrideNothingEligible", "No selected component currently has a removable Instance Vertex Color override.");
			return false;
		}

		return true;
	}

	/**
	 * Removes the LOD0 Instance Vertex Color override from every target's real UStaticMeshComponent,
	 * inside a single FScopedTransaction so this is one coherent Undo step. NEVER touches the
	 * component's Static Mesh asset -- no Mesh->Modify(), GetMeshDescription(), CommitMeshDescription(),
	 * Build(), or MarkPackageDirty() on the Static Mesh anywhere in this function.
	 *
	 * AUDITED: uses the engine's own public API, UStaticMeshComponent::RemoveInstanceVertexColorsFromLOD(0)
	 * (StaticMeshComponent.h/.cpp) -- NOT the all-LODs UStaticMeshComponent::RemoveInstanceVertexColors()
	 * (which simply loops RemoveInstanceVertexColorsFromLOD() over every LOD index and would silently
	 * expand this plugin's scope past LOD0, which nothing else here supports yet). Reading the
	 * implementation: RemoveInstanceVertexColorsFromLOD(0) does exactly
	 * `LODData[0].ReleaseOverrideVertexColorsAndBlock(); LODData[0].PaintedVertices.Empty();` -- the
	 * SAME safe release this plugin's own WriteInstanceOverrideTargets already relies on for the
	 * opposite (write) direction -- plus (WITH_EDITORONLY_DATA) refreshing StaticMeshDerivedDataKey
	 * from the current Static Mesh's RenderData, keeping FixupOverrideColorsIfNecessary's staleness
	 * bookkeeping consistent. It touches ONLY LODData[0]; every other LOD entry (if this component was
	 * ever painted per-LOD via the Editor's own Mesh Paint tool) is left completely untouched.
	 *
	 * RemoveInstanceVertexColorsFromLOD() itself calls neither Modify() nor MarkRenderStateDirty() nor
	 * MarkPackageDirty() -- confirmed by reading it end to end -- so, exactly mirroring the call order
	 * UStaticMeshComponent::CopyInstanceVertexColorsIfCompatible() itself uses around the same API
	 * (Modify() first, removal, then a render-state refresh), this function calls Component->Modify()
	 * immediately before the removal and Component->MarkRenderStateDirty() immediately after.
	 * Component->Modify() marks the COMPONENT's own package (the level/Actor package) dirty; the
	 * Static Mesh asset and its package are never touched.
	 *
	 * Undo/Redo: identical mechanism already audited for WriteInstanceOverrideTargets, applied
	 * symmetrically. UStaticMeshComponent::Serialize() unconditionally serializes LODData regardless
	 * of its own UPROPERTY(Transient) tag, specifically so a full-object transaction can snapshot and
	 * restore it. On Undo, TArray's transacted-reload element reconstruction destructs the CURRENT
	 * FStaticMeshComponentLODInfo (harmless here -- its OverrideVertexColors is already null after the
	 * removal) before the archive reloads the OLD snapshot, whose Ar.IsLoading() branch allocates a
	 * fresh FColorVertexBuffer with the PRE-REMOVAL colors and calls BeginInitResource() itself -- so
	 * Undo safely brings the override back exactly as it was, and Redo safely removes it again, with
	 * no leak and no dangling render-thread pointer either way. Viewport refresh after Undo/Redo is
	 * handled by the engine's own UActorComponent::PostEditUndo(), same as the write path.
	 */
	static bool RemoveInstanceOverrideTargets(const TArray<FVertexMaskForgeRemoveOverrideTarget>& Targets, FText& OutErrorText)
	{
		for (const FVertexMaskForgeRemoveOverrideTarget& Target : Targets)
		{
			if (!HasRemovableLOD0Override(Target.Component.Get()))
			{
				OutErrorText = FText::Format(
					LOCTEXT("RemoveOverrideRevalidationFailedFormat", "'{0}' failed re-validation immediately before removing (no longer has a removable override); aborting (nothing was modified)."),
					FText::FromString(Target.ComponentLabel));
				return false;
			}
		}

		FScopedTransaction Transaction(LOCTEXT("RemoveVertexMaskForgeInstanceOverride", "Remove Vertex Mask Forge Instance Override"));

		for (const FVertexMaskForgeRemoveOverrideTarget& Target : Targets)
		{
			UStaticMeshComponent* Component = Target.Component.Get();

			Component->Modify();
			Component->RemoveInstanceVertexColorsFromLOD(0);
			Component->MarkRenderStateDirty();
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
		DetachAndDestroyPreviewComponent(PreviewComponentPtr);
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

void SVertexMaskForgePanel::OnAxisParamChangedDiscrete()
{
	InvalidateBoundingBoxMasks();
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
				InvalidateBoundingBoxMasks();
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
				InvalidateBoundingBoxMasks();
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
				BoundingBoxAxisParams[AxisIndex].bInvert = (NewState == ECheckBoxState::Checked);
				OnAxisParamChangedDiscrete();
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

	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::OriginalMaterial));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::RGBVertexColor));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::RedChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::GreenChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::BlueChannel));
	PreviewModeOptions.Add(MakeShared<EVertexMaskForgePreviewMode>(EVertexMaskForgePreviewMode::AlphaChannel));

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

			+ SVerticalBox::Slot()
			.AutoHeight()
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
						.Text(LOCTEXT("BBoxMaskSectionTitle", "Bounding Box Mask"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
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

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("GenerateMask", "Generate Mask"))
							.OnClicked(this, &SVertexMaskForgePanel::OnGenerateBoundingBoxMaskClicked)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(12.f, 0.f, 0.f, 0.f))
						[
							SNew(SCheckBox)
							.IsChecked(this, &SVertexMaskForgePanel::GetAutoUpdatePreviewState)
							.OnCheckStateChanged(this, &SVertexMaskForgePanel::OnAutoUpdatePreviewChanged)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AutoUpdatePreviewLabel", "Auto Update Preview"))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
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

					// Secondary row, explicitly alternative to the native Accept/Cancel row above: two
					// related-but-independent per-instance override actions, sharing the row's width
					// evenly so neither reads as more "primary" than the other. Kept on its own row
					// (not folded into the native Accept/Cancel row) so the whole pair still reads as
					// an alternative, not a third/fourth equally-weighted primary action. Each keeps
					// its own tooltip, enabled condition, and callback -- never combined.
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.Text(LOCTEXT("AcceptAsInstanceOverride", "Accept as Instance Override"))
							.ToolTipText(LOCTEXT("AcceptAsInstanceOverrideTooltip", "Stores the generated Vertex Colors as overrides on the selected component instances without modifying the Source Static Mesh. The result persists when the level is saved."))
							.OnClicked(this, &SVertexMaskForgePanel::OnAcceptAsInstanceOverrideClicked)
							.IsEnabled(this, &SVertexMaskForgePanel::CanAcceptAsInstanceOverride)
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.Text(LOCTEXT("RemoveInstanceOverride", "Remove Instance Override"))
							.ToolTipText(LOCTEXT("RemoveInstanceOverrideTooltip", "Removes Vertex Color overrides from the selected component instances. The components will return to the Vertex Colors stored in their Source Static Mesh. The Source Static Mesh will not be modified."))
							.OnClicked(this, &SVertexMaskForgePanel::OnRemoveInstanceOverrideClicked)
							.IsEnabled(this, &SVertexMaskForgePanel::CanRemoveInstanceOverride)
						]
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
	// Accept / Accept as Instance Override / Cancel are always visible and available regardless of
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
	// called at the tail of OnCancelChangesClicked() / AcceptPendingChanges() /
	// AcceptPendingChangesAsInstanceOverride() (and ONLY there -- see its own doc comment), consumes
	// this flag and calls RefreshSelection() automatically once the operation has fully concluded
	// against its ORIGINAL targets. So Accept/Cancel/Accept as Instance Override remain fully able to
	// act on the original selection at any time, and the panel still catches up with a changed scene
	// selection automatically, without ever retargeting the operation itself.
	if (OperationState != EVertexMaskForgeOperationState::Idle)
	{
		bSceneSelectionChangedDuringActiveOperation = true;
		return;
	}

	// Already Idle: this call itself is about to sync SelectedMeshes with the current scene selection
	// directly, so any flag left over from an earlier session (e.g. OperationState settled back to
	// Idle through a path other than Cancel/Accept/Accept as Instance Override, such as a mask being
	// invalidated with no PreviewComponents left) is moot -- clear it defensively so a later Cancel/
	// Accept/Accept as Instance Override never performs a redundant extra refresh for a selection
	// change this call already picked up.
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

	bool bAnyAxisEnabled = false;
	for (const FVertexMaskForgeAxisMaskParams& Params : BoundingBoxAxisParams)
	{
		if (Params.bEnabled)
		{
			bAnyAxisEnabled = true;
			break;
		}
	}
	if (!bAnyAxisEnabled)
	{
		// Per the explicit requirement: never generate an empty mask silently, never replace the
		// previous Preview, never enter Pending Changes with invalid data.
		LastOperationErrorText = LOCTEXT("NoAxisEnabled", "Enable at least one Bounding Box axis.");
		RecomputeOperationState();
		return FReply::Handled();
	}

	// Computed ONCE for this whole click (batch), before touching any entry's mask -- Unified Bounds
	// must never regenerate just one mesh. Participation uses bForGeneration=true (every entry with
	// a Ready working mesh, since generation is what's about to populate their BoundingBoxMask).
	TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> CollectiveBounds;
	const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr = nullptr;
	if (bUseUnifiedBounds)
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
		// GenerateBoundingBoxMask's audit note), but Ready is kept as the entry-level precondition
		// for consistency with the rest of the panel's pipeline/UX (an entry whose working mesh
		// failed to build is flagged Unavailable across the board).
		if (Entry->WorkingMesh.State != EVertexMaskForgeWorkingMeshState::Ready)
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.BoundingBoxMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			++NumUnavailable;
			continue;
		}

		// Resolved only for the duration of this call, consistent with the rest of the panel.
		const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
		if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.BoundingBoxMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			++NumUnavailable;
			continue;
		}

		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
			Entry->WorkingMesh.BoundingBoxMask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			++NumUnavailable;
			continue;
		}

		// Entry-level reference evaluation: the first live PreviewComponent's transform (for World
		// Space axes), or Identity if this entry has none (Content-Browser-only, or no components
		// currently valid) -- see the audit note on FVertexMaskForgeWorkingMesh::BoundingBoxMask.
		// Actual per-instance Preview/Accept composition re-evaluates per component when needed.
		FTransform ReferenceTransform = FTransform::Identity;
		for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
		{
			if (const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get())
			{
				ReferenceTransform = SourceComponent->GetComponentTransform();
				break;
			}
		}

		Entry->WorkingMesh.BoundingBoxMask = VertexMaskForgePanel::GenerateBoundingBoxMask(
			RenderData->LODResources[0], BoundingBoxAxisParams, ReferenceTransform, CollectiveBoundsPtr);
		Entry->WorkingMesh.BoundingBoxMask.SelectionMeshCount = SelectedMeshes.Num();

		switch (Entry->WorkingMesh.BoundingBoxMask.State)
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

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Built Bounding Box masks: %d ready; %d unavailable; %d degenerate; %d invalid"),
		NumReady, NumUnavailable, NumDegenerate, NumInvalid);

	// If a Vertex Color preview mode is active, recompose and reapply immediately using the
	// mask(s) just persisted -- the user should not have to reselect the dropdown.
	UpdateAllPreviews();

	return FReply::Handled();
}

void SVertexMaskForgePanel::InvalidateBoundingBoxMasks()
{
	LastMaskActionStatusText = FText::GetEmpty();

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (Entry.IsValid())
		{
			// Reset only the mask; the working mesh (FDynamicMesh3) itself is left untouched.
			Entry->WorkingMesh.BoundingBoxMask = FVertexMaskForgeScalarMask();
		}
	}

	// Any preview color derived from the now-stale mask must stop being shown immediately -- true
	// regardless of Auto Update Preview: the OLD mask is stale the instant a parameter changes, even
	// if a new one hasn't been (re)generated yet.
	UpdateAllPreviews();
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

		FVertexMaskForgeScalarMask NewMask = VertexMaskForgePanel::GenerateConstantMask(RenderData->LODResources[0], ConstantValue, Source);
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

	// Recomposes/reapplies via the exact same ApplyPreviewToEntry/ComposeRenderOrderPreviewColors
	// path as every other mask, and marks Pending Changes via RecomputeOperationState() at the end.
	UpdateAllPreviews();
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
	// existing result is stale regardless of Auto Update Preview.
	InvalidateBoundingBoxMasks();

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
		FTimerDelegate::CreateSP(this, &SVertexMaskForgePanel::RunAutoUpdatePreview),
		DebounceSeconds,
		/*bLoop=*/false);
}

void SVertexMaskForgePanel::RunAutoUpdatePreview()
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

	bool bAnyAxisEnabled = false;
	for (const FVertexMaskForgeAxisMaskParams& Params : BoundingBoxAxisParams)
	{
		if (Params.bEnabled)
		{
			bAnyAxisEnabled = true;
			break;
		}
	}
	if (!bAnyAxisEnabled)
	{
		// Preserve every entry's existing mask untouched and surface the specific message -- do not
		// fall through to the generic "could not be regenerated" wording below.
		LastOperationErrorText = LOCTEXT("NoAxisEnabledAutoUpdate", "Enable at least one Bounding Box axis.");
		UpdateAllPreviews();
		return;
	}

	// Computed ONCE for this whole regeneration (batch), before touching any entry's mask -- Unified
	// Bounds must never recompute just one mesh. A failure here is global (not per-entry), so it is
	// treated like "no axis enabled": preserve every entry's existing mask, surface the specific
	// message, and do not attempt any per-entry regeneration this pass.
	TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> CollectiveBounds;
	const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr = nullptr;
	if (bUseUnifiedBounds)
	{
		FText CollectiveError;
		if (!VertexMaskForgePanel::ComputeCollectiveAxisBounds(SelectedMeshes, BoundingBoxAxisParams, /*bForGeneration=*/true, CollectiveBounds, CollectiveError))
		{
			LastOperationErrorText = CollectiveError;
			UpdateAllPreviews();
			return;
		}
		CollectiveBoundsPtr = &CollectiveBounds;
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
		for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
		{
			if (const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get())
			{
				ReferenceTransform = SourceComponent->GetComponentTransform();
				break;
			}
		}

		FVertexMaskForgeScalarMask NewMask = VertexMaskForgePanel::GenerateBoundingBoxMask(
			RenderData->LODResources[0], BoundingBoxAxisParams, ReferenceTransform, CollectiveBoundsPtr);
		NewMask.SelectionMeshCount = SelectedMeshes.Num();

		if (NewMask.State == EVertexMaskForgeScalarMaskState::Ready)
		{
			Entry->WorkingMesh.BoundingBoxMask = MoveTemp(NewMask);
		}
		else
		{
			// Keep whatever mask this entry already had (Ready or NotGenerated) -- an auto-triggered
			// regeneration must never replace a valid Preview with incomplete/degenerate data.
			++NumFailed;
			if (FirstFailedAssetName.IsEmpty())
			{
				FirstFailedAssetName = Entry->AssetName;
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
	// LastOperationErrorText (see the comment at the top of this function).
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
		if (Entry->WorkingMesh.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready)
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

void SVertexMaskForgePanel::RecomputeOperationState()
{
	if (OperationState == EVertexMaskForgeOperationState::Applying)
	{
		return;
	}

	bool bHasPending = false;
	if (CurrentPreviewMode != EVertexMaskForgePreviewMode::OriginalMaterial)
	{
		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (Entry.IsValid() && !Entry->PreviewComponents.IsEmpty()
				&& Entry->WorkingMesh.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready)
			{
				bHasPending = true;
				break;
			}
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
		return LOCTEXT("OperationStatePending", "Pending Changes: Accept writes Vertex Colors to the Source Static Mesh asset (affects every instance); Accept as Instance Override writes only to the selected component(s); Cancel discards.");
	case EVertexMaskForgeOperationState::Applying:
		return LOCTEXT("OperationStateApplying", "Applying...");
	case EVertexMaskForgeOperationState::Failed:
		return LOCTEXT("OperationStateFailed", "Accept failed (see message above).");
	case EVertexMaskForgeOperationState::Idle:
	default:
		if (!LastRemoveOverrideStatusText.IsEmpty())
		{
			return LastRemoveOverrideStatusText;
		}
		if (!LastInstanceOverrideStatusText.IsEmpty())
		{
			return LastInstanceOverrideStatusText;
		}
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
	LastInstanceOverrideStatusText = FText::GetEmpty();
	LastRemoveOverrideStatusText = FText::GetEmpty();

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
	LastInstanceOverrideStatusText = FText::GetEmpty();
	LastRemoveOverrideStatusText = FText::GetEmpty();

	TArray<VertexMaskForgePanel::FVertexMaskForgeAcceptTarget> Targets;
	FText ErrorText;
	if (!VertexMaskForgePanel::BuildAcceptTargets(
		SelectedMeshes, CurrentPreviewMode, bChannelFilterR, bChannelFilterG, bChannelFilterB, bChannelFilterA,
		bUseUnifiedBounds, BoundingBoxAxisParams,
		Targets, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Warning, TEXT("Vertex Mask Forge: Accept blocked: %s"), *ErrorText.ToString());
		return false;
	}

	// Confirm the (permanent, all-instances-affected) destination before the first write. Not shown
	// for every minor adjustment -- only here, at the point of an actually destructive/permanent
	// operation.
	TArray<FString> AssetNames;
	AssetNames.Reserve(Targets.Num());
	for (const VertexMaskForgePanel::FVertexMaskForgeAcceptTarget& Target : Targets)
	{
		AssetNames.Add(Target.AssetName);
	}
	const EAppReturnType::Type Choice = FMessageDialog::Open(
		EAppMsgType::OkCancel,
		FText::Format(
			LOCTEXT("AcceptConfirmFormat",
				"This will permanently write Vertex Colors into {0} Static Mesh Asset(s):\n\n{1}\n\n"
				"This affects EVERY instance/placement of these assets in every level, not just the "
				"currently selected one(s). This can be undone with Editor Undo.\n\nProceed?"),
			FText::AsNumber(Targets.Num()),
			FText::FromString(FString::Join(AssetNames, TEXT("\n")))));
	if (Choice != EAppReturnType::Ok)
	{
		// User declined at the confirmation step -- Preview and state are untouched, not a failure.
		return false;
	}

	OperationState = EVertexMaskForgeOperationState::Applying;

	if (!VertexMaskForgePanel::WriteAcceptTargets(Targets, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept failed while writing: %s"), *ErrorText.ToString());
		return false;
	}

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Accepted Vertex Color changes for %d Static Mesh asset(s)."), Targets.Num());

	// Success: destroy the transient Preview (its job is done -- the colors now live permanently on
	// the asset) and return to Idle.
	DestroyAllPreviews();
	OperationState = EVertexMaskForgeOperationState::Idle;
	LastOperationErrorText = FText::GetEmpty();

	// Deferred sync: the write above already completed against the ORIGINAL SelectedMeshes/Targets
	// captured before this call; only now, with OperationState settled back to Idle, is it safe to
	// catch up with a scene selection that may have changed while this operation was pending.
	SyncSelectionIfChangedDuringOperation();

	return true;
}

FReply SVertexMaskForgePanel::OnAcceptAsInstanceOverrideClicked()
{
	AcceptPendingChangesAsInstanceOverride();
	return FReply::Handled();
}

bool SVertexMaskForgePanel::AcceptPendingChangesAsInstanceOverride()
{
	if (OperationState != EVertexMaskForgeOperationState::PendingChanges)
	{
		return false;
	}

	LastOperationErrorText = FText::GetEmpty();
	LastMaskActionStatusText = FText::GetEmpty();
	LastInstanceOverrideStatusText = FText::GetEmpty();
	LastRemoveOverrideStatusText = FText::GetEmpty();

	TArray<VertexMaskForgePanel::FVertexMaskForgeInstanceOverrideTarget> Targets;
	FText ErrorText;
	if (!VertexMaskForgePanel::BuildInstanceOverrideTargets(
		SelectedMeshes, CurrentPreviewMode, bChannelFilterR, bChannelFilterG, bChannelFilterB, bChannelFilterA,
		bUseUnifiedBounds, BoundingBoxAxisParams,
		Targets, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Warning, TEXT("Vertex Mask Forge: Accept as Instance Override blocked: %s"), *ErrorText.ToString());
		return false;
	}

	// Confirm the (permanent, per-instance, asset-safe) destination before the first write -- same
	// "only at the point of an actually permanent operation" rule as native Accept's confirmation.
	const EAppReturnType::Type Choice = FMessageDialog::Open(
		EAppMsgType::OkCancel,
		FText::Format(
			LOCTEXT("InstanceOverrideConfirmFormat",
				"Vertex Colors will be stored as per-instance overrides on {0} selected component(s). "
				"The Source Static Mesh assets will not be modified. The result persists when the level "
				"is saved.\n\nProceed?"),
			FText::AsNumber(Targets.Num())));
	if (Choice != EAppReturnType::Ok)
	{
		// User declined at the confirmation step -- Preview and state are untouched, not a failure.
		return false;
	}

	OperationState = EVertexMaskForgeOperationState::Applying;

	if (!VertexMaskForgePanel::WriteInstanceOverrideTargets(Targets, ErrorText))
	{
		OperationState = EVertexMaskForgeOperationState::Failed;
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept as Instance Override failed while writing: %s"), *ErrorText.ToString());
		return false;
	}

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Accepted Vertex Color changes as instance overrides on %d component(s). Source Static Mesh assets were not modified."),
		Targets.Num());

	// Success: destroy the transient Preview (its job is done -- the colors now live permanently on
	// the real component(s)) and return to Idle, exactly like native Accept, but WITHOUT ever calling
	// into AcceptPendingChanges()/WriteAcceptTargets() -- this path never touches the Static Mesh asset.
	DestroyAllPreviews();
	OperationState = EVertexMaskForgeOperationState::Idle;
	LastOperationErrorText = FText::GetEmpty();
	LastInstanceOverrideStatusText = FText::Format(
		LOCTEXT("InstanceOverrideSuccessFormat", "Vertex Colors saved as instance overrides on {0} component(s). Source Static Mesh assets were not modified."),
		FText::AsNumber(Targets.Num()));

	// Deferred sync: the write above already completed against the ORIGINAL SelectedMeshes/Targets
	// captured before this call; only now, with OperationState settled back to Idle, is it safe to
	// catch up with a scene selection that may have changed while this operation was pending.
	SyncSelectionIfChangedDuringOperation();

	return true;
}

bool SVertexMaskForgePanel::CanRemoveInstanceOverride() const
{
	if (OperationState != EVertexMaskForgeOperationState::Idle)
	{
		// An unresolved PendingChanges/Failed Preview decision must be concluded via Accept, Accept
		// as Instance Override, or Cancel first -- this never discards a pending Preview itself.
		return false;
	}

	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
		{
			if (VertexMaskForgePanel::HasRemovableLOD0Override(State.SourceComponent.Get()))
			{
				return true;
			}
		}
	}

	return false;
}

FReply SVertexMaskForgePanel::OnRemoveInstanceOverrideClicked()
{
	RemoveInstanceOverrides();
	return FReply::Handled();
}

bool SVertexMaskForgePanel::RemoveInstanceOverrides()
{
	if (!CanRemoveInstanceOverride())
	{
		return false;
	}

	LastOperationErrorText = FText::GetEmpty();
	LastMaskActionStatusText = FText::GetEmpty();
	LastInstanceOverrideStatusText = FText::GetEmpty();
	LastRemoveOverrideStatusText = FText::GetEmpty();

	TArray<VertexMaskForgePanel::FVertexMaskForgeRemoveOverrideTarget> Targets;
	FText ErrorText;
	if (!VertexMaskForgePanel::BuildRemoveInstanceOverrideTargets(SelectedMeshes, Targets, ErrorText))
	{
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Warning, TEXT("Vertex Mask Forge: Remove Instance Override blocked: %s"), *ErrorText.ToString());
		return false;
	}

	// Confirm before the first write -- same "only at the point of an actually permanent operation"
	// rule as Accept / Accept as Instance Override's own confirmations.
	const EAppReturnType::Type Choice = FMessageDialog::Open(
		EAppMsgType::OkCancel,
		FText::Format(
			LOCTEXT("RemoveOverrideConfirmFormat",
				"Vertex Color overrides will be removed from {0} selected component(s). These "
				"components will return to the Vertex Colors stored in their Source Static Mesh. "
				"The Source Static Mesh assets will not be modified.\n\nProceed?"),
			FText::AsNumber(Targets.Num())));
	if (Choice != EAppReturnType::Ok)
	{
		// User declined at the confirmation step -- nothing was modified, not a failure.
		return false;
	}

	if (!VertexMaskForgePanel::RemoveInstanceOverrideTargets(Targets, ErrorText))
	{
		LastOperationErrorText = ErrorText;
		UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Remove Instance Override failed while writing: %s"), *ErrorText.ToString());
		return false;
	}

	UE_LOG(LogVertexMaskForge, Log,
		TEXT("Vertex Mask Forge: Removed Instance Vertex Color overrides from %d component(s). Source Static Mesh assets were not modified."),
		Targets.Num());

	// Success: this operation never involves a Preview or PendingChanges session (CanRemoveInstanceOverride
	// only allows it while already Idle), so there is no OperationState transition and no
	// DestroyAllPreviews() call here -- the original selection and OperationState are left exactly as
	// they were.
	LastRemoveOverrideStatusText = FText::Format(
		LOCTEXT("RemoveOverrideSuccessFormat", "Instance Vertex Color overrides removed from {0} component(s). Source Static Mesh assets were not modified."),
		FText::AsNumber(Targets.Num()));

	return true;
}

void SVertexMaskForgePanel::RestorePreviewForEntry(FVertexMaskForgeSelectedMesh& Entry)
{
	for (FVertexMaskForgePreviewComponentState& State : Entry.PreviewComponents)
	{
		VertexMaskForgePanel::RestoreComponentOriginal(State, ActorHideStates);
	}
}

void SVertexMaskForgePanel::ApplyPreviewToEntry(
	const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry,
	const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr)
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
		|| WorkingMesh.BoundingBoxMask.State != EVertexMaskForgeScalarMaskState::Ready)
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

		// AUDITED (World Space checkpoint): the entry-level WorkingMesh.BoundingBoxMask is only a
		// REFERENCE, evaluated with one representative transform. When Source == BoundingBox, the
		// mask actually used for THIS component's Preview is re-evaluated fresh with THIS
		// component's own ComponentTransform, so World Space axes correctly vary per instance (see
		// the audit note on FVertexMaskForgeWorkingMesh::BoundingBoxMask). Constant Fill sources are
		// transform-independent, so the shared reference is reused directly without recomputation.
		FVertexMaskForgeScalarMask PerComponentMask;
		const FVertexMaskForgeScalarMask* EffectiveMask = &WorkingMesh.BoundingBoxMask;
		if (WorkingMesh.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::BoundingBox)
		{
			PerComponentMask = VertexMaskForgePanel::GenerateBoundingBoxMask(
				RenderData->LODResources[0], WorkingMesh.BoundingBoxMask.UsedAxisParams, SourceComponent->GetComponentTransform(),
				CollectiveBoundsPtr);
			if (PerComponentMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// This specific instance's World Space evaluation came back degenerate/invalid even
				// though the entry-level reference was Ready -- fall back to this component's
				// original appearance rather than showing stale or fabricated data.
				VertexMaskForgePanel::RestoreComponentOriginal(State, ActorHideStates);
				continue;
			}
			EffectiveMask = &PerComponentMask;
		}

		// Read-only: this buffer belongs to SourceComponent and is never modified by the plugin.
		const FColorVertexBuffer* InstanceOverrideColors =
			SourceComponent->LODData.IsValidIndex(0) ? SourceComponent->LODData[0].OverrideVertexColors : nullptr;

		// TEMPORARY diagnostic (audited render-vertex-order fix): NumComposed vs the LOD's render
		// vertex count directly proves the 1:1 correspondence in the log, per-component, every time
		// the preview is (re)applied.
		int32 NumComposed = 0;
		const TArray<FColor> RenderOrderColors = VertexMaskForgePanel::ComposeRenderOrderPreviewColors(
			*EffectiveMask, RenderData->LODResources[0], InstanceOverrideColors,
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
					RestorePreviewForEntry(*Entry);
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

		ApplyPreviewToEntry(Entry, CollectiveBoundsPtr);

		if (Entry->PreviewComponents.IsEmpty())
		{
			continue; // Content-Browser-only; not counted in the viewport preview tally.
		}

		switch (Entry->WorkingMesh.BoundingBoxMask.State)
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
