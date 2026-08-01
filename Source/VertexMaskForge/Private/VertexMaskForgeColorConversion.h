#pragma once

#include "CoreMinimal.h"

/**
 * M16-K.5F: the single shared implementation of the FColor<->FVector4f conversion pair this plugin's
 * composition paths already needed -- extracted verbatim (no formula/rounding/clamp change) from what
 * were, before this checkpoint, TWO deliberately-duplicated copies of ToDisplayFColor
 * (SVertexMaskForgePanel.cpp and VertexMaskForgeDisplayColorDerivation.cpp) plus several inline
 * FColor->FVector4f constructions repeating the same /255.0f pattern. Both real callers now consume
 * this module instead of their own local copies -- see each file's own diff for confirmation.
 *
 * Pure, scalar, stateless -- no batching, no sRGB/linear transform, no domain (render-vertex/corner)
 * awareness of any kind. Operates on exactly one FColor/FVector4f pair per call.
 */
namespace VertexMaskForgeColorConversion
{
	/** FColor (0-255 per channel) -> FVector4f (0.0-1.0 per channel), plain division by 255.0f, all
	 *  four channels including Alpha. No clamping -- FColor's own uint8 channels are already in range. */
	FVector4f ToLinearColorF(const FColor& Color);

	/** FVector4f -> FColor: each channel independently rounded (FMath::RoundToInt(Value * 255.0f)) and
	 *  clamped to [0, 255] before the uint8 narrowing conversion -- values below 0.0 or above 1.0 are
	 *  clamped after scaling, never before. Applies to all four channels including Alpha; no sRGB/linear
	 *  transform of any kind. */
	FColor ToDisplayFColor(const FVector4f& Color);
}
