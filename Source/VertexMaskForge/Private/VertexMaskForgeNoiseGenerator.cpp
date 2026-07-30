#include "VertexMaskForgeNoiseGenerator.h"

#include "Async/ParallelFor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeGeneratorUtils.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// --- Noise: procedural 3D Perlin/FBM, Local Space (V1) ------------------------------------------

	/**
	 * AUDITED (Noise V1, explicit requirement -- "Seed deve alterar o domínio por meio de um hash
	 * determinístico convertido em um offset 3D"): converts Seed into a 3D domain offset via
	 * GetTypeHash/HashCombine on Seed itself combined with a distinct per-axis salt constant -- the SAME
	 * deterministic-hash idiom already used by ComputeDeterministicScrambleAngle/
	 * ComputeDynamicMeshGeometryFingerprint elsewhere in this file. NEVER FMath::Rand/FRandomStream (no
	 * engine RNG state touched), NEVER a pointer/address, NEVER dependent on iteration/execution order --
	 * a pure function of Seed alone, so the SAME Seed always produces the EXACT same offset, and is safe
	 * to call from any thread (ParallelFor workers included) with no synchronization.
	 */
	FVector ComputeNoiseSeedOffset(const int32 Seed)
	{
		auto AxisOffset = [Seed](const uint32 AxisSalt) -> double
		{
			const uint32 Hash = HashCombine(GetTypeHash(Seed), AxisSalt);
			// Arbitrary-looking but fully deterministic spread, large enough that neighboring seeds
			// land in visibly different regions of Perlin's own periodic (256-cell) domain.
			return (static_cast<double>(Hash) / static_cast<double>(MAX_uint32)) * 1000.0;
		};
		return FVector(AxisOffset(0x9E3779B1u), AxisOffset(0x85EBCA77u), AxisOffset(0xC2B2AE3Du));
	}

	/**
	 * AUDITED (Noise V2-A): SIGNED Fractal Brownian Motion -- a weighted sum of Perlin octaves at
	 * increasing frequency (Frequency *= Lacunarity each step, starting at 1) and decreasing amplitude
	 * (Amplitude *= Roughness each step, starting at 1), normalized by the TOTAL amplitude weight, kept
	 * SIGNED (no [0,1] remap here -- callers decide how to use it: FractalPerlin remaps it directly,
	 * Turbulence uses it BOTH for its warp signals AND its final evaluation). Extracted verbatim from
	 * Noise V1's original FractalPerlin branch so FractalPerlin's own output is bit-for-bit unchanged.
	 * Pure function of its explicit parameters only (no global/member state, no RNG) -- safe for
	 * ParallelFor, deterministic, order-independent. Octaves/Roughness/Lacunarity are assumed
	 * already-clamped by the caller (see ComputeRawNoiseValue's own safe-range clamps).
	 */
	float EvaluateSignedFBM(const FVector& P, const int32 Octaves, const float Roughness, const float Lacunarity)
	{
		double Frequency = 1.0;
		float Amplitude = 1.0f;
		double Sum = 0.0;
		float WeightSum = 0.0f;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			const FVector OctavePosition = P * Frequency;
			Sum += static_cast<double>(FMath::PerlinNoise3D(OctavePosition) * Amplitude);
			WeightSum += Amplitude;
			Frequency *= Lacunarity;
			Amplitude *= Roughness;
		}
		constexpr float WeightEpsilon = 1e-4f;
		return static_cast<float>(Sum / FMath::Max(WeightSum, WeightEpsilon));
	}

	/**
	 * AUDITED (Noise V2-A): Billow -- like EvaluateSignedFBM, but each octave's raw Perlin sample is
	 * folded to 2*abs(Perlin)-1 (still SIGNED, in [-1,1]) before being weighted and accumulated, per the
	 * explicit V2-A formula. Kept signed on return -- ComputeRawNoiseValue applies the shared
	 * signed-to-[0,1] remap, exactly like FractalPerlin. With a single octave this reduces to
	 * 2*abs(Perlin(P))-1, which the shared remap turns into abs(Perlin(P)).
	 */
	float EvaluateBillow(const FVector& P, const int32 Octaves, const float Roughness, const float Lacunarity)
	{
		double Frequency = 1.0;
		float Amplitude = 1.0f;
		double SignedSum = 0.0;
		float AmplitudeSum = 0.0f;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			const FVector OctavePosition = P * Frequency;
			const float PerlinValue = FMath::PerlinNoise3D(OctavePosition);
			const float BillowSigned = 2.0f * FMath::Abs(PerlinValue) - 1.0f;
			SignedSum += static_cast<double>(BillowSigned * Amplitude);
			AmplitudeSum += Amplitude;
			Frequency *= Lacunarity;
			Amplitude *= Roughness;
		}
		constexpr float WeightEpsilon = 1e-4f;
		return static_cast<float>(SignedSum / FMath::Max(AmplitudeSum, WeightEpsilon));
	}

	/**
	 * AUDITED (Noise V2-A): Ridged -- each octave contributes pow(1-abs(Perlin), 2), ALREADY in [0,1]
	 * space (never signed), weighted and accumulated, per the explicit V2-A formula. UNLIKE
	 * EvaluateSignedFBM/EvaluateBillow, the result is returned saturated and ready to use directly as
	 * RawMask -- ComputeRawNoiseValue skips its shared signed-to-[0,1] remap for this type, per the
	 * explicit "Não converter Ridge para signed antes da soma" requirement. With a single octave this
	 * reduces to pow(1-abs(Perlin(P)), 2) exactly.
	 */
	float EvaluateRidged(const FVector& P, const int32 Octaves, const float Roughness, const float Lacunarity)
	{
		double Frequency = 1.0;
		float Amplitude = 1.0f;
		double Sum = 0.0;
		float AmplitudeSum = 0.0f;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			const FVector OctavePosition = P * Frequency;
			const float PerlinValue = FMath::PerlinNoise3D(OctavePosition);
			float Ridge = 1.0f - FMath::Abs(PerlinValue);
			Ridge = Ridge * Ridge;
			Sum += static_cast<double>(Ridge * Amplitude);
			AmplitudeSum += Amplitude;
			Frequency *= Lacunarity;
			Amplitude *= Roughness;
		}
		constexpr float WeightEpsilon = 1e-4f;
		return FMath::Clamp(static_cast<float>(Sum / FMath::Max(AmplitudeSum, WeightEpsilon)), 0.0f, 1.0f);
	}

	/**
	 * AUDITED (Noise V2-A): Turbulence -- domain-warped Fractal Perlin, deliberately distinct from
	 * Billow (never a plain sum of abs(Perlin)). Three SIGNED FBM samples (EvaluateSignedFBM, same
	 * Octaves/Roughness/Lacunarity) are taken at P offset by three FIXED, mutually-separated constant
	 * vectors (never derived from Seed/SeedOffset -- deliberately distinct from it, per the explicit
	 * "Não reutilizar exatamente o mesmo SeedOffset" requirement, so the warp axes are decorrelated both
	 * from each other and from the base seed domain), forming a 3D warp vector; P is displaced by that
	 * warp vector scaled by TurbulenceStrength (noise-space units); a final SIGNED FBM sample at the
	 * warped position is returned (still signed -- ComputeRawNoiseValue applies the shared remap).
	 * TurbulenceStrength == 0 collapses WarpedPosition back to P exactly, so Turbulence reduces to plain
	 * FractalPerlin (EvaluateSignedFBM(P, ...)) bit-for-bit when Strength is 0. All three warp offsets
	 * and the recursive EvaluateSignedFBM calls are pure functions of their explicit inputs -- no global
	 * state, no RNG, safe for ParallelFor, deterministic regardless of execution order.
	 */
	float EvaluateTurbulence(const FVector& P, const int32 Octaves, const float Roughness, const float Lacunarity, const float Strength)
	{
		// Fixed, deliberately-separated constants (never SeedOffset) -- decorrelate the three warp axes
		// from each other and from the base Seed domain. Magnitude chosen well outside Perlin's 256-cell
		// period so each axis samples a visibly different neighborhood of the noise field.
		static const FVector WarpOffsetX(37.234, 91.847, 15.372);
		static const FVector WarpOffsetY(64.129, 12.583, 77.914);
		static const FVector WarpOffsetZ(88.472, 45.607, 23.951);

		const float WarpX = EvaluateSignedFBM(P + WarpOffsetX, Octaves, Roughness, Lacunarity);
		const float WarpY = EvaluateSignedFBM(P + WarpOffsetY, Octaves, Roughness, Lacunarity);
		const float WarpZ = EvaluateSignedFBM(P + WarpOffsetZ, Octaves, Roughness, Lacunarity);

		const FVector WarpedPosition = P + FVector(WarpX, WarpY, WarpZ) * static_cast<double>(Strength);
		return EvaluateSignedFBM(WarpedPosition, Octaves, Roughness, Lacunarity);
	}

	// --- Cellular (V2-B): Worley F1, Worley F2-F1, Voronoi ------------------------------------------

	/**
	 * AUDITED (Noise V2-B): converts a 32-bit hash into a float in [0, 1) -- takes the low 24 bits (a
	 * full 32-bit uint's worth of entropy is more than the mantissa of a float can represent anyway) and
	 * divides by 2^24, per the explicit required conversion. Never returns exactly 1.0.
	 */
	float Hash01(const uint32 Hash)
	{
		return static_cast<float>(Hash & 0x00FFFFFFu) / 16777216.0f;
	}

	/**
	 * AUDITED (Noise V2-B): the ONE deterministic integer hash every cellular helper below builds on --
	 * mixes a 3D cell coordinate, the Noise Seed, and a caller-chosen Salt (to decorrelate different
	 * uses: feature-point X/Y/Z, Voronoi's solid value) via the SAME GetTypeHash/HashCombine idiom
	 * already used elsewhere in this file (see ComputeDeterministicScrambleAngle,
	 * ComputeDynamicMeshGeometryFingerprint) -- well-defined uint32 overflow, no FMath::Rand, no shared/
	 * global RNG state, no pointers, no geometry IDs, deterministic on a given platform regardless of
	 * ParallelFor execution order. GetTypeHash(int32) already handles negative CellX/Y/Z correctly (a
	 * pure bit-pattern reinterpretation, not a magnitude-dependent formula), so cells on either side of
	 * the origin hash exactly as well-distributed as positive ones.
	 */
	uint32 HashCellCoordinate(const int32 CellX, const int32 CellY, const int32 CellZ, const int32 Seed, const uint32 Salt)
	{
		uint32 Hash = GetTypeHash(CellX);
		Hash = HashCombine(Hash, GetTypeHash(CellY));
		Hash = HashCombine(Hash, GetTypeHash(CellZ));
		Hash = HashCombine(Hash, GetTypeHash(Seed));
		Hash = HashCombine(Hash, Salt);
		return Hash;
	}

	/**
	 * AUDITED (Noise V2-B): Random3(CandidateCell, Seed) -- three independent, deterministic [0,1)
	 * values (one per axis), each from HashCellCoordinate salted differently so the X/Y/Z components of
	 * the resulting feature-point jitter are decorrelated from each other (never the same value reused
	 * on all three axes, which would visibly align feature points along the diagonal).
	 */
	FVector ComputeCellFeatureOffset(const FIntVector& Cell, const int32 Seed)
	{
		constexpr uint32 SaltX = 0xA24BAED4u;
		constexpr uint32 SaltY = 0x9F6F6DACu;
		constexpr uint32 SaltZ = 0xC2A2A7DDu;
		return FVector(
			Hash01(HashCellCoordinate(Cell.X, Cell.Y, Cell.Z, Seed, SaltX)),
			Hash01(HashCellCoordinate(Cell.X, Cell.Y, Cell.Z, Seed, SaltY)),
			Hash01(HashCellCoordinate(Cell.X, Cell.Y, Cell.Z, Seed, SaltZ)));
	}

	/** AUDITED (Noise V2-B): the solid per-region Voronoi value -- salted independently from the feature
	 *  X/Y/Z offsets above, so it is never derivable from (and never correlates with) F1/F2 distance --
	 *  hashed from ClosestCell alone, so every point sharing a ClosestCell gets EXACTLY the same value,
	 *  with no gradient inside the region and no smoothing. */
	float ComputeVoronoiValue(const FIntVector& ClosestCell, const int32 Seed)
	{
		constexpr uint32 VoronoiValueSalt = 0x6F4C6BA1u;
		return Hash01(HashCellCoordinate(ClosestCell.X, ClosestCell.Y, ClosestCell.Z, Seed, VoronoiValueSalt));
	}

	/** The two smallest feature-point distances found while scanning the 3x3x3 neighborhood, plus the
	 *  cell that produced the smallest one -- shared by all three cellular Noise Types so they always
	 *  agree on the same feature-point layout for the same Scale/Offset/Seed. */
	struct FCellularNoiseSample
	{
		float F1 = 0.0f;
		float F2 = 0.0f;
		FIntVector ClosestCell = FIntVector::ZeroValue;
	};

	/**
	 * AUDITED (Noise V2-B): the ONE shared cellular evaluation Worley F1/Worley F2-F1/Voronoi all call --
	 * floors P to its containing Cell, scans the fixed 3x3x3 neighborhood (27 candidates, Offset X/Y/Z
	 * each -1..+1, in a FIXED nested-loop order) ONCE, and tracks F1/F2/ClosestCell together in that same
	 * pass (no second pass, no dynamic array of the 27 distances). FeaturePoint = CandidateCell +
	 * Random3(CandidateCell, Seed), per the explicit formula -- exactly the SAME feature points regardless
	 * of which of the three cellular types is asking, since only P and Seed are consulted (never the
	 * enum). Tie-breaking is deterministic: strict `<` comparisons only, evaluated in the SAME fixed
	 * iteration order every call, so a Distance exactly equal to the current best never overwrites it --
	 * the result depends only on the 27 (CandidateCell, Distance) pairs, never on execution order (safe
	 * under ParallelFor). No heap allocation, no TArray/TMap, no locks -- three doubles and one FIntVector
	 * of local state for the whole scan.
	 */
	FCellularNoiseSample EvaluateCellularNoise(const FVector& P, const int32 Seed)
	{
		const FIntVector Cell(FMath::FloorToInt(P.X), FMath::FloorToInt(P.Y), FMath::FloorToInt(P.Z));

		float BestF1 = TNumericLimits<float>::Max();
		float BestF2 = TNumericLimits<float>::Max();
		FIntVector BestCell = Cell;

		for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
		{
			for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const FIntVector CandidateCell(Cell.X + OffsetX, Cell.Y + OffsetY, Cell.Z + OffsetZ);
					const FVector RandomOffset = ComputeCellFeatureOffset(CandidateCell, Seed);
					const FVector FeaturePoint(
						static_cast<double>(CandidateCell.X) + RandomOffset.X,
						static_cast<double>(CandidateCell.Y) + RandomOffset.Y,
						static_cast<double>(CandidateCell.Z) + RandomOffset.Z);
					const float Distance = static_cast<float>((FeaturePoint - P).Size());

					if (Distance < BestF1)
					{
						BestF2 = BestF1;
						BestF1 = Distance;
						BestCell = CandidateCell;
					}
					else if (Distance < BestF2)
					{
						BestF2 = Distance;
					}
				}
			}
		}

		FCellularNoiseSample Sample;
		Sample.F1 = BestF1;
		Sample.F2 = BestF2;
		Sample.ClosestCell = BestCell;
		return Sample;
	}

	// --- Alligator (V2-C): a distinct cellular type from a two-largest RBF-contribution difference ----

	/**
	 * AUDITED (Noise V2-C): the SINGLE-octave Alligator base value -- uses the EXACT SAME feature-point
	 * layout as WorleyF1/WorleyF2MinusF1/Voronoi (CandidateCell + Random3(CandidateCell, Seed),
	 * ComputeCellFeatureOffset, same 3x3x3 neighborhood, same fixed iteration order), but a DIFFERENT
	 * combination rule: each candidate contributes CellValue*RBF(Distance) (0 once Distance >= 1, a
	 * smoothstep-shaped falloff for Distance in [0,1)), and the result is the DIFFERENCE between the two
	 * LARGEST contributions found -- never a Worley distance, never Voronoi's solid per-cell hash. This
	 * "difference of two competing round volumes" is what produces the scale-like/rounded-volume look,
	 * distinct from both Worley families and Voronoi. Same single-pass, no-container, strict-comparison,
	 * fixed-order contract as EvaluateCellularNoise -- safe for ParallelFor, deterministic regardless of
	 * execution order.
	 */
	float EvaluateBaseAlligator(const FVector& P, const int32 Seed)
	{
		// Distinct from the Feature X/Y/Z salts (ComputeCellFeatureOffset) and from the Voronoi value
		// salt (ComputeVoronoiValue) -- documented per the explicit requirement.
		constexpr uint32 AlligatorValueSalt = 0x3D4C2F17u;

		const FIntVector Cell(FMath::FloorToInt(P.X), FMath::FloorToInt(P.Y), FMath::FloorToInt(P.Z));

		float Largest = 0.0f;
		float SecondLargest = 0.0f;

		for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
		{
			for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const FIntVector CandidateCell(Cell.X + OffsetX, Cell.Y + OffsetY, Cell.Z + OffsetZ);
					const FVector RandomOffset = ComputeCellFeatureOffset(CandidateCell, Seed);
					const FVector FeaturePoint(
						static_cast<double>(CandidateCell.X) + RandomOffset.X,
						static_cast<double>(CandidateCell.Y) + RandomOffset.Y,
						static_cast<double>(CandidateCell.Z) + RandomOffset.Z);
					const float Distance = static_cast<float>((FeaturePoint - P).Size());

					float Contribution = 0.0f;
					if (Distance < 1.0f)
					{
						const float T = 1.0f - Distance;
						const float RBF = T * T * (3.0f - 2.0f * T); // smoothstep, 0 at Distance=1, 1 at Distance=0
						const float CellValue = Hash01(HashCellCoordinate(CandidateCell.X, CandidateCell.Y, CandidateCell.Z, Seed, AlligatorValueSalt));
						Contribution = CellValue * RBF;
					}

					if (Contribution > Largest)
					{
						SecondLargest = Largest;
						Largest = Contribution;
					}
					else if (Contribution > SecondLargest)
					{
						SecondLargest = Contribution;
					}
				}
			}
		}

		return FMath::Max(Largest - SecondLargest, 0.0f);
	}

	/**
	 * AUDITED (Noise V2-C): Alligator's multi-octave accumulation -- SAME contract as EvaluateSignedFBM/
	 * EvaluateBillow/EvaluateRidged (Frequency *= Lacunarity, Amplitude *= Roughness, starting at 1),
	 * except EvaluateBaseAlligator is ALREADY in [0,1] (never signed), so -- like Ridged -- this
	 * accumulates directly in [0,1] space and skips any signed-to-[0,1] remap. Octaves == 1 reduces to
	 * EvaluateBaseAlligator(P, Seed) exactly (Sum = Sample*1, AmplitudeSum = 1). Does not apply the
	 * frequency-scale-1.64 convention some third-party Alligator implementations use -- the Vertex Mask
	 * Forge Scale control remains the only visible base-frequency knob, per the explicit requirement.
	 */
	float EvaluateAlligator(const FVector& P, const int32 Seed, const int32 Octaves, const float Roughness, const float Lacunarity)
	{
		double Frequency = 1.0;
		float Amplitude = 1.0f;
		double Sum = 0.0;
		float AmplitudeSum = 0.0f;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			const FVector OctavePosition = P * Frequency;
			const float Sample = EvaluateBaseAlligator(OctavePosition, Seed);
			Sum += static_cast<double>(Sample * Amplitude);
			AmplitudeSum += Amplitude;
			Frequency *= Lacunarity;
			Amplitude *= Roughness;
		}
		constexpr float WeightEpsilon = 1e-4f;
		return (AmplitudeSum > WeightEpsilon) ? FMath::Clamp(static_cast<float>(Sum / AmplitudeSum), 0.0f, 1.0f) : 0.0f;
	}

	/**
	 * AUDITED (Noise V1, extended V2-C): the expensive, GENERATIVE type-dispatch half of Noise -- samples
	 * ONE value of whichever Noise Type Params selects at NoisePosition (an ALREADY-PREPARED noise-space
	 * position -- see ComputeRawNoiseValue for how LocalPosition/Scale/Offset/SeedOffset combine into it;
	 * this function never re-derives that itself, so it can be called at Blur's six extra offset
	 * positions without duplicating the position-preparation logic), returns a value already reduced to
	 * [0, 1] (no Blur/Multiplier/Levels/Invert applied here -- see ComputeRawNoiseValue for Blur,
	 * ApplyNoiseArtisticParams for the artistic stage).
	 *
	 * PERLIN: SignedNoise = FMath::PerlinNoise3D(NoisePosition); Mask = saturate(SignedNoise*0.5+0.5).
	 *
	 * FBM (FractalPerlin): sums SEVERAL Perlin octaves at increasing frequency (Frequency *= Lacunarity
	 * each step, starting at 1) and decreasing amplitude (Amplitude *= Roughness each step, starting at
	 * 1), normalizes by the TOTAL amplitude weight, and converts signed-to-[0,1] ONLY ONCE, AFTER that
	 * sum -- per the explicit "Não remapear cada oitava individualmente antes da soma" requirement:
	 * SignedFBM = Sum(Perlin(P*Frequency)*Amplitude) / Sum(Amplitude); Mask = saturate(SignedFBM*0.5+0.5).
	 * WeightSum is guarded against zero (Octaves clamped to >= 1, Roughness >= 0, so WeightSum is always
	 * >= 1 in practice, but an Epsilon floor is still applied defensively) -- never a division by zero.
	 *
	 * SAFETY: FMath::PerlinNoise3D itself is confirmed (UnrealMath.cpp) to read ONLY a function-local
	 * `static const int32 Permutation[512]` lookup table -- populated once at first use via C++11
	 * thread-safe static initialization and never mutated afterward, no FMath::Rand/global RNG state,
	 * no heap access -- so concurrent reads from multiple ParallelFor workers are safe with no locking,
	 * and the result depends ONLY on the input Location (fully deterministic, order-independent). All
	 * inputs are defensively clamped (Octaves/Roughness/Lacunarity/TurbulenceStrength to their documented
	 * safe ranges) and the final result is FMath::IsFinite-checked before being trusted, so this can
	 * never return NaN/Inf even from a pathological parameter combination.
	 */
	float EvaluateUnblurredNoiseAtPosition(const FVector& NoisePosition, const FVertexMaskForgeNoiseGenerativeParams& Params)
	{
		if (Params.NoiseType == EVertexMaskForgeNoiseType::Perlin)
		{
			float SignedResult = FMath::PerlinNoise3D(NoisePosition);
			if (!FMath::IsFinite(SignedResult))
			{
				SignedResult = 0.0f;
			}
			return FMath::Clamp(SignedResult * 0.5f + 0.5f, 0.0f, 1.0f);
		}

		// V2-B cellular types -- not multi-octave (Octaves/Roughness/Lacunarity/TurbulenceStrength are
		// never read here), share the SAME feature-point layout (EvaluateCellularNoise depends only on
		// NoisePosition and Params.Seed, never on which of the three types is asking).
		if (Params.NoiseType == EVertexMaskForgeNoiseType::WorleyF1
			|| Params.NoiseType == EVertexMaskForgeNoiseType::WorleyF2MinusF1
			|| Params.NoiseType == EVertexMaskForgeNoiseType::Voronoi)
		{
			const FCellularNoiseSample Sample = EvaluateCellularNoise(NoisePosition, Params.Seed);

			float RawMask;
			switch (Params.NoiseType)
			{
			case EVertexMaskForgeNoiseType::WorleyF1:
				// RawMask = saturate(F1) -- Euclidean distance to the nearest feature point.
				RawMask = Sample.F1;
				break;
			case EVertexMaskForgeNoiseType::WorleyF2MinusF1:
				// RawMask = saturate(max(F2-F1, 0)) -- near zero at cell edges, never a plain inversion
				// or remap of F1 (a DIFFERENT quantity: the gap between the two nearest distances).
				RawMask = FMath::Max(Sample.F2 - Sample.F1, 0.0f);
				break;
			case EVertexMaskForgeNoiseType::Voronoi:
			default:
				// Solid per-region value -- hashed from ClosestCell alone, never from F1/F2 distance.
				RawMask = ComputeVoronoiValue(Sample.ClosestCell, Params.Seed);
				break;
			}

			if (!FMath::IsFinite(RawMask))
			{
				RawMask = 0.0f;
			}
			return FMath::Clamp(RawMask, 0.0f, 1.0f);
		}

		const int32 SafeOctaves = FMath::Clamp(Params.Octaves, 1, 8);
		const float SafeRoughness = FMath::Clamp(Params.Roughness, 0.0f, 1.0f);
		const float SafeLacunarity = FMath::Max(Params.Lacunarity, 1.0f);
		const float SafeTurbulenceStrength = FMath::Clamp(Params.TurbulenceStrength, 0.0f, 5.0f);

		// Ridged and Alligator are the two types whose per-octave accumulation is already in [0,1] space
		// -- see EvaluateRidged/EvaluateAlligator's own doc comments for why they must skip the shared
		// signed-to-[0,1] remap below.
		if (Params.NoiseType == EVertexMaskForgeNoiseType::Ridged)
		{
			float RawMask = EvaluateRidged(NoisePosition, SafeOctaves, SafeRoughness, SafeLacunarity);
			if (!FMath::IsFinite(RawMask))
			{
				RawMask = 0.0f;
			}
			return FMath::Clamp(RawMask, 0.0f, 1.0f);
		}
		if (Params.NoiseType == EVertexMaskForgeNoiseType::Alligator)
		{
			float RawMask = EvaluateAlligator(NoisePosition, Params.Seed, SafeOctaves, SafeRoughness, SafeLacunarity);
			if (!FMath::IsFinite(RawMask))
			{
				RawMask = 0.0f;
			}
			return FMath::Clamp(RawMask, 0.0f, 1.0f);
		}

		float SignedResult;
		switch (Params.NoiseType)
		{
		case EVertexMaskForgeNoiseType::Billow:
			SignedResult = EvaluateBillow(NoisePosition, SafeOctaves, SafeRoughness, SafeLacunarity);
			break;
		case EVertexMaskForgeNoiseType::Turbulence:
			SignedResult = EvaluateTurbulence(NoisePosition, SafeOctaves, SafeRoughness, SafeLacunarity, SafeTurbulenceStrength);
			break;
		case EVertexMaskForgeNoiseType::FractalPerlin:
		default:
			SignedResult = EvaluateSignedFBM(NoisePosition, SafeOctaves, SafeRoughness, SafeLacunarity);
			break;
		}

		if (!FMath::IsFinite(SignedResult))
		{
			SignedResult = 0.0f;
		}
		return FMath::Clamp(SignedResult * 0.5f + 0.5f, 0.0f, 1.0f);
	}

	/**
	 * AUDITED (V2-C): prepares BaseNoisePosition (LocalPosition * (Scale/100) + Offset + SeedOffset --
	 * UNCHANGED formula, per the explicit "preserve the current Seed/SeedOffset prep" requirement) and
	 * then applies the universal Blur kernel BEFORE handing off to EvaluateUnblurredNoiseAtPosition for
	 * the actual per-type evaluation.
	 *
	 * Blur <= 0 (the default): an EXACT short-circuit straight to
	 * EvaluateUnblurredNoiseAtPosition(BaseNoisePosition, Params) -- zero extra evaluations, reproducing
	 * every one of the eight pre-V2-C Noise Types bit-for-bit (this function does not itself branch on
	 * NoiseType at all, so nothing about Blur=0 can diverge per type).
	 *
	 * Blur > 0: Radius = Blur*0.5 (noise-space units); a SYMMETRIC seven-tap kernel -- center weight 0.4,
	 * each of the six axis-aligned taps (+-X, +-Y, +-Z, each offset by Radius) weight 0.1, summing to
	 * exactly 1.0 -- averages EvaluateUnblurredNoiseAtPosition at those seven noise-space positions. Pure
	 * position-space sampling of the SAME procedural field (never mesh adjacency, vertex neighbors, or
	 * Actor Transform), so it stays Local-Space/topology-independent and Nanite/non-Nanite-consistent
	 * exactly like every other Noise Type. NEVER calls ComputeRawNoiseValue itself (no recursion) --
	 * every tap goes directly to EvaluateUnblurredNoiseAtPosition, which never reads Params.Blur.
	 */
	float ComputeRawNoiseValue(const FVector& LocalPosition, const FVertexMaskForgeNoiseGenerativeParams& Params, const FVector& SeedOffset)
	{
		constexpr double ScaleEpsilon = 1e-4;
		const double SafeScaleX = FMath::Max(FMath::Abs(static_cast<double>(Params.ScaleX)), ScaleEpsilon);
		const double SafeScaleY = FMath::Max(FMath::Abs(static_cast<double>(Params.ScaleY)), ScaleEpsilon);
		const double SafeScaleZ = FMath::Max(FMath::Abs(static_cast<double>(Params.ScaleZ)), ScaleEpsilon);

		const FVector BaseNoisePosition(
			LocalPosition.X * (SafeScaleX / 100.0) + static_cast<double>(Params.OffsetX) + SeedOffset.X,
			LocalPosition.Y * (SafeScaleY / 100.0) + static_cast<double>(Params.OffsetY) + SeedOffset.Y,
			LocalPosition.Z * (SafeScaleZ / 100.0) + static_cast<double>(Params.OffsetZ) + SeedOffset.Z);

		const float SafeBlur = FMath::Clamp(Params.Blur, 0.0f, 1.0f);
		if (SafeBlur <= 0.0f)
		{
			return EvaluateUnblurredNoiseAtPosition(BaseNoisePosition, Params);
		}

		const double Radius = static_cast<double>(SafeBlur) * 0.5;
		const float CenterValue = EvaluateUnblurredNoiseAtPosition(BaseNoisePosition, Params);
		const float XPositive = EvaluateUnblurredNoiseAtPosition(BaseNoisePosition + FVector(Radius, 0.0, 0.0), Params);
		const float XNegative = EvaluateUnblurredNoiseAtPosition(BaseNoisePosition - FVector(Radius, 0.0, 0.0), Params);
		const float YPositive = EvaluateUnblurredNoiseAtPosition(BaseNoisePosition + FVector(0.0, Radius, 0.0), Params);
		const float YNegative = EvaluateUnblurredNoiseAtPosition(BaseNoisePosition - FVector(0.0, Radius, 0.0), Params);
		const float ZPositive = EvaluateUnblurredNoiseAtPosition(BaseNoisePosition + FVector(0.0, 0.0, Radius), Params);
		const float ZNegative = EvaluateUnblurredNoiseAtPosition(BaseNoisePosition - FVector(0.0, 0.0, Radius), Params);

		const float Blurred =
			CenterValue * 0.4f
			+ XPositive * 0.1f + XNegative * 0.1f
			+ YPositive * 0.1f + YNegative * 0.1f
			+ ZPositive * 0.1f + ZNegative * 0.1f;

		float RawNoise = Blurred;
		if (!FMath::IsFinite(RawNoise))
		{
			RawNoise = 0.0f;
		}
		return FMath::Clamp(RawNoise, 0.0f, 1.0f);
	}

	/**
	 * AUDITED (Noise V1): guarantees GeneratorState.NoiseRawCache is valid for WorkingMesh's CURRENT
	 * geometry AND the CURRENT generative parameters -- the ONE place ComputeRawNoiseValue is ever
	 * invoked in a loop. Reuses the cache verbatim only when BOTH NoiseCacheFingerprint ==
	 * WorkingMesh.GeometryFingerprint AND NoiseCacheUsedParams == Params (see
	 * FVertexMaskForgeNoiseGenerativeParams::operator==) -- either differing forces a full recompute.
	 * Multiplier/Levels/Invert/Opacity/Blend Mode changes never reach this function at all (see
	 * OnNoiseArtisticParamChanged, which calls the cheap ApplyNoiseArtisticParams path directly).
	 *
	 * PARALLELIZED (audited): one ParallelFor over the domain's own vertex count, matching
	 * GenerateAmbientOcclusionMask's own audited pattern -- ComputeRawNoiseValue's only dependency is
	 * the read-only Params/SeedOffset (captured by reference, never mutated) and each vertex's own
	 * position, so results never depend on execution order. WorkingMesh (const, Identity) and
	 * GeneratorState (mutable, Artistic State -- M16-J.0B.1's WorkingMesh Domain Split) are two separate
	 * references since the cache written here lives on GeneratorState while the fingerprint/geometry read
	 * here lives on WorkingMesh.
	 */
	void EnsureNoiseRawCache(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		FVertexMaskForgeGeneratorState& GeneratorState,
		const bool bUseSourceTopology,
		const FStaticMeshLODResources* LOD0ForRenderVertexDomain,
		const FVertexMaskForgeNoiseGenerativeParams& Params)
	{
		if (!WorkingMesh.Mesh.IsValid())
		{
			return;
		}
		if (GeneratorState.NoiseCacheFingerprint == WorkingMesh.GeometryFingerprint
			&& GeneratorState.NoiseCacheUsedParams == Params
			&& !GeneratorState.NoiseRawCache.IsEmpty())
		{
			return;
		}

		const FVector SeedOffset = ComputeNoiseSeedOffset(Params.Seed);

		if (bUseSourceTopology)
		{
			using namespace UE::Geometry;
			const FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
			const int32 MaxVID = Mesh.MaxVertexID();
			TArray<float> RawValues;
			RawValues.SetNumZeroed(MaxVID);

			ParallelFor(MaxVID, [&Mesh, &RawValues, &Params, &SeedOffset](int32 VertexID)
			{
				if (!Mesh.IsVertex(VertexID))
				{
					return;
				}
				const FVector LocalPosition = Mesh.GetVertex(VertexID);
				RawValues[VertexID] = ComputeRawNoiseValue(LocalPosition, Params, SeedOffset);
			});

			GeneratorState.NoiseRawCache = MoveTemp(RawValues);
		}
		else if (LOD0ForRenderVertexDomain)
		{
			const FPositionVertexBuffer& Positions = LOD0ForRenderVertexDomain->VertexBuffers.PositionVertexBuffer;
			const int32 NumRenderVerts = static_cast<int32>(Positions.GetNumVertices());
			TArray<float> RawValues;
			RawValues.SetNumUninitialized(NumRenderVerts);

			ParallelFor(NumRenderVerts, [&Positions, &RawValues, &Params, &SeedOffset](int32 RenderIndex)
			{
				const FVector LocalPosition(Positions.VertexPosition(RenderIndex));
				RawValues[RenderIndex] = ComputeRawNoiseValue(LocalPosition, Params, SeedOffset);
			});

			GeneratorState.NoiseRawCache = MoveTemp(RawValues);
		}
		else
		{
			GeneratorState.NoiseRawCache.Reset();
			return;
		}

		GeneratorState.NoiseCacheFingerprint = WorkingMesh.GeometryFingerprint;
		GeneratorState.NoiseCacheUsedParams = Params;

		UE_LOG(LogVertexMaskForge, Log,
			TEXT("Vertex Mask Forge: Noise raw pattern computed (%d vertices)."),
			GeneratorState.NoiseRawCache.Num());
	}

	/**
	 * AUDITED (Noise V1): the CHEAP, purely-downstream half of Noise generation -- turns the cached raw
	 * pattern (already in [0, 1]) into the final [0, 1] mask value, via Multiplier -> Levels -> Invert.
	 * Reuses VertexMaskForgeGeneratorUtils::ApplyCurvatureLevels verbatim for the Levels step (same
	 * generic, epsilon-safe-denominator remap contract -- see that function's own doc comment; calling
	 * it here is NOT a formula duplication, it is the SAME shared helper, despite its Curvature-era
	 * name) -- per the explicit "reutilize os helpers existentes... não duplique fórmulas" requirement.
	 * Invert is applied LAST, after Levels, exactly like Curvature's own Invert.
	 */
	TArray<float> ApplyNoiseArtisticParams(
		const TArray<float>& RawCache,
		const float Multiplier,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert)
	{
		const float ClampedMultiplier = FMath::Max(Multiplier, 0.0f);
		const float ClampedLevelsMin = FMath::Clamp(LevelsMin, 0.0f, 1.0f);
		const float ClampedLevelsMax = FMath::Clamp(LevelsMax, 0.0f, 1.0f);

		TArray<float> Result;
		Result.SetNumUninitialized(RawCache.Num());
		for (int32 i = 0; i < RawCache.Num(); ++i)
		{
			const float Multiplied = FMath::Clamp(RawCache[i] * ClampedMultiplier, 0.0f, 1.0f);
			const float Leveled = VertexMaskForgeGeneratorUtils::ApplyCurvatureLevels(Multiplied, ClampedLevelsMin, ClampedLevelsMax);
			Result[i] = bInvert ? (1.0f - Leveled) : Leveled;
		}
		return Result;
	}
}

namespace VertexMaskForgeNoiseGenerator
{
	FVertexMaskForgeScalarMask GenerateNoiseMask(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		FVertexMaskForgeGeneratorState& GeneratorState,
		const FStaticMeshLODResources& LOD0,
		const FVertexMaskForgeNoiseGenerativeParams& Params,
		const float Multiplier,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::Noise;

		const int32 NumRenderVerts = static_cast<int32>(LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (!WorkingMesh.Mesh.IsValid() || NumRenderVerts <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		EnsureNoiseRawCache(WorkingMesh, GeneratorState, /*bUseSourceTopology=*/false, &LOD0, Params);
		if (GeneratorState.NoiseRawCache.Num() != NumRenderVerts)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		const TArray<float> Processed = ApplyNoiseArtisticParams(GeneratorState.NoiseRawCache, Multiplier, LevelsMin, LevelsMax, bInvert);

		Mask.Values = Processed;
		Mask.bHasValue.Init(true, NumRenderVerts);

		double Sum = 0.0;
		for (int32 i = 0; i < Processed.Num(); ++i)
		{
			const float Value = Processed[i];
			++Mask.NumValidValues;
			Sum += Value;
			Mask.MinValue = (Mask.NumValidValues == 1) ? Value : FMath::Min(Mask.MinValue, Value);
			Mask.MaxValue = (Mask.NumValidValues == 1) ? Value : FMath::Max(Mask.MaxValue, Value);
			if (Value <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
			if (Value >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}

	FVertexMaskForgeScalarMask GenerateNoiseMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		FVertexMaskForgeGeneratorState& GeneratorState,
		const FVertexMaskForgeNoiseGenerativeParams& Params,
		const float Multiplier,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::Noise;

		if (!WorkingMesh.Mesh.IsValid())
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}
		const FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		Mask.RenderVertexCount = Mesh.VertexCount();

		EnsureNoiseRawCache(WorkingMesh, GeneratorState, /*bUseSourceTopology=*/true, nullptr, Params);
		if (GeneratorState.NoiseRawCache.IsEmpty())
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		const TArray<float> Processed = ApplyNoiseArtisticParams(GeneratorState.NoiseRawCache, Multiplier, LevelsMin, LevelsMax, bInvert);

		const int32 MaxVID = Mesh.MaxVertexID();
		Mask.Values.SetNumZeroed(MaxVID);
		Mask.bHasValue.Init(false, MaxVID);

		double Sum = 0.0;
		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			if (!Processed.IsValidIndex(VertexID))
			{
				continue;
			}
			const float Value = Processed[VertexID];
			Mask.Values[VertexID] = Value;
			Mask.bHasValue[VertexID] = true;
			++Mask.NumValidValues;
			Sum += Value;
			Mask.MinValue = (Mask.NumValidValues == 1) ? Value : FMath::Min(Mask.MinValue, Value);
			Mask.MaxValue = (Mask.NumValidValues == 1) ? Value : FMath::Max(Mask.MaxValue, Value);
			if (Value <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
			if (Value >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}
}
