// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadDamageable.h"
#include "MadGameplayDefinitions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadZombieResistanceTest,
	"MadFall.AI.ZombieResistances",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadZombieResistanceTest::RunTest(const FString& Parameters)
{
	// Parsing: multipliers by type, errors on nonsense.
	{
		TArray<FMadDefinitionError> Errors;
		const FString Json = TEXT(R"({"schema":"madfall.zombie/1","id":"test:armoured","resistances":{"madfall:pierce":0.5,"madfall:fire":1.5}})");
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		FMadZombieDefinition Zombie;
		TestTrue(TEXT("zombie with resistances parses"), MadFall::GameplayDefinitionsJson::ParseZombie(Object.ToSharedRef(), TEXT("z.json"), FName(TEXT("test")), Zombie, Errors));
		TestEqual(TEXT("no errors"), Errors.Num(), 0);
		TestEqual(TEXT("pierce halved"), Zombie.GetDamageMultiplier(FName(TEXT("madfall:pierce"))), 0.5f);
		TestEqual(TEXT("weak to fire"), Zombie.GetDamageMultiplier(FName(TEXT("madfall:fire"))), 1.5f);
		TestEqual(TEXT("unlisted types get through"), Zombie.GetDamageMultiplier(FName(TEXT("madfall:blunt"))), 1.0f);

		const FString Bad = TEXT(R"({"schema":"madfall.zombie/1","id":"test:bad","resistances":{"madfall:pierce":-1,"madfall:fire":"lots"}})");
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Bad), Object);
		FMadZombieDefinition BadZombie;
		TArray<FMadDefinitionError> BadErrors;
		MadFall::GameplayDefinitionsJson::ParseZombie(Object.ToSharedRef(), TEXT("bad.json"), FName(TEXT("test")), BadZombie, BadErrors);
		TestEqual(TEXT("a negative and a string multiplier are both errors"), BadErrors.Num(), 2);
	}

	// Choosing the damage type a weapon deals best.
	{
		TMap<FName, float> Spear;
		Spear.Add(FName(TEXT("madfall:blunt")), 20.0f);
		Spear.Add(FName(TEXT("madfall:pierce")), 30.0f);
		FName Type;
		float Amount = MadFall::Combat::ChooseDamage(Spear, [](FName) { return 1.0f; }, Type);
		TestEqual(TEXT("with no resistance the bigger number wins"), Type, FName(TEXT("madfall:pierce")));
		TestEqual(TEXT("its amount, before resistance"), Amount, 30.0f);

		Amount = MadFall::Combat::ChooseDamage(Spear, [](FName T) { return T == FName(TEXT("madfall:pierce")) ? 0.5f : 1.0f; }, Type);
		TestEqual(TEXT("armour against pierce makes blunt the better hit (20 > 15)"), Type, FName(TEXT("madfall:blunt")));
		TestEqual(TEXT("and the amount is blunt's"), Amount, 20.0f);

		TMap<FName, float> Nothing;
		FName Unchanged(TEXT("madfall:blunt"));
		TestEqual(TEXT("no damage entries deal nothing"), MadFall::Combat::ChooseDamage(Nothing, [](FName) { return 1.0f; }, Unchanged), 0.0f);
	}

	// The shipped armoured zombies resist.
	{
		const FMadGameplayDefinitions& Shipped = MadFall::GetGameplayDefinitions();
		if (const FMadZombieDefinition* Soldier = Shipped.FindZombie(FName(TEXT("madfall:zombie_soldier"))))
		{
			TestTrue(TEXT("a soldier's armour stops arrows"), Soldier->GetDamageMultiplier(FName(TEXT("madfall:pierce"))) < 1.0f);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadZombieAbilityTest,
	"MadFall.AI.ZombieAbilities",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadZombieAbilityTest::RunTest(const FString& Parameters)
{
	auto Json = [](const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	};

	FMadZombieDefinition Plain;
	TArray<FMadDefinitionError> Errors;
	MadFall::GameplayDefinitionsJson::ParseZombie(Json(TEXT(R"({ "schema": "madfall.zombie/1", "id": "test:shambler" })")), TEXT("t"), FName(TEXT("test")), Plain, Errors);
	TestFalse(TEXT("a zombie without abilities has no ranged attack"), Plain.HasRanged());
	TestFalse(TEXT("and no scream"), Plain.HasScream());

	FMadZombieDefinition Both;
	MadFall::GameplayDefinitionsJson::ParseZombie(Json(TEXT(R"({ "schema": "madfall.zombie/1", "id": "test:both",
		"abilities": { "ranged": { "damage": 7, "range": 12, "seconds": 2, "speed": 30 }, "scream": { "radius": 50, "seconds": 20, "summons": 99 } } })")),
		TEXT("t"), FName(TEXT("test")), Both, Errors);
	TestEqual(TEXT("abilities parse cleanly"), Errors.Num(), 0);
	TestTrue(TEXT("ranged"), Both.HasRanged() && Both.RangedDamage == 7.0f && Both.RangedRange == 12.0f && Both.RangedSpeed == 30.0f);
	TestTrue(TEXT("scream"), Both.HasScream() && Both.ScreamRadius == 50.0f);
	TestEqual(TEXT("summons are capped"), Both.ScreamSummons, 8);

	FMadZombieDefinition Bad;
	TArray<FMadDefinitionError> BadErrors;
	MadFall::GameplayDefinitionsJson::ParseZombie(Json(TEXT(R"({ "schema": "madfall.zombie/1", "id": "test:bad",
		"abilities": { "ranged": { "damage": 5, "range": 10, "seconds": 0, "speed": 0 }, "teleport": {} } })")), TEXT("t"), FName(TEXT("test")), Bad, BadErrors);
	TestTrue(TEXT("a zero cooldown is reported"), BadErrors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("/abilities/ranged")); }));
	TestTrue(TEXT("an unknown ability is reported"), BadErrors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("teleport")); }));
	TestTrue(TEXT("and repaired"), Bad.RangedSeconds >= 0.2f && Bad.RangedSpeed > 0.0f);

	const FMadGameplayDefinitions& Defs = MadFall::GetGameplayDefinitions();
	const FMadZombieDefinition* Spitter = Defs.FindZombie(FName(TEXT("madfall:zombie_spitter")));
	const FMadZombieDefinition* Screamer = Defs.FindZombie(FName(TEXT("madfall:zombie_screamer")));
	TestTrue(TEXT("the shipped spitter spits"), Spitter != nullptr && Spitter->HasRanged() && Spitter->SpawnGroups.Contains(FName(TEXT("madfall:zombies/horde"))));
	TestTrue(TEXT("the shipped screamer screams"), Screamer != nullptr && Screamer->HasScream() && Screamer->ScreamSummons > 0);
	TestTrue(TEXT("neither turns up on the first days"), Spitter && Screamer && Spitter->MinGameStage > 0 && Screamer->MinGameStage > 0);
	return true;
}

#endif
