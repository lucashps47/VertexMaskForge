#include "VertexMaskForgeGeneratorUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "StaticMeshResources.h"

namespace VertexMaskForgeGeneratorUtils
{
	uint32 ComputeDynamicMeshGeometryFingerprint(const UE::Geometry::FDynamicMesh3& Mesh)
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

	TArray<float> ApplyAdjacencyTopologicalBlur(const TArray<TArray<int32>>& Adjacency, const TArray<float>& Input, const TArray<bool>& bHasValue, const float BlurAmount)
	{
		if (BlurAmount <= 0.0f || Input.IsEmpty())
		{
			return Input;
		}

		const int32 FullIterations = FMath::FloorToInt32(BlurAmount);
		const float FractionalIteration = BlurAmount - static_cast<float>(FullIterations);

		// Never lets an unwritten (degenerate-normal) element bleed into or receive a blurred value --
		// an element with no raw value stays exactly as unwritten as it started (never guessed), and it
		// never contributes to a neighbor's average either (there is nothing valid to contribute).
		auto RunOneIteration = [&Adjacency, &bHasValue](const TArray<float>& Src) -> TArray<float>
		{
			TArray<float> Dst = Src;
			for (int32 i = 0; i < Src.Num(); ++i)
			{
				if (!bHasValue.IsValidIndex(i) || !bHasValue[i])
				{
					continue;
				}
				float Sum = Src[i];
				int32 Count = 1;
				if (Adjacency.IsValidIndex(i))
				{
					for (const int32 NeighborIndex : Adjacency[i])
					{
						if (Src.IsValidIndex(NeighborIndex) && bHasValue.IsValidIndex(NeighborIndex) && bHasValue[NeighborIndex])
						{
							Sum += Src[NeighborIndex];
							++Count;
						}
					}
				}
				Dst[i] = Sum / static_cast<float>(Count);
			}
			return Dst;
		};

		TArray<float> Current = Input;
		for (int32 Iter = 0; Iter < FullIterations; ++Iter)
		{
			Current = RunOneIteration(Current);
		}

		if (FractionalIteration > UE_KINDA_SMALL_NUMBER)
		{
			TArray<float> OneMore = RunOneIteration(Current);
			for (int32 i = 0; i < Current.Num(); ++i)
			{
				if (bHasValue.IsValidIndex(i) && bHasValue[i])
				{
					Current[i] = FMath::Lerp(Current[i], OneMore[i], FractionalIteration);
				}
			}
		}

		return Current;
	}

	TArray<TArray<int32>> BuildRenderVertexAdjacency(const FStaticMeshLODResources& LOD0, const int32 NumRenderVerts)
	{
		TArray<TArray<int32>> Adjacency;
		Adjacency.SetNum(NumRenderVerts);

		const FRawStaticIndexBuffer& IndexBuffer = LOD0.IndexBuffer;
		const int32 NumIndices = IndexBuffer.GetNumIndices();
		for (int32 TriStart = 0; TriStart + 2 < NumIndices; TriStart += 3)
		{
			const int32 I0 = static_cast<int32>(IndexBuffer.GetIndex(TriStart + 0));
			const int32 I1 = static_cast<int32>(IndexBuffer.GetIndex(TriStart + 1));
			const int32 I2 = static_cast<int32>(IndexBuffer.GetIndex(TriStart + 2));
			if (!Adjacency.IsValidIndex(I0) || !Adjacency.IsValidIndex(I1) || !Adjacency.IsValidIndex(I2))
			{
				continue;
			}
			Adjacency[I0].AddUnique(I1); Adjacency[I0].AddUnique(I2);
			Adjacency[I1].AddUnique(I0); Adjacency[I1].AddUnique(I2);
			Adjacency[I2].AddUnique(I0); Adjacency[I2].AddUnique(I1);
		}
		return Adjacency;
	}

	TArray<TArray<int32>> BuildCornerAdjacency(const UE::Geometry::FDynamicMesh3& Mesh, const UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay, const int32 NumCorners)
	{
		using namespace UE::Geometry;

		TArray<TArray<int32>> Adjacency;
		Adjacency.SetNum(NumCorners);

		TMap<int32, int32> TriangleIDToCornerBase;
		TriangleIDToCornerBase.Reserve(Mesh.TriangleCount());
		{
			int32 Base = 0;
			for (const int32 TriangleID : Mesh.TriangleIndicesItr())
			{
				TriangleIDToCornerBase.Add(TriangleID, Base);
				Base += 3;
			}
		}

		int32 CornerBase = 0;
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			// Within a single triangle, all 3 corners are always the same continuous surface --
			// unconditional, no overlay check needed (a face can never be split from itself).
			for (int32 C = 0; C < 3; ++C)
			{
				for (int32 C2 = 0; C2 < 3; ++C2)
				{
					if (C != C2)
					{
						Adjacency[CornerBase + C].Add(CornerBase + C2);
					}
				}
			}

			// AUDITED (V2-F corrective pass): cross-triangle connections must (a) link ONLY the two
			// corners that are the actual shared-edge endpoints -- matched by underlying Mesh VertexID,
			// never by local Corner slot 0/1/2, since winding order is not guaranteed to agree between
			// two triangles on either side of an edge -- and (b) be skipped entirely when
			// NormalOverlay->IsSeamEdge() reports the PrimaryNormals overlay has a split (different
			// Element IDs on either side) at that edge, i.e. an authored hard edge. This is the SAME
			// seam query FDynamicMeshNormalOverlay already exposes and other engine code relies on for
			// this exact purpose -- not a second, hand-rolled continuity check. Deliberately keyed to the
			// NORMAL overlay specifically, not any UV overlay: a UV seam does not imply a normal split
			// (e.g. a cylinder cap seam can still be normal-smooth) and must not interrupt this Blur.
			const FIndex3i TriVertices = Mesh.GetTriangle(TriangleID);
			const FIndex3i NeighborTriangles = Mesh.GetTriNeighbourTris(TriangleID);
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const int32 NeighborTriangleID = NeighborTriangles[Edge];
				if (NeighborTriangleID == INDEX_NONE)
				{
					continue;
				}
				const int32* NeighborBasePtr = TriangleIDToCornerBase.Find(NeighborTriangleID);
				if (!NeighborBasePtr)
				{
					continue;
				}

				const int32 EdgeID = Mesh.GetTriEdge(TriangleID, Edge);
				if (NormalOverlay && NormalOverlay->IsSeamEdge(EdgeID))
				{
					continue; // Hard edge / split normal on the NORMAL overlay -- Blur must not cross it.
				}

				// Edge `Edge` connects local corners `Edge` and `(Edge+1)%3` of this triangle (the same
				// convention GetTriNeighbourTris/GetTriEdge share -- see FDynamicMesh3::FindTriangleEdge).
				const int32 LocalA = Edge;
				const int32 LocalB = (Edge + 1) % 3;
				const int32 VertexA = TriVertices[LocalA];
				const int32 VertexB = TriVertices[LocalB];

				const FIndex3i NeighborVertices = Mesh.GetTriangle(NeighborTriangleID);
				int32 NeighborLocalA = INDEX_NONE, NeighborLocalB = INDEX_NONE;
				for (int32 NC = 0; NC < 3; ++NC)
				{
					if (NeighborVertices[NC] == VertexA) { NeighborLocalA = NC; }
					else if (NeighborVertices[NC] == VertexB) { NeighborLocalB = NC; }
				}
				if (NeighborLocalA == INDEX_NONE || NeighborLocalB == INDEX_NONE)
				{
					continue; // Should not happen for a genuine edge-neighbor, but never guess.
				}

				Adjacency[CornerBase + LocalA].Add(*NeighborBasePtr + NeighborLocalA);
				Adjacency[CornerBase + LocalB].Add(*NeighborBasePtr + NeighborLocalB);
			}

			CornerBase += 3;
		}
		return Adjacency;
	}
}
