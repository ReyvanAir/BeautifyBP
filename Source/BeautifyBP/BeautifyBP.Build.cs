using UnrealBuildTool;

public class BeautifyBP : ModuleRules
{
	public BeautifyBP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"InputCore",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"ToolMenus",
			"Kismet",
			"GraphEditor",
			"BlueprintGraph",
		});
	}
}
