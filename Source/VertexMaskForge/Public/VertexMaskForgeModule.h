#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FSpawnTabArgs;
class SDockTab;

class FVertexMaskForgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void UnregisterMenus();

	TSharedRef<SDockTab> SpawnVertexMaskForgeTab(const FSpawnTabArgs& SpawnTabArgs);

	FDelegateHandle ToolMenusStartupHandle;
};
