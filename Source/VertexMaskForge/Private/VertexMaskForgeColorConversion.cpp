#include "VertexMaskForgeColorConversion.h"

namespace VertexMaskForgeColorConversion
{
	FVector4f ToLinearColorF(const FColor& Color)
	{
		return FVector4f(
			static_cast<float>(Color.R) / 255.0f,
			static_cast<float>(Color.G) / 255.0f,
			static_cast<float>(Color.B) / 255.0f,
			static_cast<float>(Color.A) / 255.0f);
	}

	FColor ToDisplayFColor(const FVector4f& Color)
	{
		auto ClampChannel = [](const float Value)
		{
			return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 255.0f), 0, 255));
		};
		return FColor(ClampChannel(Color.X), ClampChannel(Color.Y), ClampChannel(Color.Z), ClampChannel(Color.W));
	}
}
