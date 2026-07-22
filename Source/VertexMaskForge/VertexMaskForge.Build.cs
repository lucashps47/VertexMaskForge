using UnrealBuildTool;

public class VertexMaskForge : ModuleRules
{
	public VertexMaskForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"ToolMenus",
				"ContentBrowser",
				"MeshDescription",
				"StaticMeshDescription",
				"MeshConversion",
				"GeometryCore",
				// Required for BeginInitResource(FRenderResource*) (RenderCore/Public/RenderResource.h),
				// used to initialize the transient FColorVertexBuffer allocated for the non-destructive
				// component-level Vertex Color preview (FStaticMeshComponentLODInfo::OverrideVertexColors).
				"RenderCore",
			}
		);
	}
}
