#include "BeautifyLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BeautifyComments.h"
#include "BeautifyLibrary.h"
#include "Engine/Engine.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "HAL/PlatformTime.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_Composite.h"
#include "K2Node_Knot.h"
#include "BeautifySettings.h"
#include "Layout/SlateRect.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace
{
	template <typename T>
	T* AddNode(UEdGraph& Graph, TFunctionRef<void(T&)> Setup = [](T&) {})
	{
		FGraphNodeCreator<T> Creator(Graph);
		T* Node = Creator.CreateNode();
		Setup(*Node);
		Creator.Finalize();
		return Node;
	}

	UK2Node_CallFunction* AddCall(UEdGraph& Graph, UClass* Library, FName Function)
	{
		return AddNode<UK2Node_CallFunction>(Graph, [Library, Function](UK2Node_CallFunction& Node)
		{
			Node.SetFromFunction(Library->FindFunctionByName(Function));
		});
	}

	UEdGraph& NewEventGraph(const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, MakeUniqueObjectName(Package, UBlueprint::StaticClass(), Name),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		return *FBlueprintEditorUtils::FindEventGraph(Blueprint);
	}

	/** What MCP tools produce: every node on one line. */
	void PutOnOneLine(UEdGraph& Graph)
	{
		for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
		{
			Graph.Nodes[Index]->SetPosition(FVector2f(Index * 10.f, 0.f));
		}
	}

	FBox2f EstimatedRect(const UEdGraphNode* Node)
	{
		return FBox2f(Node->GetPosition(), Node->GetPosition() + BeautifyBP::EstimateNodeSize(Node));
	}

	bool IsLeftOf(const UEdGraphNode* Left, const UEdGraphNode* Right)
	{
		return EstimatedRect(Left).Max.X < EstimatedRect(Right).Min.X;
	}

	int32 CountOverlaps(const UEdGraph& Graph)
	{
		TArray<FBox2f> Rects;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (!Node->IsA<UEdGraphNode_Comment>())
			{
				Rects.Add(EstimatedRect(Node));
			}
		}
		int32 Overlaps = 0;
		for (int32 A = 0; A < Rects.Num(); ++A)
		{
			for (int32 B = A + 1; B < Rects.Num(); ++B)
			{
				Overlaps += Rects[A].Intersect(Rects[B]) ? 1 : 0;
			}
		}
		return Overlaps;
	}

	TArray<UEdGraphNode_Comment*> GeneratedComments(const UEdGraph& Graph)
	{
		TArray<UEdGraphNode_Comment*> Result;
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
			if (Comment && BeautifyBP::IsGeneratedComment(Comment))
			{
				Result.Add(Comment);
			}
		}
		return Result;
	}

	FBox2f CommentArea(const UEdGraphNode_Comment* Comment)
	{
		return FBox2f(Comment->GetPosition(), Comment->GetPosition() + FVector2f(Comment->NodeWidth, Comment->NodeHeight));
	}

	/** Every wire in the graph as "node.pin->node.pin"; Beautify must leave this set untouched. */
	TSet<FString> Wires(const UEdGraph& Graph)
	{
		TSet<FString> Result;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					Result.Add(FString::Printf(TEXT("%s.%s->%s.%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString(),
						*Linked->GetOwningNode()->NodeGuid.ToString(), *Linked->PinName.ToString()));
				}
			}
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeautifyBPLayoutTest, "BeautifyBP.Layout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeautifyBPLayoutTest::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(TEXT("/Temp/BeautifyBPLayoutTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, MakeUniqueObjectName(Package, UBlueprint::StaticClass(), TEXT("BP_BeautifyTest")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	UEdGraph& Graph = *FBlueprintEditorUtils::FindEventGraph(Blueprint);
	const UEdGraphSchema* Schema = Graph.GetSchema();

	// Event -> Sequence -> [Then 0] PrintA -> Branch -> True: PrintB -> PrintD, False: PrintC -> PrintD
	//                   -> [Then 1] PrintE
	// Pure: Add -> ToString -> PrintA and PrintE. One unused Add.
	UK2Node_CustomEvent* Event = AddNode<UK2Node_CustomEvent>(Graph, [](UK2Node_CustomEvent& Node) { Node.CustomFunctionName = TEXT("BeautifyTest"); });
	UK2Node_ExecutionSequence* Sequence = AddNode<UK2Node_ExecutionSequence>(Graph);
	UK2Node_IfThenElse* Branch = AddNode<UK2Node_IfThenElse>(Graph);
	auto Print = [&Graph] { return AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)); };
	UK2Node_CallFunction* PrintA = Print();
	UK2Node_CallFunction* PrintB = Print();
	UK2Node_CallFunction* PrintC = Print();
	UK2Node_CallFunction* PrintD = Print();
	UK2Node_CallFunction* PrintE = Print();
	UK2Node_CallFunction* Add = AddCall(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt));
	UK2Node_CallFunction* ToString = AddCall(Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString));
	UK2Node_CallFunction* Unused = AddCall(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt));

	auto Link = [this, Schema](UEdGraphPin* From, UEdGraphPin* To) { TestTrue(TEXT("Wire created"), From && To && Schema->TryCreateConnection(From, To)); };
	Link(Event->FindPinChecked(UEdGraphSchema_K2::PN_Then), Sequence->GetExecPin());
	Link(Sequence->GetThenPinGivenIndex(0), PrintA->GetExecPin());
	Link(Sequence->GetThenPinGivenIndex(1), PrintE->GetExecPin());
	Link(PrintA->GetThenPin(), Branch->GetExecPin());
	Link(Branch->GetThenPin(), PrintB->GetExecPin());
	Link(Branch->GetElsePin(), PrintC->GetExecPin());
	Link(PrintB->GetThenPin(), PrintD->GetExecPin());
	Link(PrintC->GetThenPin(), PrintD->GetExecPin());
	Link(Add->FindPinChecked(TEXT("ReturnValue")), ToString->FindPinChecked(TEXT("InInt")));
	Link(ToString->FindPinChecked(TEXT("ReturnValue")), PrintA->FindPinChecked(TEXT("InString")));
	Link(ToString->FindPinChecked(TEXT("ReturnValue")), PrintE->FindPinChecked(TEXT("InString")));

	// What MCP tools produce: every node on one line.
	TArray<UEdGraphNode*> Nodes(Graph.Nodes);
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		Nodes[Index]->SetPosition(FVector2f(Index * 10.f, 0.f));
	}
	TMap<UEdGraphNode*, FVector2f> Before;
	for (UEdGraphNode* Node : Nodes)
	{
		Before.Add(Node, Node->GetPosition());
	}
	const TSet<FString> WiresBefore = Wires(Graph);

	TestEqual(TEXT("Every node placed"), BeautifyBP::Beautify(&Graph, false), Nodes.Num());
	TestTrue(TEXT("Wires unchanged"), Wires(Graph).Difference(WiresBefore).IsEmpty() && WiresBefore.Difference(Wires(Graph)).IsEmpty());

	// No editor shows this graph, so the layout used estimated sizes; check against the same ones.
	auto Rect = [](const UEdGraphNode* Node) { return FBox2f(Node->GetPosition(), Node->GetPosition() + BeautifyBP::EstimateNodeSize(Node)); };
	for (int32 A = 0; A < Nodes.Num(); ++A)
	{
		for (int32 B = A + 1; B < Nodes.Num(); ++B)
		{
			const FBox2f RectA = Rect(Nodes[A]);
			const FBox2f RectB = Rect(Nodes[B]);
			const bool bOverlap = RectA.Min.X < RectB.Max.X && RectB.Min.X < RectA.Max.X && RectA.Min.Y < RectB.Max.Y && RectB.Min.Y < RectA.Max.Y;
			TestFalse(FString::Printf(TEXT("%s overlaps %s"), *Nodes[A]->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Nodes[B]->GetNodeTitle(ENodeTitleType::ListView).ToString()), bOverlap);
		}
	}

	auto LeftOf = [&Rect](const UEdGraphNode* Left, const UEdGraphNode* Right) { return Rect(Left).Max.X < Rect(Right).Min.X; };
	TestTrue(TEXT("Exec flows left to right"), LeftOf(Event, Sequence) && LeftOf(Sequence, PrintA) && LeftOf(Sequence, PrintE) && LeftOf(PrintA, Branch)
		&& LeftOf(Branch, PrintB) && LeftOf(Branch, PrintC) && LeftOf(PrintB, PrintD) && LeftOf(PrintC, PrintD));
	TestTrue(TEXT("Pure inputs sit left of their first consumer"), LeftOf(Add, ToString) && LeftOf(ToString, PrintA));
	TestTrue(TEXT("Branch True continues on the row, False goes below"), PrintB->GetPosition().Y < PrintC->GetPosition().Y);
	TestTrue(TEXT("Sequence Then 1 goes below Then 0's whole chain"), PrintE->GetPosition().Y > PrintC->GetPosition().Y);
	TestTrue(TEXT("Unused node placed below the event chain"), Unused->GetPosition().Y > PrintE->GetPosition().Y);

	// One box for the event chain. The three default events of a new Actor are one node each and get none.
	TArray<UEdGraphNode_Comment*> Boxes = GeneratedComments(Graph);
	TestEqual(TEXT("One comment box per chain of two or more nodes"), Boxes.Num(), 1);
	if (Boxes.Num() == 1)
	{
		const FBox2f Area = CommentArea(Boxes[0]);
		TestEqual(TEXT("Default title names the event and its calls"), Boxes[0]->NodeComment, FString(TEXT("BeautifyTest: Print String")));
		TestTrue(TEXT("Box encloses the chain and its pure inputs"), Area.IsInside(Rect(Event)) && Area.IsInside(Rect(PrintD)) && Area.IsInside(Rect(PrintE)) && Area.IsInside(Rect(Add)));
		TestFalse(TEXT("Box leaves out the unused node"), Area.Intersect(Rect(Unused)));
	}

	TestTrue(TEXT("Undo ran"), GEditor && GEditor->UndoTransaction());
	bool bRestored = true;
	for (UEdGraphNode* Node : Nodes)
	{
		bRestored &= Node->GetPosition() == Before[Node];
	}
	TestTrue(TEXT("One undo restores every position"), bRestored);
	TestEqual(TEXT("The same undo removes the box"), GeneratedComments(Graph).Num(), 0);

	// Script entry points, as an MCP pipeline would call them.
	TestEqual(TEXT("Python/Blueprint entry point places every node"), UBeautifyLibrary::BeautifyGraph(Blueprint, NAME_None), Nodes.Num());
	TestEqual(TEXT("Graph found by name"), UBeautifyLibrary::BeautifyGraph(Blueprint, Graph.GetFName()), Nodes.Num());
	TestEqual(TEXT("Re-running replaces the box instead of adding another"), GeneratedComments(Graph).Num(), 1);
	AddExpectedMessage(TEXT("has no graph named"), EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("Unknown graph places nothing"), UBeautifyLibrary::BeautifyGraph(Blueprint, TEXT("NoSuchGraph")), 0);

	// Titles from a script, keyed loosely by event name.
	UBeautifyLibrary::BeautifyGraphWithTitles(Blueprint, NAME_None, { { TEXT("Beautify Test"), TEXT("Say hello") } });
	Boxes = GeneratedComments(Graph);
	TestTrue(TEXT("Script-supplied title used"), Boxes.Num() == 1 && Boxes[0]->NodeComment == TEXT("Say hello"));

	// A box the user retitles is theirs: kept, and since it holds the event, the chain gets no second box.
	if (Boxes.Num() == 1)
	{
		UEdGraphNode_Comment* Retitled = Boxes[0];
		Retitled->NodeComment = TEXT("My section");
		UBeautifyLibrary::BeautifyGraph(Blueprint, NAME_None);
		TestTrue(TEXT("Retitled box kept as the user's"), Graph.Nodes.Contains(Retitled) && !BeautifyBP::IsGeneratedComment(Retitled));
		TestEqual(TEXT("No second box over a chain the user boxed"), GeneratedComments(Graph).Num(), 0);
		Graph.RemoveNode(Retitled);
	}

	// A user's comment follows its nodes: drag two nodes and their comment away, Beautify pulls the nodes back into the chain.
	UEdGraphNode_Comment* UserComment = AddNode<UEdGraphNode_Comment>(Graph);
	const FBox2f Around = Rect(PrintB) + Rect(PrintD);
	UserComment->SetBounds(FSlateRect(Around.Min.X - 16.f, Around.Min.Y - 48.f, Around.Max.X + 16.f, Around.Max.Y + 16.f));
	for (UEdGraphNode* Node : TArray<UEdGraphNode*>{ PrintB, PrintD, UserComment })
	{
		Node->SetPosition(Node->GetPosition() + FVector2f(4000.f, 4000.f));
	}
	UBeautifyLibrary::BeautifyGraph(Blueprint, NAME_None);
	TestTrue(TEXT("Nodes came back into the chain"), PrintB->GetPosition().X < Event->GetPosition().X + 4000.f);
	TestTrue(TEXT("User comment followed its nodes"), CommentArea(UserComment).IsInside(Rect(PrintB)) && CommentArea(UserComment).IsInside(Rect(PrintD)));
	TestFalse(TEXT("User comment not taken over"), BeautifyBP::IsGeneratedComment(UserComment));

	for (UEdGraphNode* Node : Nodes)
	{
		Node->SetPosition(Before[Node]);
	}
	GEngine->Exec(nullptr, *FString::Printf(TEXT("BeautifyBP.Format %s"), *Blueprint->GetPathName()));
	TestTrue(TEXT("Console command lays the graph out"), PrintB->GetPosition().Y < PrintC->GetPosition().Y);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeautifyBPEdgeCasesTest, "BeautifyBP.EdgeCases", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeautifyBPEdgeCasesTest::RunTest(const FString& Parameters)
{
	UEdGraph& Graph = NewEventGraph(TEXT("BP_BeautifyEdgeCases"));
	const UEdGraphSchema* Schema = Graph.GetSchema();
	auto Link = [this, Schema](UEdGraphPin* From, UEdGraphPin* To) { TestTrue(TEXT("Wire created"), From && To && Schema->TryCreateConnection(From, To)); };
	auto Print = [](UEdGraph& InGraph) { return AddCall(InGraph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)); };
	auto Event = [&Graph](const TCHAR* Name) { return AddNode<UK2Node_CustomEvent>(Graph, [Name](UK2Node_CustomEvent& Node) { Node.CustomFunctionName = Name; }); };

	// Main binds Handler to OnDestroyed. Unrelated sits between them on the line, so only the binding can pull Handler up.
	UK2Node_CustomEvent* Main = Event(TEXT("Main"));
	UK2Node_AddDelegate* Bind = AddNode<UK2Node_AddDelegate>(Graph, [](UK2Node_AddDelegate& Node)
	{
		Node.SetFromProperty(AActor::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(AActor, OnDestroyed)), false, AActor::StaticClass());
	});
	UK2Node_CustomEvent* Unrelated = Event(TEXT("Unrelated"));
	UK2Node_CallFunction* UnrelatedPrint = Print(Graph);
	UK2Node_CustomEvent* Handler = Event(TEXT("Handler"));
	UK2Node_CallFunction* HandlerPrint = Print(Graph);
	Link(Main->FindPinChecked(UEdGraphSchema_K2::PN_Then), Bind->GetExecPin());
	Link(Unrelated->FindPinChecked(UEdGraphSchema_K2::PN_Then), UnrelatedPrint->GetExecPin());
	Link(Handler->FindPinChecked(UEdGraphSchema_K2::PN_Then), HandlerPrint->GetExecPin());
	// The layout reads wires only, so skip the signature matching the editor does when you drag this one.
	Handler->FindPinChecked(UK2Node_Event::DelegateOutputName)->MakeLinkTo(Bind->GetDelegatePin());

	// A reroute between a pure node and the node it feeds.
	UK2Node_CallFunction* ToString = AddCall(Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString));
	UK2Node_Knot* Knot = AddNode<UK2Node_Knot>(Graph);
	ToString->FindPinChecked(TEXT("ReturnValue"))->MakeLinkTo(Knot->GetInputPin());
	Knot->GetOutputPin()->MakeLinkTo(HandlerPrint->FindPinChecked(TEXT("InString")));

	// A collapsed graph whose two nodes sit on top of each other.
	UK2Node_Composite* Collapsed = AddNode<UK2Node_Composite>(Graph);
	UEdGraph& Inner = *Collapsed->BoundGraph;
	UK2Node_CallFunction* InnerA = Print(Inner);
	UK2Node_CallFunction* InnerB = Print(Inner);
	Link(InnerA->GetThenPin(), InnerB->GetExecPin());

	PutOnOneLine(Graph);
	PutOnOneLine(Inner);
	TMap<UEdGraphNode*, FVector2f> Before;
	for (UEdGraph* Each : { &Graph, &Inner })
	{
		for (UEdGraphNode* Node : Each->Nodes)
		{
			Before.Add(Node, Node->GetPosition());
		}
	}

	BeautifyBP::Beautify(&Graph, false);

	TestEqual(TEXT("No overlaps"), CountOverlaps(Graph), 0);
	TestTrue(TEXT("Bound event goes right below the chain that binds it"),
		Main->GetPosition().Y < Handler->GetPosition().Y && Handler->GetPosition().Y < Unrelated->GetPosition().Y);
	TestTrue(TEXT("Reroute sits between its source and its consumer"), IsLeftOf(ToString, Knot) && IsLeftOf(Knot, HandlerPrint));
	TestTrue(TEXT("Collapsed graph tidied too"), IsLeftOf(InnerA, InnerB) && CountOverlaps(Inner) == 0);

	// The three default events of a new Actor are one node each, get no box, and so need no room for one.
	TArray<UEdGraphNode*> Defaults = Graph.Nodes.FilterByPredicate([](const UEdGraphNode* Node) { return Node->IsA<UK2Node_Event>() && !Node->IsA<UK2Node_CustomEvent>(); });
	Defaults.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodePosY < B.NodePosY; });
	TestTrue(TEXT("Found the default events"), Defaults.Num() >= 2);
	if (Defaults.Num() >= 2)
	{
		const float Gap = Defaults[1]->GetPosition().Y - EstimatedRect(Defaults[0]).Max.Y;
		TestTrue(TEXT("Unboxed chains skip the comment room"), Gap < BeautifyBP::CommentPad + BeautifyBP::CommentTitleHeight + 128.f);
	}

	TestTrue(TEXT("Undo ran"), GEditor && GEditor->UndoTransaction());
	bool bRestored = true;
	for (const TPair<UEdGraphNode*, FVector2f>& Pair : Before)
	{
		bRestored &= Pair.Key->GetPosition() == Pair.Value;
	}
	TestTrue(TEXT("One undo restores the graph and the collapsed graph"), bRestored);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeautifyBPOtherGraphTypeTest, "BeautifyBP.OtherGraphType", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeautifyBPOtherGraphTypeTest::RunTest(const FString& Parameters)
{
	// A SimFlow-style graph without needing SimFlow: plain nodes with flow pins only, on a non-Blueprint schema.
	// The material schema just stands in for "some other graph type"; nothing material-specific runs.
	UPackage* Package = CreatePackage(TEXT("/Temp/BeautifyBPOtherGraph"));
	UEdGraph* Graph = NewObject<UEdGraph>(Package, NAME_None, RF_Transactional);
	Graph->Schema = UMaterialGraphSchema::StaticClass();

	auto Step = [Graph](int32 Outputs)
	{
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NAME_None, RF_Transactional);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->CreatePin(EGPD_Input, TEXT("flow"), TEXT("In"));
		for (int32 Index = 0; Index < Outputs; ++Index)
		{
			Node->CreatePin(EGPD_Output, TEXT("flow"), *FString::Printf(TEXT("Out%d"), Index));
		}
		return Node;
	};
	auto Link = [](UEdGraphNode* From, int32 Output, UEdGraphNode* To)
	{
		From->FindPinChecked(*FString::Printf(TEXT("Out%d"), Output))->MakeLinkTo(To->FindPinChecked(TEXT("In")));
	};

	// Start -> A -> (Out0) B, (Out1) C. Second start -> D -> B. B loops back to A. E is connected to nothing.
	UEdGraphNode* Start = Step(1);
	UEdGraphNode* A = Step(2);
	UEdGraphNode* B = Step(1);
	UEdGraphNode* C = Step(1);
	UEdGraphNode* SecondStart = Step(1);
	UEdGraphNode* D = Step(1);
	UEdGraphNode* E = Step(1);
	Start->FindPinChecked(TEXT("In"))->bHidden = true;
	SecondStart->FindPinChecked(TEXT("In"))->bHidden = true;
	Link(Start, 0, A);
	Link(A, 0, B);
	Link(A, 1, C);
	Link(SecondStart, 0, D);
	Link(D, 0, B);
	Link(B, 0, A);
	PutOnOneLine(*Graph);
	TMap<UEdGraphNode*, FVector2f> Before;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Before.Add(Node, Node->GetPosition());
	}

	TArray<TSoftClassPtr<UEdGraphSchema>>& Listed = GetMutableDefault<UBeautifySettings>()->OtherGraphSchemas;
	const TArray<TSoftClassPtr<UEdGraphSchema>> Saved = Listed;
	Listed.Reset();
	TestFalse(TEXT("An unlisted graph type is left alone"), BeautifyBP::CanBeautify(Graph));
	Listed.Add(UMaterialGraphSchema::StaticClass());
	TestTrue(TEXT("A listed graph type may be arranged"), BeautifyBP::CanBeautify(Graph));

	TestEqual(TEXT("Every node placed"), BeautifyBP::Beautify(Graph, false), 7);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*Graph), 0);
	TestTrue(TEXT("Flow runs left to right, the loop back to A aside"), IsLeftOf(Start, A) && IsLeftOf(A, B) && IsLeftOf(A, C) && IsLeftOf(SecondStart, D) && IsLeftOf(D, B));
	TestTrue(TEXT("First output continues on the row, the second goes below"), B->GetPosition().Y < C->GetPosition().Y);
	TestTrue(TEXT("Chains get comment boxes here too"), GeneratedComments(*Graph).Num() >= 1);

	TestTrue(TEXT("Undo ran"), GEditor && GEditor->UndoTransaction());
	bool bRestored = true;
	for (const TPair<UEdGraphNode*, FVector2f>& Pair : Before)
	{
		bRestored &= Pair.Key->GetPosition() == Pair.Value;
	}
	TestTrue(TEXT("One undo restores the graph"), bRestored);

	Listed = Saved;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeautifyBPLargeGraphTest, "BeautifyBP.LargeGraph", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeautifyBPLargeGraphTest::RunTest(const FString& Parameters)
{
	// One chain 1500 calls long, each with a pure input: about 3000 nodes, and recursion 1500 deep.
	constexpr int32 Length = 1500;
	UEdGraph& Graph = NewEventGraph(TEXT("BP_BeautifyLarge"));
	const UEdGraphSchema* Schema = Graph.GetSchema();
	UK2Node_CustomEvent* Event = AddNode<UK2Node_CustomEvent>(Graph, [](UK2Node_CustomEvent& Node) { Node.CustomFunctionName = TEXT("Long"); });
	UEdGraphPin* Then = Event->FindPinChecked(UEdGraphSchema_K2::PN_Then);
	TArray<UK2Node_CallFunction*> Prints;
	for (int32 Index = 0; Index < Length; ++Index)
	{
		UK2Node_CallFunction* PrintNode = AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
		UK2Node_CallFunction* ToString = AddCall(Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString));
		Schema->TryCreateConnection(Then, PrintNode->GetExecPin());
		Schema->TryCreateConnection(ToString->FindPinChecked(TEXT("ReturnValue")), PrintNode->FindPinChecked(TEXT("InString")));
		Then = PrintNode->GetThenPin();
		Prints.Add(PrintNode);
	}
	PutOnOneLine(Graph);

	const double Start = FPlatformTime::Seconds();
	BeautifyBP::Beautify(&Graph, false);
	const double Seconds = FPlatformTime::Seconds() - Start;
	AddInfo(FString::Printf(TEXT("Beautified %d nodes in %.2f s"), Graph.Nodes.Num(), Seconds));

	TestTrue(TEXT("Under 5 seconds"), Seconds < 5.0);
	bool bLeftToRight = IsLeftOf(Event, Prints[0]);
	for (int32 Index = 1; Index < Prints.Num(); ++Index)
	{
		bLeftToRight &= IsLeftOf(Prints[Index - 1], Prints[Index]);
	}
	TestTrue(TEXT("Exec runs left to right along the whole chain"), bLeftToRight);
	TestEqual(TEXT("No overlaps"), CountOverlaps(Graph), 0);
	return true;
}

#endif
