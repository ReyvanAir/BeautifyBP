#include "BeautifyComments.h"

#include "BeautifySettings.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Tunnel.h"
#include "Layout/SlateRect.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace BeautifyBP
{
	namespace
	{
		// Package metadata isn't part of undo history, so tags are never removed: an undone delete
		// brings the comment back still tagged, and the next run replaces it instead of duplicating it.
		const TCHAR* GeneratedKey = TEXT("BeautifyBP.GeneratedTitle");

		FBox2f BoundsOf(const TArray<UEdGraphNode*>& Nodes, const TMap<UEdGraphNode*, FVector2f>& Sizes)
		{
			FBox2f Box(ForceInit);
			for (UEdGraphNode* Node : Nodes)
			{
				Box += Node->GetPosition();
				Box += Node->GetPosition() + Sizes.FindRef(Node);
			}
			return Box;
		}

		FSlateRect CommentRect(const FBox2f& Box, int32 FontSize)
		{
			const float Title = CommentTitleHeight + (FontSize - 18);
			return FSlateRect(Box.Min.X - CommentPad, Box.Min.Y - Title, Box.Max.X + CommentPad, Box.Max.Y + CommentPad);
		}

		/** The title as drawn on the node ("Print String", not the ListView "PrintString"), without the "Target is ..." line. */
		FString VisibleTitle(const UEdGraphNode* Node)
		{
			FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			FString FirstLine;
			return Title.Split(TEXT("\n"), &FirstLine, nullptr) ? FirstLine : Title;
		}

		FString RootName(const UEdGraphNode* Root)
		{
			FString Name = VisibleTitle(Root);
			if (Root->IsA<UK2Node_Event>())
			{
				Name.RemoveFromStart(TEXT("Event "));
			}
			return Name;
		}

		/** Lowercase without spaces, underscores or a leading "event", so "OnTaskTick" matches "Event On Task Tick". */
		FString TitleKey(const FString& Text)
		{
			FString Key = Text.ToLower().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
			Key.RemoveFromStart(TEXT("event"));
			return Key;
		}

		/** The event or function name, then the first few function calls it runs: "On Task Tick: Get All Actors With Tag, Finish Task". */
		FString TitleFor(const FChain& Chain, const TMap<FString, FString>* Titles)
		{
			const FString Name = RootName(Chain.Root);
			if (Titles)
			{
				const FString Key = TitleKey(Name);
				for (const TPair<FString, FString>& Pair : *Titles)
				{
					if (TitleKey(Pair.Key) == Key)
					{
						return Pair.Value;
					}
				}
			}
			// Blueprints list their function calls. Other graph types (SimFlow) have no such split, so every step counts.
			TArray<FString> Calls;
			for (const UEdGraphNode* Node : Chain.Nodes)
			{
				const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				const bool bStep = Call ? !Call->IsNodePure() : Node != Chain.Root && !Node->IsA<UK2Node>();
				if (bStep && Calls.Num() < 3)
				{
					Calls.AddUnique(VisibleTitle(Node));
				}
			}
			return Calls.IsEmpty() ? Name : FString::Printf(TEXT("%s: %s"), *Name, *FString::Join(Calls, TEXT(", ")));
		}

		FLinearColor ColorFor(const UEdGraphNode* Root, const UBeautifySettings& Settings)
		{
			if (Root->IsA<UK2Node_CustomEvent>())
			{
				return Settings.CustomEventColor;
			}
			// ponytail: matching the class name covers InputAction, InputKey, InputAxis and Enhanced Input
			// event nodes without depending on their modules. Switch to IsA checks if one gets misfiled.
			if (Root->GetClass()->GetName().Contains(TEXT("Input")))
			{
				return Settings.InputEventColor;
			}
			if (Root->IsA<UK2Node_Event>())
			{
				return Settings.EngineEventColor;
			}
			if (Root->IsA<UK2Node_FunctionEntry>() || Root->IsA<UK2Node_Tunnel>())
			{
				return Settings.FunctionColor;
			}
			return Settings.OtherColor;
		}
	}

	bool IsGeneratedComment(const UEdGraphNode_Comment* Comment)
	{
		FMetaData& MetaData = Comment->GetPackage()->GetMetaData();
		return MetaData.HasValue(Comment, GeneratedKey) && MetaData.GetValue(Comment, GeneratedKey) == Comment->NodeComment;
	}

	TArray<FUserComment> FindUserComments(const UEdGraph& Graph, const TMap<UEdGraphNode*, FVector2f>& Sizes)
	{
		TArray<FUserComment> Result;
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
			if (!Comment || IsGeneratedComment(Comment))
			{
				continue;
			}
			const FBox2f Area(Comment->GetPosition(), Comment->GetPosition() + FVector2f(Comment->NodeWidth, Comment->NodeHeight));
			FUserComment& Entry = Result.Add_GetRef({ Comment, {} });
			for (const TPair<UEdGraphNode*, FVector2f>& Pair : Sizes)
			{
				if (Area.IsInside(FBox2f(Pair.Key->GetPosition(), Pair.Key->GetPosition() + Pair.Value)))
				{
					Entry.Members.Add(Pair.Key);
				}
			}
		}
		return Result;
	}

	void UpdateComments(UEdGraph& Graph, const TArray<FChain>& Chains, const TArray<FUserComment>& UserComments,
		const TMap<UEdGraphNode*, FVector2f>& Sizes, const TMap<FString, FString>* Titles)
	{
		const UBeautifySettings& Settings = *GetDefault<UBeautifySettings>();
		Graph.Modify();

		// The user's comments follow their nodes. A chain whose event the user already boxed doesn't get a second box.
		TSet<UEdGraphNode*> UserBoxed;
		for (const FUserComment& User : UserComments)
		{
			if (!User.Members.IsEmpty())
			{
				User.Comment->Modify();
				User.Comment->SetBounds(CommentRect(BoundsOf(User.Members, Sizes), User.Comment->GetFontSize()));
				UserBoxed.Append(User.Members);
			}
		}

		// Replace what an earlier run generated, even when boxes are now switched off.
		TArray<UEdGraphNode_Comment*> Stale;
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
			if (Comment && IsGeneratedComment(Comment))
			{
				Stale.Add(Comment);
			}
		}
		for (UEdGraphNode_Comment* Comment : Stale)
		{
			Comment->Modify();
			Graph.RemoveNode(Comment);
		}

		if (!Settings.bAddCommentBoxes)
		{
			return;
		}
		for (const FChain& Chain : Chains)
		{
			if (!GetsCommentBox(Chain) || UserBoxed.Contains(Chain.Root))
			{
				continue;
			}
			const FString Title = TitleFor(Chain, Titles);
			FGraphNodeCreator<UEdGraphNode_Comment> Creator(Graph);
			UEdGraphNode_Comment* Comment = Creator.CreateNode(/*bSelectNewNode*/ false);
			Creator.Finalize();
			Comment->NodeComment = Title;
			Comment->CommentColor = ColorFor(Chain.Root, Settings);
			Comment->SetBounds(CommentRect(BoundsOf(Chain.Nodes, Sizes), Comment->GetFontSize()));
			Comment->GetPackage()->GetMetaData().SetValue(Comment, GeneratedKey, *Title);
		}
	}
}
