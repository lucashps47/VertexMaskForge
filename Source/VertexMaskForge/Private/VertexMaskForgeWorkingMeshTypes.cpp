#include "VertexMaskForgeWorkingMeshTypes.h"

#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

// FDynamicMesh3 is a complete type in the header now (M6), but these special member functions stay
// defined out-of-line here, unchanged from before M6.
FVertexMaskForgeWorkingMesh::~FVertexMaskForgeWorkingMesh() = default;
FVertexMaskForgeWorkingMesh::FVertexMaskForgeWorkingMesh(FVertexMaskForgeWorkingMesh&&) = default;
FVertexMaskForgeWorkingMesh& FVertexMaskForgeWorkingMesh::operator=(FVertexMaskForgeWorkingMesh&&) = default;

// Same reasoning as above -- kept out-of-line, unchanged from before M6.
FVertexMaskForgePreviewComponentState::~FVertexMaskForgePreviewComponentState() = default;
FVertexMaskForgePreviewComponentState::FVertexMaskForgePreviewComponentState(FVertexMaskForgePreviewComponentState&&) = default;
FVertexMaskForgePreviewComponentState& FVertexMaskForgePreviewComponentState::operator=(FVertexMaskForgePreviewComponentState&&) = default;

namespace VertexMaskForgeWorkingMeshTypes
{
	/**
	 * AUDITED (V2-E corrective pass): FMatrix::Inverse() on a singular matrix produces NaN/Inf, which
	 * must never reach TransformVector.
	 */
	bool ComputeWorldSpaceNormalMatrix(const FTransform& ComponentTransform, FMatrix& OutNormalMatrix)
	{
		constexpr float ScaleEpsilon = 1e-6f;
		const FVector Scale = ComponentTransform.GetScale3D();
		if (FMath::Abs(Scale.X) < ScaleEpsilon || FMath::Abs(Scale.Y) < ScaleEpsilon || FMath::Abs(Scale.Z) < ScaleEpsilon)
		{
			return false;
		}
		const FMatrix Candidate = ComponentTransform.ToMatrixWithScale().Inverse().GetTransposed();
		// AUDITED (V2-E corrective pass): FMatrix::Inverse() on a near-singular-in-practice matrix
		// (e.g. a pathological rotation quaternion) can still produce NaN/Inf even after the scale-
		// magnitude guard above -- checked explicitly, never trusted implicitly.
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				if (!FMath::IsFinite(Candidate.M[Row][Col]))
				{
					return false;
				}
			}
		}
		OutNormalMatrix = Candidate;
		return true;
	}

	namespace
	{
		/**
		 * AUDITED (V2-E CORRECTIVE PASS -- root cause of the original bug): TransformNormal's own contract
		 * is `Rotate(Normalize(InverseScale * Normal))` (see UE::Geometry::TTransformSRT3::TransformNormal's
		 * own doc comment, confirmed against the GeometryCore source during the original V2-E audit) -- the
		 * per-normal Normalize() step means TWO normal matrices A and B produce IDENTICAL results for EVERY
		 * possible input normal if and only if B = k*A for some SINGLE POSITIVE SCALAR k (normalize(k*A*N) ==
		 * normalize(A*N) for any k>0, since scaling a vector never changes its direction; conversely, if B is
		 * NOT a positive scalar multiple of A, there EXISTS at least one input N for which normalize(A*N) !=
		 * normalize(B*N) -- the original bug's own counter-example, a diagonal normal under Identity vs.
		 * Scale(2,1,1), is exactly one such N). Comparing only 3 independently-normalized canonical axis
		 * vectors (the ORIGINAL, insufficient implementation) missed this: non-uniform scale can leave EVERY
		 * individual axis vector's OWN direction unchanged after its own normalization, while still producing
		 * a matrix that is NOT a scalar multiple of the reference -- so a diagonal (or any non-axis-aligned)
		 * normal still disagrees. Testing full matrix proportionality is therefore not just "more thorough"
		 * than testing 3 axes -- it is the exact necessary-and-sufficient condition, mathematically equivalent
		 * to (and far cheaper than) comparing the transformed-and-normalized result for EVERY possible corner/
		 * render normal one by one (Option B in the corrective brief), since the matrix multiply is linear and
		 * the normalize step's scale-invariance is exact, not approximate.
		 *
		 * AUDITED (M6): private to this translation unit -- single caller (HasConflictingWorldSpaceNormalTransforms
		 * below), never called from generation code, so it does not need the shared-implementation treatment
		 * ComputeWorldSpaceNormalMatrix required.
		 */
		bool AreNormalMatricesEquivalent(const FMatrix& A, const FMatrix& B, float& OutMaxRelativeDeviation)
		{
			OutMaxRelativeDeviation = 0.0f;

			// Find A's largest-magnitude element to derive the candidate scalar k robustly (dividing by a
			// near-zero element would amplify floating-point noise into a meaningless k).
			double MaxAbsA = 0.0;
			double K = 0.0;
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Col = 0; Col < 3; ++Col)
				{
					const double AElem = A.M[Row][Col];
					if (FMath::Abs(AElem) > MaxAbsA)
					{
						MaxAbsA = FMath::Abs(AElem);
						K = B.M[Row][Col] / AElem;
					}
				}
			}

			// A degenerate (all-zero) normal matrix should never reach here (ComputeWorldSpaceNormalMatrix
			// already rejects degenerate transforms before this is ever called) -- treated defensively as a
			// non-match rather than asserting.
			if (MaxAbsA < 1e-9)
			{
				return false;
			}
			// K <= 0 means B is either a non-scalar-multiple of A or a NEGATIVE multiple -- a negative k would
			// flip every transformed normal to point the opposite way (a genuinely different, not equivalent,
			// result) -- never treated as a match. This is NOT a "negative/mirrored scale isn't supported"
			// limitation: a mirrored component's OWN normal matrix already encodes its mirroring internally
			// (via ComputeWorldSpaceNormalMatrix's inverse-transpose, whose sign naturally flips for a
			// negative-determinant transform) -- two SIMILARLY-mirrored components still compare as
			// equivalent here (B = +k*A), and only a genuine orientation DISAGREEMENT (K<=0, or K>0 but the
			// full-matrix check below fails) is ever flagged as a conflict.
			if (K <= 0.0)
			{
				OutMaxRelativeDeviation = 1.0f; // Maximal disagreement -- orientation itself disagrees.
				return false;
			}

			// Verify B == K*A across all 9 elements, relative to A's own largest magnitude (scaled by K) so
			// the tolerance is meaningful regardless of the transform's absolute magnitude.
			double MaxAbsDeviation = 0.0;
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Col = 0; Col < 3; ++Col)
				{
					const double Expected = K * A.M[Row][Col];
					const double Actual = B.M[Row][Col];
					MaxAbsDeviation = FMath::Max(MaxAbsDeviation, FMath::Abs(Actual - Expected));
				}
			}
			const double ReferenceScale = FMath::Max(MaxAbsA * K, 1e-9);
			OutMaxRelativeDeviation = static_cast<float>(MaxAbsDeviation / ReferenceScale);

			constexpr float RelativeToleranceForEquivalence = 1e-3f;
			return OutMaxRelativeDeviation <= RelativeToleranceForEquivalence;
		}
	}

	/**
	 * AUDITED (V2-E CORRECTIVE PASS): compares EVERY live component's own World-Space normal MATRIX (see
	 * ComputeWorldSpaceNormalMatrix) against a single reference matrix (the first valid one found) using
	 * full-matrix proportionality (see AreNormalMatricesEquivalent's own doc comment for why this is the
	 * mathematically necessary-and-sufficient test -- NOT the previous, insufficient 3-axis-vector
	 * comparison). Translation is irrelevant by construction (ComputeWorldSpaceNormalMatrix only ever
	 * reads the transform's 3x3 linear part). Uniform-scale differences (e.g. Scale 1 vs. Scale 2,
	 * otherwise identical rotation) ARE equivalent (their normal matrices are exact positive scalar
	 * multiples of each other). Non-uniform scale differences are NOT silently accepted (their matrices
	 * are provably non-proportional whenever they would actually change a transformed normal's
	 * direction). Different rotations are NOT equivalent (a pure rotation is orthogonal, never a scalar
	 * multiple of a different rotation, except the identity case). A component with a degenerate
	 * transform is skipped here (never treated as a conflict on its own -- see this entry's separate
	 * DirectionalNormalMask.State==Invalid diagnostic) but every OTHER pair is still compared. Never
	 * picks a "winning" instance -- returns true (conflict) the moment ANY live component disagrees with
	 * the reference beyond tolerance.
	 */
	bool HasConflictingWorldSpaceNormalTransforms(const TArray<FVertexMaskForgePreviewComponentState>& PreviewComponents, float& OutMaxRelativeDeviation)
	{
		OutMaxRelativeDeviation = 0.0f;

		FMatrix ReferenceMatrix = FMatrix::Identity;
		bool bHaveReference = false;
		bool bAnyConflict = false;

		for (const FVertexMaskForgePreviewComponentState& State : PreviewComponents)
		{
			const UStaticMeshComponent* Component = State.SourceComponent.Get();
			if (!IsValid(Component))
			{
				continue;
			}
			FMatrix NormalMatrix;
			if (!ComputeWorldSpaceNormalMatrix(Component->GetComponentTransform(), NormalMatrix))
			{
				continue; // A degenerate individual transform is its own separate diagnostic, not a conflict here.
			}

			if (!bHaveReference)
			{
				ReferenceMatrix = NormalMatrix;
				bHaveReference = true;
				continue;
			}

			float ThisDeviation = 0.0f;
			if (!AreNormalMatricesEquivalent(ReferenceMatrix, NormalMatrix, ThisDeviation))
			{
				bAnyConflict = true;
			}
			OutMaxRelativeDeviation = FMath::Max(OutMaxRelativeDeviation, ThisDeviation);
		}

		return bAnyConflict;
	}

	/**
	 * Scale-aware degenerate triangle test (V2-G corrective audit) -- RELATIVE test on the sine of the
	 * angle between the two edges from P0 (CrossSq = |E0|^2|E1|^2*sin^2(theta), so CrossSq <=
	 * k^2*E0Sq*E1Sq reduces to sin(theta) <= k -- scale-invariant by construction, unlike an absolute
	 * area tolerance which would wrongly accept a huge near-collinear triangle and wrongly reject a tiny
	 * valid one). A separate ABSOLUTE floor on edge length catches the genuinely-zero-length-edge case,
	 * which the relative test alone cannot distinguish from a valid tiny angle.
	 */
	bool IsThicknessTriangleDegenerate(const FVector3d& P0, const FVector3d& P1, const FVector3d& P2)
	{
		constexpr double EdgeLengthAbsoluteFloorSq = 1e-10;   // (1e-5 local units)^2
		constexpr double RelativeAreaToleranceSq = 1e-8;      // sin(theta) <= 1e-4

		const FVector3d E0 = P1 - P0;
		const FVector3d E1 = P2 - P0;
		const double E0Sq = E0.SquaredLength();
		const double E1Sq = E1.SquaredLength();
		const FVector3d Cross = FVector3d::CrossProduct(E0, E1);
		const double CrossSq = Cross.SquaredLength();

		return !FMath::IsFinite(E0Sq) || !FMath::IsFinite(E1Sq) || Cross.ContainsNaN()
			|| E0Sq <= EdgeLengthAbsoluteFloorSq || E1Sq <= EdgeLengthAbsoluteFloorSq
			|| CrossSq <= RelativeAreaToleranceSq * E0Sq * E1Sq;
	}

	/**
	 * AUDITED (V2-G, Thickness freshness): the fingerprint/count checks already performed elsewhere
	 * (WedgeMap counts, ValidateSourceTopologyCorrespondence, GeometryFingerprint) prove DOMAIN/
	 * STRUCTURAL correspondence, never that POSITIONS or NORMALS are unchanged -- a reimport/edit
	 * that preserves every count and ID would slip through all of them silently. Since Thickness's
	 * measured distance is a direct function of position+normal+connectivity, this function performs the
	 * FULL semantic comparison the corrective audit closed on: value-by-value, keyed by RenderVertexIndex
	 * (never by any Dynamic-Mesh-internal VertexID/NormalElementID, which are allocation artifacts, not
	 * source content). Called ONLY as a second gate, AFTER Cache.CachedGeometryFingerprint (a uint32
	 * fast-reject) already matched CurrentFingerprint -- a fingerprint match NEVER by itself proves
	 * freshness, this comparison is always still required; see WriteAcceptTargets' own call site.
	 */
	bool AreThicknessGeometrySnapshotsExactlyEquivalent(
		const FVertexMaskForgeThicknessCache& Cache,
		const FStaticMeshLODResources& CurrentLOD0)
	{
		const FPositionVertexBuffer& CurrentPositions = CurrentLOD0.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& CurrentTangents = CurrentLOD0.VertexBuffers.StaticMeshVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(CurrentPositions.GetNumVertices());

		if (NumRenderVerts != Cache.SnapshotPositions.Num() || NumRenderVerts != Cache.SnapshotTangentZ.Num()
			|| static_cast<int32>(CurrentTangents.GetNumVertices()) != NumRenderVerts)
		{
			return false;
		}

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const FVector3f CurPos = CurrentPositions.VertexPosition(i);
			const FVector3f& OldPos = Cache.SnapshotPositions[i];
			if (CurPos.ContainsNaN() || OldPos.ContainsNaN() || CurPos != OldPos)
			{
				return false;   // NaN/Inf is always a mismatch -- never compared as "equal" to anything
			}

			const FVector4f CurTangent4 = CurrentTangents.VertexTangentZ(i);
			const FVector3f CurTangent(CurTangent4.X, CurTangent4.Y, CurTangent4.Z);
			const FVector3f& OldTangent = Cache.SnapshotTangentZ[i];
			if (CurTangent.ContainsNaN() || OldTangent.ContainsNaN() || CurTangent != OldTangent)
			{
				return false;   // catches a tangent-Z-only edit even with positions/counts unchanged
			}
		}

		// Connectivity: re-derive the SAME filtered (degenerate-excluded) triangle sequence the cache's
		// own LocalMesh bake produces, by render-vertex-index -- never by any internal TriangleID, so a
		// reorder that preserves counts but changes WHICH vertices form a triangle is still detected.
		TArray<FIntVector> CurrentTriangles;
		const int32 NumIndices = CurrentLOD0.IndexBuffer.GetNumIndices();
		CurrentTriangles.Reserve(NumIndices / 3);
		for (int32 TriIndex = 0; TriIndex < NumIndices / 3; ++TriIndex)
		{
			const int32 I0 = static_cast<int32>(CurrentLOD0.IndexBuffer.GetIndex(TriIndex * 3 + 0));
			const int32 I1 = static_cast<int32>(CurrentLOD0.IndexBuffer.GetIndex(TriIndex * 3 + 1));
			const int32 I2 = static_cast<int32>(CurrentLOD0.IndexBuffer.GetIndex(TriIndex * 3 + 2));
			if (I0 == I1 || I1 == I2 || I0 == I2)
			{
				continue;
			}
			if (!CurrentPositions.GetNumVertices() || I0 >= NumRenderVerts || I1 >= NumRenderVerts || I2 >= NumRenderVerts || I0 < 0 || I1 < 0 || I2 < 0)
			{
				return false;
			}
			if (IsThicknessTriangleDegenerate(FVector3d(CurrentPositions.VertexPosition(I0)), FVector3d(CurrentPositions.VertexPosition(I1)), FVector3d(CurrentPositions.VertexPosition(I2))))
			{
				continue;
			}
			CurrentTriangles.Add(FIntVector(I0, I1, I2));
		}

		if (CurrentTriangles.Num() != Cache.SnapshotTriangles.Num())
		{
			return false;
		}
		for (int32 i = 0; i < CurrentTriangles.Num(); ++i)
		{
			if (CurrentTriangles[i] != Cache.SnapshotTriangles[i])
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Source-Topology sibling of AreThicknessGeometrySnapshotsExactlyEquivalent -- reuses TriIDMap (the
	 * SAME source-stable Dynamic-TriangleID -> FTriangleID correspondence WriteSourceTopologyAcceptTargets
	 * already relies on) to compare, per corner, the CURRENT MeshDescription's position/normal against
	 * the value used when Thickness was generated (read from WorkingMesh.Mesh/its NormalOverlay --
	 * comparing WITHIN that single persistent object is always self-consistent, so no cross-run Dynamic-
	 * Mesh-internal ID comparison is ever needed). Never reconverts MeshDescription->FDynamicMesh3.
	 */
	bool IsThicknessSourceTopologyContentUnchanged(
		const UE::Geometry::FDynamicMesh3& OldMesh,
		const TArray<FTriangleID>& TriIDMap,
		const FMeshDescription& CurrentMeshDescription)
	{
		using namespace UE::Geometry;

		const FDynamicMeshNormalOverlay* OldNormalOverlay =
			(OldMesh.HasAttributes() && OldMesh.Attributes()->PrimaryNormals() != nullptr) ? OldMesh.Attributes()->PrimaryNormals() : nullptr;
		if (!OldNormalOverlay)
		{
			return false;
		}

		FStaticMeshConstAttributes Attributes(CurrentMeshDescription);
		TVertexAttributesConstRef<FVector3f> CurrentPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesConstRef<FVector3f> CurrentNormals = Attributes.GetVertexInstanceNormals();

		for (const int32 TriangleID : OldMesh.TriangleIndicesItr())
		{
			if (!TriIDMap.IsValidIndex(TriangleID))
			{
				return false;
			}
			const FTriangleID SourceTriangleID = TriIDMap[TriangleID];
			if (!CurrentMeshDescription.IsTriangleValid(SourceTriangleID))
			{
				return false;
			}
			const TArrayView<const FVertexInstanceID> SourceInstances = CurrentMeshDescription.GetTriangleVertexInstances(SourceTriangleID);
			if (SourceInstances.Num() != 3)
			{
				return false;
			}

			const FIndex3i OldVertTri = OldMesh.GetTriangle(TriangleID);
			const FIndex3i OldNormalTri = OldNormalOverlay->IsSetTriangle(TriangleID) ? OldNormalOverlay->GetTriangle(TriangleID) : FIndex3i(INDEX_NONE, INDEX_NONE, INDEX_NONE);

			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVertexInstanceID CurInstanceID = SourceInstances[Corner];
				if (!CurrentMeshDescription.IsVertexInstanceValid(CurInstanceID))
				{
					return false;
				}
				const FVertexID CurVertexID = CurrentMeshDescription.GetVertexInstanceVertex(CurInstanceID);

				const FVector3f CurPos = CurrentPositions[CurVertexID];
				const FVector3d OldPosD = OldMesh.GetVertex(OldVertTri[Corner]);
				if (CurPos.ContainsNaN() || !FMath::IsFinite(OldPosD.X) || !FMath::IsFinite(OldPosD.Y) || !FMath::IsFinite(OldPosD.Z) || FVector3f(OldPosD) != CurPos)
				{
					return false;
				}

				const FVector3f CurNormal = CurrentNormals[CurInstanceID];
				const int32 OldElementID = OldNormalTri[Corner];
				if (OldElementID == INDEX_NONE || !OldNormalOverlay->IsElement(OldElementID))
				{
					return false;
				}
				const FVector3f OldNormal = OldNormalOverlay->GetElement(OldElementID);
				if (CurNormal.ContainsNaN() || OldNormal.ContainsNaN() || CurNormal != OldNormal)
				{
					return false;
				}
			}
		}
		return true;
	}
}
