// Copyright MadFall. All Rights Reserved.

#include "MadModResolver.h"

#include "MadFallApiVersion.h"
#include "Misc/Crc.h"

FString FMadModResolution::Fingerprint() const
{
	FString Text;
	for (const FMadModManifest& Mod : LoadOrder)
	{
		Text += FString::Printf(TEXT("%s@%s;"), *Mod.Id.ToString(), *Mod.Version.ToString());
	}
	return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Text));
}

namespace MadFall::ModResolver
{
	namespace
	{
		auto Lexical = [](const FName& A, const FName& B) { return A.LexicalLess(B); };

		/** Tarjan's strongly connected components over the given nodes. */
		void FindCycles(const TArray<FName>& Nodes, const TMap<FName, TSet<FName>>& Edges, TArray<TArray<FName>>& OutCycles)
		{
			TMap<FName, int32> Index;
			TMap<FName, int32> LowLink;
			TSet<FName> OnStack;
			TArray<FName> Stack;
			int32 Counter = 0;

			TFunction<void(FName)> Visit = [&](FName Node)
			{
				Index.Add(Node, Counter);
				LowLink.Add(Node, Counter);
				++Counter;
				Stack.Push(Node);
				OnStack.Add(Node);

				TArray<FName> Targets = Edges[Node].Array();
				Targets.Sort(Lexical);
				for (const FName& Target : Targets)
				{
					if (!Edges.Contains(Target))
					{
						continue;
					}
					if (!Index.Contains(Target))
					{
						Visit(Target);
						LowLink[Node] = FMath::Min(LowLink[Node], LowLink[Target]);
					}
					else if (OnStack.Contains(Target))
					{
						LowLink[Node] = FMath::Min(LowLink[Node], Index[Target]);
					}
				}

				if (LowLink[Node] == Index[Node])
				{
					TArray<FName> Component;
					FName Member;
					do
					{
						Member = Stack.Pop();
						OnStack.Remove(Member);
						Component.Add(Member);
					} while (Member != Node);

					const bool bSelfLoop = Component.Num() == 1 && Edges[Node].Contains(Node);
					if (Component.Num() > 1 || bSelfLoop)
					{
						Component.Sort(Lexical);
						OutCycles.Add(MoveTemp(Component));
					}
				}
			};

			for (const FName& Node : Nodes)
			{
				if (!Index.Contains(Node))
				{
					Visit(Node);
				}
			}
		}
	}

	FMadModResolution Resolve(const TArray<FMadModManifest>& Discovered, const TSet<FName>& DisabledByUser)
	{
		FMadModResolution Result;

		auto Fail = [&Result](FName Id, const FString& Message)
		{
			Result.Status.Add(Id, EMadModStatus::Failed);
			Result.Issues.Add(FMadModIssue{ Id, true, Message });
		};

		// --- identity -------------------------------------------------------------
		TMap<FName, const FMadModManifest*> ById;
		for (const FMadModManifest& Mod : Discovered)
		{
			if (const FMadModManifest* const* Existing = ById.Find(Mod.Id))
			{
				Result.Issues.Add(FMadModIssue{ Mod.Id, false, FString::Printf(
					TEXT("installed twice (%s and %s); only the first is used"), *(*Existing)->Directory, *Mod.Directory) });
				continue;
			}
			ById.Add(Mod.Id, &Mod);

			if (DisabledByUser.Contains(Mod.Id))
			{
				Result.Status.Add(Mod.Id, EMadModStatus::DisabledByUser);
			}
			else if (!MadFall::ModApi::IsCompatible(Mod.ApiMajor, Mod.ApiMinor))
			{
				Fail(Mod.Id, FString::Printf(TEXT("written for mod API %d.%d, but this game provides %s"),
					Mod.ApiMajor, Mod.ApiMinor, *MadFall::ModApi::GetVersionString()));
			}
			else
			{
				Result.Status.Add(Mod.Id, EMadModStatus::Enabled);
			}
		}

		// Every pass below walks ids in this order, so which of two mutually
		// incompatible mods fails never depends on hash-map iteration order.
		TArray<FName> AllIds;
		ById.GenerateKeyArray(AllIds);
		AllIds.Sort(Lexical);

		while (true)
		{
			// --- dependencies and incompatibilities, to a fixed point -----------------
			for (bool bChanged = true; bChanged;)
			{
				bChanged = false;
				for (const FName& Id : AllIds)
				{
					const FMadModManifest& Mod = *ById[Id];
					if (!Result.IsEnabled(Id))
					{
						continue;
					}

					for (const FMadModDependency& Dependency : Mod.Dependencies)
					{
						const FMadModManifest* const* Target = ById.Find(Dependency.ModId);
						if (Target == nullptr || !Result.IsEnabled(Dependency.ModId))
						{
							if (!Dependency.bOptional)
							{
								const EMadModStatus* TargetStatus = Result.Status.Find(Dependency.ModId);
								const TCHAR* Why = Target == nullptr ? TEXT("is not installed")
									: (TargetStatus && *TargetStatus == EMadModStatus::DisabledByUser ? TEXT("is disabled") : TEXT("failed to load"));
								Fail(Id, FString::Printf(TEXT("requires %s, which %s"), *Dependency.ModId.ToString(), Why));
								bChanged = true;
								break;
							}
							continue;
						}

						if (!Dependency.Version.IsSatisfiedBy((*Target)->Version))
						{
							if (Dependency.bOptional)
							{
								Result.Issues.Add(FMadModIssue{ Id, false, FString::Printf(
									TEXT("optional dependency %s %s does not satisfy \"%s\""),
									*Dependency.ModId.ToString(), *(*Target)->Version.ToString(), *Dependency.Version.Source) });
								continue;
							}
							Fail(Id, FString::Printf(TEXT("requires %s \"%s\", but %s is installed"),
								*Dependency.ModId.ToString(), *Dependency.Version.Source, *(*Target)->Version.ToString()));
							bChanged = true;
							break;
						}
					}

					if (!Result.IsEnabled(Id))
					{
						continue;
					}

					for (const FName& Other : Mod.Incompatible)
					{
						if (Result.IsEnabled(Other))
						{
							Fail(Id, FString::Printf(TEXT("declares itself incompatible with %s, which is enabled"), *Other.ToString()));
							bChanged = true;
							break;
						}
					}
				}
			}

			// --- order ---------------------------------------------------------------
			TArray<FName> Enabled = AllIds.FilterByPredicate([&Result](const FName& Id) { return Result.IsEnabled(Id); });

			TMap<FName, TSet<FName>> Before;   // edge A -> B: A loads before B
			TMap<FName, int32> InDegree;
			for (const FName& Id : Enabled)
			{
				Before.Add(Id);
				InDegree.Add(Id, 0);
			}

			auto AddEdge = [&](FName First, FName Second)
			{
				// A mod naming itself in load_after/load_before is a harmless typo, not a cycle.
				if (First == Second || !Before.Contains(First) || !Before.Contains(Second))
				{
					return;
				}
				bool bAlready = false;
				Before[First].Add(Second, &bAlready);
				if (!bAlready)
				{
					++InDegree[Second];
				}
			};

			for (const FName& Id : Enabled)
			{
				const FMadModManifest& Mod = *ById[Id];
				for (const FMadModDependency& Dependency : Mod.Dependencies) { AddEdge(Dependency.ModId, Id); }
				for (const FName& Other : Mod.LoadAfter) { AddEdge(Other, Id); }
				for (const FName& Other : Mod.LoadBefore) { AddEdge(Id, Other); }
			}

			// Kahn's algorithm, always taking the alphabetically smallest ready mod.
			TArray<FName> Ready = Enabled.FilterByPredicate([&InDegree](const FName& Id) { return InDegree[Id] == 0; });
			TArray<FName> Ordered;
			while (Ready.Num() > 0)
			{
				Ready.Sort(Lexical);
				const FName Next = Ready[0];
				Ready.RemoveAt(0);
				Ordered.Add(Next);

				TArray<FName> Targets = Before[Next].Array();
				Targets.Sort(Lexical);
				for (const FName& Target : Targets)
				{
					if (--InDegree[Target] == 0)
					{
						Ready.Add(Target);
					}
				}
			}

			if (Ordered.Num() == Enabled.Num())
			{
				for (const FName& Id : Ordered)
				{
					Result.LoadOrder.Add(*ById[Id]);
				}
				return Result;
			}

			// Only the mods actually on a cycle fail. Mods merely downstream of one
			// then fail through the dependency pass with an accurate reason, or
			// load normally if the edge was only a soft ordering hint.
			TArray<FName> Leftover = Enabled.FilterByPredicate([&Ordered](const FName& Id) { return !Ordered.Contains(Id); });
			TMap<FName, TSet<FName>> LeftoverEdges;
			for (const FName& Id : Leftover)
			{
				TSet<FName>& Targets = LeftoverEdges.Add(Id);
				for (const FName& Target : Before[Id])
				{
					if (Leftover.Contains(Target)) { Targets.Add(Target); }
				}
			}

			TArray<TArray<FName>> Cycles;
			FindCycles(Leftover, LeftoverEdges, Cycles);
			check(Cycles.Num() > 0);

			for (const TArray<FName>& Cycle : Cycles)
			{
				FString Names;
				for (const FName& Id : Cycle)
				{
					Names += (Names.IsEmpty() ? TEXT("") : TEXT(" -> ")) + Id.ToString();
				}
				for (const FName& Id : Cycle)
				{
					Fail(Id, FString::Printf(TEXT("load order cycle: %s"), *Names));
				}
			}
		}
	}
}
