// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadAnimal.h"
#include "MadAnimalSubsystem.h"
#include "MadBiomeRegistry.h"
#include "MadGameplayDefinitions.h"
#include "MadQuadrupedRig.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadAnimalTests
{
	TSharedRef<FJsonObject> Json(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	}

	FMadAnimalDefinition Make(EMadAnimalBehaviour Behaviour, float AttackDamage)
	{
		FMadAnimalDefinition Animal;
		Animal.Behaviour = Behaviour;
		Animal.AttackDamage = AttackDamage;
		Animal.SightRange = 20.0f;
		Animal.HearingRange = 24.0f;
		Animal.FleeRange = 10.0f;
		return Animal;
	}

	FMadAnimalSenses See(float Distance, bool bSees = true, bool bHears = false, float SinceHurt = -1.0f)
	{
		FMadAnimalSenses Senses;
		Senses.bPlayerPresent = true;
		Senses.DistanceVoxels = Distance;
		Senses.bSeesPlayer = bSees;
		Senses.bHearsPlayer = bHears;
		Senses.SecondsSinceHurt = SinceHurt;
		return Senses;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadAnimalDefinitionsTest,
	"MadFall.Animals.Definitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadAnimalDefinitionsTest::RunTest(const FString& Parameters)
{
	using namespace MadAnimalTests;

	// A full definition parses into every field.
	{
		FMadAnimalDefinition Animal;
		TArray<FMadDefinitionError> Errors;
		const bool bParsed = MadFall::GameplayDefinitionsJson::ParseAnimal(Json(TEXT(R"({
			"schema": "madfall.animal/1", "id": "test:elk", "display_name": "Elk", "tags": ["animal.game"],
			"behaviour": "defensive",
			"spawn": { "biomes": ["madfall:tundra"], "weight": 2.5, "herd": [2, 4], "active": "night" },
			"stats": { "health": 150, "walk_speed": 1.0, "run_speed": 6.0, "attack_damage": 15, "attack_seconds": 1.5, "damage_type": "madfall:pierce" },
			"senses": { "sight": 30, "hearing": 35, "flee": 14 },
			"rewards": { "experience": 40, "loot_table": "madfall:loot/deer" },
			"appearance": { "body": [140, 40, 60], "legs": 80, "neck": 50, "scale": 1.2, "tint": [1.0, 0.5, 0.0] }
		})")), TEXT("elk.json"), FName(TEXT("test")), Animal, Errors);
		TestTrue(TEXT("parses"), bParsed);
		TestEqual(TEXT("no errors"), Errors.Num(), 0);
		TestEqual(TEXT("behaviour"), Animal.Behaviour, EMadAnimalBehaviour::Defensive);
		TestEqual(TEXT("activity"), Animal.Activity, EMadAnimalActivity::Night);
		TestTrue(TEXT("biomes"), Animal.Biomes.Num() == 1 && Animal.Biomes[0] == FName(TEXT("madfall:tundra")));
		TestEqual(TEXT("herd"), FIntPoint(Animal.HerdMin, Animal.HerdMax), FIntPoint(2, 4));
		TestEqual(TEXT("health"), Animal.Health, 150.0f);
		TestEqual(TEXT("flee range"), Animal.FleeRange, 14.0f);
		TestEqual(TEXT("body"), Animal.BodySize, FVector(140.0, 40.0, 60.0));
		TestEqual(TEXT("scale"), Animal.Scale, 1.2f);
		TestEqual(TEXT("tint converts sRGB to linear"), Animal.Tint.G, MadFall::Surfaces::SRGBToLinear(0.5f), 1e-4f);
		TestTrue(TEXT("active at night"), Animal.IsActive(true));
		TestFalse(TEXT("not by day"), Animal.IsActive(false));
	}

	// Mistakes are reported with a pointer and repaired to something usable.
	{
		FMadAnimalDefinition Animal;
		TArray<FMadDefinitionError> Errors;
		MadFall::GameplayDefinitionsJson::ParseAnimal(Json(TEXT(R"({
			"schema": "madfall.animal/1", "id": "test:bad", "behaviour": "shy",
			"spawn": { "herd": [3, 1], "active": "dusk" }, "stats": { "health": 0 }, "appearance": { "body": [10, 0] }
		})")), TEXT("bad.json"), FName(TEXT("test")), Animal, Errors);
		for (const TCHAR* Pointer : { TEXT("/behaviour"), TEXT("/spawn/herd"), TEXT("/spawn/active"), TEXT("/stats/health"), TEXT("/appearance/body") })
		{
			TestTrue(*FString::Printf(TEXT("error at %s"), Pointer), Errors.ContainsByPredicate(
				[Pointer](const FMadDefinitionError& E) { return E.ToString().Contains(Pointer); }));
		}
		TestTrue(TEXT("herd repaired"), Animal.HerdMin >= 1 && Animal.HerdMax >= Animal.HerdMin);
		TestTrue(TEXT("health repaired"), Animal.Health > 0.0f);
	}

	// Shipped animals: every biome they name exists, every loot table resolves,
	// and every land biome has something living in it by day and by night.
	const FMadGameplayDefinitions& Defs = MadFall::GetGameplayDefinitions();
	const FMadBiomeRegistry& Biomes = UMadVoxelWorldSubsystem::GetBiomeRegistry();
	TestTrue(TEXT("four shipped animals"), Defs.GetAnimals().Num() >= 4);
	for (const FMadAnimalDefinition& Animal : Defs.GetAnimals())
	{
		TestFalse(*FString::Printf(TEXT("%s spawns somewhere"), *Animal.Id.ToString()), Animal.Biomes.IsEmpty());
		for (const FName& Biome : Animal.Biomes)
		{
			TestTrue(*FString::Printf(TEXT("%s's biome %s exists"), *Animal.Id.ToString(), *Biome.ToString()), Biomes.FindIndex(Biome) != INDEX_NONE);
		}
		TestTrue(*FString::Printf(TEXT("%s drops something"), *Animal.Id.ToString()),
			!Animal.LootTable.IsNone() && Defs.FindLootTable(Animal.LootTable) != nullptr);
	}
	for (const TCHAR* Biome : { TEXT("madfall:plains"), TEXT("madfall:forest"), TEXT("madfall:desert"), TEXT("madfall:tundra"), TEXT("madfall:highlands") })
	{
		for (const bool bNight : { false, true })
		{
			TArray<const FMadAnimalDefinition*> Candidates;
			UMadAnimalSubsystem::GetCandidates(Defs.GetAnimals(), FName(Biome), bNight, Candidates);
			TestTrue(*FString::Printf(TEXT("%s has wildlife by %s"), Biome, bNight ? TEXT("night") : TEXT("day")), Candidates.Num() > 0);
		}
	}

	TArray<const FMadAnimalDefinition*> Plains;
	UMadAnimalSubsystem::GetCandidates(Defs.GetAnimals(), FName(TEXT("madfall:plains")), false, Plains);
	TestFalse(TEXT("wolves stay in the forest by day"), Plains.ContainsByPredicate(
		[](const FMadAnimalDefinition* A) { return A->Id == FName(TEXT("madfall:wolf")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadAnimalBehaviourTest,
	"MadFall.Animals.Behaviour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadAnimalBehaviourTest::RunTest(const FString& Parameters)
{
	using namespace MadAnimalTests;
	using MadFall::Animals::Decide;

	const FMadAnimalDefinition Deer = Make(EMadAnimalBehaviour::Skittish, 0.0f);
	TestEqual(TEXT("no survivor: graze"), Decide(Deer, FMadAnimalSenses()), EMadAnimalState::Graze);
	TestEqual(TEXT("seen far off: keeps grazing"), Decide(Deer, See(15.0f)), EMadAnimalState::Graze);
	TestEqual(TEXT("seen close: bolts"), Decide(Deer, See(8.0f)), EMadAnimalState::Flee);
	TestEqual(TEXT("close but unseen and quiet: a stalker gets near"), Decide(Deer, See(5.0f, false)), EMadAnimalState::Graze);
	TestEqual(TEXT("heard sprinting: bolts"), Decide(Deer, See(20.0f, false, true)), EMadAnimalState::Flee);
	TestEqual(TEXT("hurt: runs even from far"), Decide(Deer, See(30.0f, false, false, 2.0f)), EMadAnimalState::Flee);
	TestEqual(TEXT("hurt long ago: calm"), Decide(Deer, See(30.0f, false, false, 60.0f)), EMadAnimalState::Graze);

	const FMadAnimalDefinition Boar = Make(EMadAnimalBehaviour::Defensive, 12.0f);
	TestEqual(TEXT("boar ignores a survivor it sees"), Decide(Boar, See(4.0f)), EMadAnimalState::Graze);
	TestEqual(TEXT("hurt boar charges"), Decide(Boar, See(6.0f, true, false, 1.0f)), EMadAnimalState::Chase);
	TestEqual(TEXT("and gores within reach"), Decide(Boar, See(1.0f, true, false, 1.0f)), EMadAnimalState::Attack);
	TestEqual(TEXT("gives up once the survivor is far away"), Decide(Boar, See(40.0f, false, false, 1.0f)), EMadAnimalState::Graze);

	const FMadAnimalDefinition Wolf = Make(EMadAnimalBehaviour::Aggressive, 9.0f);
	TestEqual(TEXT("wolf hunts a survivor it sees"), Decide(Wolf, See(15.0f)), EMadAnimalState::Chase);
	TestEqual(TEXT("not one it cannot see"), Decide(Wolf, See(15.0f, false)), EMadAnimalState::Graze);
	TestEqual(TEXT("wolf bites within reach"), Decide(Wolf, See(1.2f)), EMadAnimalState::Attack);

	const FMadAnimalDefinition Harmless = Make(EMadAnimalBehaviour::Aggressive, 0.0f);
	TestEqual(TEXT("an 'aggressive' animal with no attack runs instead"), Decide(Harmless, See(3.0f)), EMadAnimalState::Flee);

	// Fleeing heads away from the threat, the requested distance, whatever the jitter.
	const FIntVector Feet(10, 10, 20);
	for (const float Jitter : { -1.0f, 0.0f, 1.0f })
	{
		const FIntVector Goal = MadFall::Animals::PickFleeGoal(Feet, FIntVector(5, 10, 20), 12, Jitter);
		TestTrue(*FString::Printf(TEXT("jitter %.0f flees east, away from a threat to the west"), Jitter), Goal.X > Feet.X + 8);
		TestEqual(*FString::Printf(TEXT("jitter %.0f stays on the same level"), Jitter), Goal.Z, Feet.Z);
		const float Distance = FVector2D(Goal.X - Feet.X, Goal.Y - Feet.Y).Size();
		TestTrue(*FString::Printf(TEXT("jitter %.0f runs about 12 voxels (%.1f)"), Jitter, Distance), Distance > 10.5f && Distance < 13.5f);
	}
	const FIntVector OnTop = MadFall::Animals::PickFleeGoal(Feet, Feet, 12, 0.0f);
	TestTrue(TEXT("a threat on the same voxel still gives a goal"), OnTop != Feet);

	// Weighted picks cover every candidate in proportion.
	FMadAnimalDefinition A = Make(EMadAnimalBehaviour::Skittish, 0.0f);
	FMadAnimalDefinition B = A;
	A.SpawnWeight = 3.0f;
	B.SpawnWeight = 1.0f;
	const TArray<const FMadAnimalDefinition*> Candidates = { &A, &B };
	TestTrue(TEXT("low roll picks the heavy one"), UMadAnimalSubsystem::PickWeighted(Candidates, 0.1f) == &A);
	TestTrue(TEXT("high roll picks the light one"), UMadAnimalSubsystem::PickWeighted(Candidates, 0.9f) == &B);
	TestNull(TEXT("no candidates, nothing"), UMadAnimalSubsystem::PickWeighted({}, 0.5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadQuadrupedPoseTest,
	"MadFall.Animals.QuadrupedPose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadQuadrupedPoseTest::RunTest(const FString& Parameters)
{
	using MadFall::Quadruped::ComputePose;

	const FMadQuadrupedPose Standing = ComputePose(1.3f, 0.0f, 0.0f, 1.0f, 0.0f);
	TestEqual(TEXT("standing legs are straight"), Standing.FrontLeft + FMath::Abs(Standing.BackRight), 0.0f, 1e-3f);
	TestEqual(TEXT("standing does not bob"), Standing.Bob, 0.0f, 1e-3f);

	const FMadQuadrupedPose Trot = ComputePose(UE_HALF_PI, 0.5f, 0.0f, 1.0f, 0.0f);
	TestTrue(TEXT("a moving leg swings"), FMath::Abs(Trot.FrontLeft) > 10.0f);
	TestEqual(TEXT("diagonal legs move together"), Trot.FrontLeft, Trot.BackRight, 1e-3f);
	TestEqual(TEXT("the other pair opposes them"), Trot.FrontRight, -Trot.FrontLeft, 1e-3f);
	TestTrue(TEXT("a gallop swings wider than a trot"), FMath::Abs(ComputePose(UE_HALF_PI, 1.0f, 0.0f, 1.0f, 0.0f).FrontLeft) > FMath::Abs(Trot.FrontLeft));

	TestTrue(TEXT("grazing lowers the head when still"), ComputePose(0.0f, 0.0f, 1.0f, 1.0f, 0.0f).Head < -40.0f);
	TestTrue(TEXT("but not at a run"), ComputePose(0.0f, 1.0f, 1.0f, 1.0f, 0.0f).Head > -10.0f);
	TestTrue(TEXT("an attack drops the head mid-butt"), ComputePose(0.0f, 0.0f, 0.0f, 0.5f, 0.0f).Head < -30.0f);
	TestEqual(TEXT("dead animals lie on their side"), ComputePose(0.0f, 0.0f, 0.0f, 1.0f, 1.0f).Fall, 90.0f, 1e-3f);
	return true;
}

#endif
