#include "BeautifyLibrary.h"

#include "BeautifyLayout.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectHash.h"

namespace
{
	int32 BeautifyNamedGraph(UObject* Asset, FName GraphName, const TMap<FString, FString>* CommentTitles)
	{
		if (!Asset)
		{
			UE_LOG(LogBeautifyBP, Warning, TEXT("BeautifyGraph: no asset given."));
			return 0;
		}

		// A Blueprint lists its graphs; any other asset (SimFlow) owns its graph as a subobject.
		TArray<UEdGraph*> Graphs;
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
		if (Blueprint)
		{
			Blueprint->GetAllGraphs(Graphs);
		}
		else
		{
			TArray<UObject*> Inner;
			GetObjectsWithOuter(Asset, Inner, EGetObjectsFlags::IncludeNestedObjects);
			for (UObject* Object : Inner)
			{
				if (UEdGraph* Graph = Cast<UEdGraph>(Object))
				{
					Graphs.Add(Graph);
				}
			}
		}

		UEdGraph* Graph = nullptr;
		if (GraphName.IsNone())
		{
			UEdGraph* const* First = Graphs.FindByPredicate([](const UEdGraph* Candidate) { return BeautifyBP::CanBeautify(Candidate); });
			Graph = Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : (First ? *First : nullptr);
		}
		else
		{
			UEdGraph* const* Found = Graphs.FindByPredicate([GraphName](const UEdGraph* Candidate) { return Candidate && Candidate->GetFName() == GraphName; });
			Graph = Found ? *Found : nullptr;
		}
		if (!Graph)
		{
			// Name the options, so a script (or the agent driving it) can fix its call.
			TArray<FString> Names;
			for (const UEdGraph* Candidate : Graphs)
			{
				Names.Add(Candidate->GetName());
			}
			UE_LOG(LogBeautifyBP, Warning, TEXT("BeautifyGraph: %s has no graph named '%s' that Beautify may arrange. Graphs: %s"),
				*Asset->GetName(), *GraphName.ToString(), *FString::Join(Names, TEXT(", ")));
			return 0;
		}
		if (!BeautifyBP::CanBeautify(Graph))
		{
			UE_LOG(LogBeautifyBP, Warning, TEXT("BeautifyGraph: %s.%s is read-only, or its graph type isn't a Blueprint graph or listed in Other Graph Schemas."),
				*Asset->GetName(), *Graph->GetName());
			return 0;
		}
		return BeautifyBP::Beautify(Graph, false, CommentTitles);
	}
}

int32 UBeautifyLibrary::BeautifyGraph(UObject* Asset, FName GraphName)
{
	return BeautifyNamedGraph(Asset, GraphName, nullptr);
}

int32 UBeautifyLibrary::BeautifyGraphWithTitles(UObject* Asset, FName GraphName, const TMap<FString, FString>& CommentTitles)
{
	return BeautifyNamedGraph(Asset, GraphName, &CommentTitles);
}

static FAutoConsoleCommand BeautifyFormatCommand(
	TEXT("BeautifyBP.Format"),
	TEXT("Arrange a graph. Usage: BeautifyBP.Format /Game/Path/AssetName [GraphName]. A Blueprint defaults to its event graph."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.IsEmpty())
		{
			UE_LOG(LogBeautifyBP, Warning, TEXT("Usage: BeautifyBP.Format /Game/Path/AssetName [GraphName]"));
			return;
		}
		// Accept a package path (/Game/BP_X) as well as an object path (/Game/BP_X.BP_X).
		FString Path = Args[0];
		if (!Path.Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		}
		UObject* Asset = LoadObject<UObject>(nullptr, *Path);
		if (!Asset)
		{
			UE_LOG(LogBeautifyBP, Warning, TEXT("BeautifyBP.Format: no asset at %s"), *Path);
			return;
		}
		const int32 Placed = UBeautifyLibrary::BeautifyGraph(Asset, Args.Num() > 1 ? FName(*Args[1]) : NAME_None);
		UE_LOG(LogBeautifyBP, Display, TEXT("BeautifyBP.Format: placed %d nodes in %s."), Placed, *Asset->GetName());
	}));
