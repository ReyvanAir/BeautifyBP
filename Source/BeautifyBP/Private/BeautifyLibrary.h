#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "BeautifyLibrary.generated.h"

/**
 * Script entry points, so tools that generate graphs (MCP, Python) can tidy them right after.
 * Python: unreal.BeautifyLibrary.beautify_graph(asset, "EventGraph")
 * Console: BeautifyBP.Format /Game/Path/BP_Name [GraphName]
 */
UCLASS()
class UBeautifyLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Arranges one graph of an asset as one undo step: a Blueprint, or an asset whose graph type is listed in
	 * Project Settings > Beautify BP > Other Graph Schemas (such as a SimFlow asset). Without a graph name, a
	 * Blueprint uses its event graph and other assets their first graph Beautify may arrange.
	 * A graph that isn't open in an editor uses estimated node sizes, so spacing is looser.
	 * Returns how many nodes were placed: 0 when the graph isn't found or can't be arranged.
	 */
	UFUNCTION(BlueprintCallable, Category = "Beautify BP")
	static int32 BeautifyGraph(UObject* Asset, FName GraphName = NAME_None);

	/**
	 * BeautifyGraph, with your own comment box titles. Keys are event or start node names, matched loosely:
	 * "OnTaskTick", "On Task Tick" and "Event On Task Tick" all name the same chain. Chains without a key
	 * get the default title.
	 * Python: unreal.BeautifyLibrary.beautify_graph_with_titles(bp, "EventGraph", {"OnTaskTick": "Check if the player looks at the target"})
	 */
	UFUNCTION(BlueprintCallable, Category = "Beautify BP")
	static int32 BeautifyGraphWithTitles(UObject* Asset, FName GraphName, const TMap<FString, FString>& CommentTitles);
};
