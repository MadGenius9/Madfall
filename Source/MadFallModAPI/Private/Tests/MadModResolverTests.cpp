// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadFallApiVersion.h"
#include "MadModManifest.h"
#include "MadModResolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadModTests
{
	FMadModManifest Mod(const TCHAR* Id, const TCHAR* Version = TEXT("1.0.0"))
	{
		FMadModManifest Manifest;
		Manifest.Id = FName(Id);
		FMadSemanticVersion::Parse(Version, Manifest.Version);
		Manifest.ApiMajor = MadFall::ModApi::VersionMajor;
		Manifest.ApiMinor = MadFall::ModApi::VersionMinor;
		Manifest.Directory = FString::Printf(TEXT("Mods/%s"), Id);
		return Manifest;
	}

	void Require(FMadModManifest& Manifest, const TCHAR* Id, const TCHAR* Constraint = TEXT("*"), bool bOptional = false)
	{
		FMadModDependency& Dependency = Manifest.Dependencies.AddDefaulted_GetRef();
		Dependency.ModId = FName(Id);
		Dependency.bOptional = bOptional;
		FString Error;
		FMadVersionConstraint::Parse(Constraint, Dependency.Version, Error);
	}

	FString OrderString(const FMadModResolution& Result)
	{
		FString Out;
		for (const FMadModManifest& Manifest : Result.LoadOrder)
		{
			Out += (Out.IsEmpty() ? TEXT("") : TEXT(",")) + Manifest.Id.ToString();
		}
		return Out;
	}

	bool HasIssue(const FMadModResolution& Result, const TCHAR* Id, const TCHAR* Needle)
	{
		return Result.Issues.ContainsByPredicate([Id, Needle](const FMadModIssue& Issue)
		{
			return Issue.ModId == FName(Id) && Issue.Message.Contains(Needle);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadModVersionTest,
	"MadFall.Mods.Versions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadModVersionTest::RunTest(const FString& Parameters)
{
	auto Satisfies = [](const TCHAR* Constraint, const TCHAR* Version)
	{
		FMadVersionConstraint C;
		FString Error;
		FMadSemanticVersion V;
		return FMadVersionConstraint::Parse(Constraint, C, Error) && FMadSemanticVersion::Parse(Version, V) && C.IsSatisfiedBy(V);
	};

	TestTrue(TEXT("* matches anything"), Satisfies(TEXT("*"), TEXT("9.9.9")));
	TestTrue(TEXT("exact"), Satisfies(TEXT("1.4.2"), TEXT("1.4.2")));
	TestFalse(TEXT("exact mismatch"), Satisfies(TEXT("1.4.2"), TEXT("1.4.3")));
	TestTrue(TEXT(">= with missing parts"), Satisfies(TEXT(">=1.2"), TEXT("1.2.0")));
	TestTrue(TEXT("range"), Satisfies(TEXT(">=1.2, <1.5"), TEXT("1.4.9")));
	TestFalse(TEXT("range upper bound"), Satisfies(TEXT(">=1.2, <1.5"), TEXT("1.5.0")));
	TestTrue(TEXT("caret within major"), Satisfies(TEXT("^1.2.0"), TEXT("1.9.0")));
	TestFalse(TEXT("caret excludes next major"), Satisfies(TEXT("^1.2.0"), TEXT("2.0.0")));
	TestFalse(TEXT("caret on 0.x is minor-locked"), Satisfies(TEXT("^0.2.0"), TEXT("0.3.0")));
	TestTrue(TEXT("tilde within minor"), Satisfies(TEXT("~1.2.0"), TEXT("1.2.7")));
	TestFalse(TEXT("tilde excludes next minor"), Satisfies(TEXT("~1.2.0"), TEXT("1.3.0")));
	TestTrue(TEXT("pre-release suffix ignored"), Satisfies(TEXT(">=2.0.0"), TEXT("2.0.0-beta.3")));

	FMadVersionConstraint Bad;
	FString Error;
	TestFalse(TEXT("garbage rejected"), FMadVersionConstraint::Parse(TEXT(">=one"), Bad, Error));

	// --- manifests -------------------------------------------------------------
	FMadModManifest Manifest;
	TArray<FString> Errors;
	TestTrue(TEXT("valid manifest"), FMadModManifest::ParseText(TEXT(R"({
		"schema": "madfall.mod/1", "id": "better_bases", "version": "2.1.0", "api_version": "0.1",
		"name": "Better Bases", "dependencies": [{ "id": "steel_pack", "version": "^1.0.0" }, { "id": "lights", "optional": true }],
		"load_after": ["ui_tweaks"], "paks": ["Content/better_bases.pak"]
	})"), TEXT("Mods/better_bases"), Manifest, Errors));
	TestEqual(TEXT("no errors"), Errors.Num(), 0);
	TestEqual(TEXT("two dependencies"), Manifest.Dependencies.Num(), 2);
	TestTrue(TEXT("optional flag"), Manifest.Dependencies.Num() == 2 && Manifest.Dependencies[1].bOptional);

	auto Rejects = [&](const TCHAR* Json, const TCHAR* Needle)
	{
		FMadModManifest Out;
		TArray<FString> ParseErrors;
		const bool bOk = FMadModManifest::ParseText(Json, TEXT("x"), Out, ParseErrors);
		return !bOk && ParseErrors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	};

	TestTrue(TEXT("reserved id"), Rejects(TEXT(R"({"schema":"madfall.mod/1","id":"madfall","version":"1.0.0","api_version":"0.1"})"), TEXT("reserved")));
	TestTrue(TEXT("uppercase id"), Rejects(TEXT(R"({"schema":"madfall.mod/1","id":"MyMod","version":"1.0.0","api_version":"0.1"})"), TEXT("lowercase")));
	TestTrue(TEXT("pak path escaping the mod"), Rejects(TEXT(R"({"schema":"madfall.mod/1","id":"sneaky","version":"1.0.0","api_version":"0.1","paks":["../../Engine/x.pak"]})"), TEXT("stay inside")));

	// Unknown fields warn but still load.
	FMadModManifest Future;
	TArray<FString> FutureErrors;
	TestTrue(TEXT("unknown field still loads"), FMadModManifest::ParseText(TEXT(R"({"schema":"madfall.mod/1","id":"future","version":"1.0.0","api_version":"0.1","hologram":true})"), TEXT("x"), Future, FutureErrors));
	TestEqual(TEXT("with one warning"), FutureErrors.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadModLoadOrderTest,
	"MadFall.Mods.LoadOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadModLoadOrderTest::RunTest(const FString& Parameters)
{
	using namespace MadModTests;

	// --- unconstrained mods are alphabetical, regardless of discovery order ------------
	{
		const FMadModResolution A = MadFall::ModResolver::Resolve({ Mod(TEXT("zeta")), Mod(TEXT("alpha")), Mod(TEXT("mid")) }, {});
		const FMadModResolution B = MadFall::ModResolver::Resolve({ Mod(TEXT("mid")), Mod(TEXT("zeta")), Mod(TEXT("alpha")) }, {});
		TestEqual(TEXT("alphabetical"), OrderString(A), FString(TEXT("alpha,mid,zeta")));
		TestEqual(TEXT("discovery order does not matter"), OrderString(B), OrderString(A));
		TestEqual(TEXT("identical fingerprints"), A.Fingerprint(), B.Fingerprint());
	}

	// --- dependencies, load_after and load_before shape the order ----------------------
	{
		FMadModManifest Base = Mod(TEXT("zbase"));
		FMadModManifest Addon = Mod(TEXT("addon"));
		Require(Addon, TEXT("zbase"));
		FMadModManifest Late = Mod(TEXT("alate"));
		Late.LoadAfter.Add(FName(TEXT("addon")));
		FMadModManifest Early = Mod(TEXT("yearly"));
		Early.LoadBefore.Add(FName(TEXT("zbase")));
		Early.LoadAfter.Add(FName(TEXT("not_installed")));   // soft edge to an absent mod: ignored

		const FMadModResolution Result = MadFall::ModResolver::Resolve({ Base, Addon, Late, Early }, {});
		TestEqual(TEXT("yearly before zbase before addon before alate"), OrderString(Result), FString(TEXT("yearly,zbase,addon,alate")));
		TestEqual(TEXT("no fatal issues"), Result.Issues.FilterByPredicate([](const FMadModIssue& I) { return I.bFatal; }).Num(), 0);
	}

	// --- missing and mismatched dependencies fail, and failures cascade ------------------
	{
		FMadModManifest Lib = Mod(TEXT("lib"), TEXT("1.4.0"));
		FMadModManifest NeedsNewLib = Mod(TEXT("needs_new"));
		Require(NeedsNewLib, TEXT("lib"), TEXT(">=2.0.0"));
		FMadModManifest NeedsNeedsNew = Mod(TEXT("needs_needs"));
		Require(NeedsNeedsNew, TEXT("needs_new"));
		FMadModManifest NeedsGhost = Mod(TEXT("needs_ghost"));
		Require(NeedsGhost, TEXT("ghost"));
		FMadModManifest OptionalGhost = Mod(TEXT("optional_ghost"));
		Require(OptionalGhost, TEXT("ghost"), TEXT("*"), /*bOptional*/ true);

		const FMadModResolution Result = MadFall::ModResolver::Resolve({ Lib, NeedsNewLib, NeedsNeedsNew, NeedsGhost, OptionalGhost }, {});
		TestEqual(TEXT("only lib and the optional one load"), OrderString(Result), FString(TEXT("lib,optional_ghost")));
		TestTrue(TEXT("version mismatch named"), HasIssue(Result, TEXT("needs_new"), TEXT("but 1.4.0 is installed")));
		TestTrue(TEXT("cascade named"), HasIssue(Result, TEXT("needs_needs"), TEXT("failed to load")));
		TestTrue(TEXT("missing named"), HasIssue(Result, TEXT("needs_ghost"), TEXT("not installed")));
	}

	// --- disabled by the player -------------------------------------------------------
	{
		FMadModManifest Lib = Mod(TEXT("lib"));
		FMadModManifest User = Mod(TEXT("user"));
		Require(User, TEXT("lib"));
		const FMadModResolution Result = MadFall::ModResolver::Resolve({ Lib, User }, { FName(TEXT("lib")) });
		TestEqual(TEXT("nothing loads"), Result.LoadOrder.Num(), 0);
		TestTrue(TEXT("lib is disabled, not failed"), Result.Status[FName(TEXT("lib"))] == EMadModStatus::DisabledByUser);
		TestTrue(TEXT("dependent says why"), HasIssue(Result, TEXT("user"), TEXT("is disabled")));
	}

	// --- cycles fail only their members ------------------------------------------------
	{
		FMadModManifest A = Mod(TEXT("cyc_a"));
		FMadModManifest B = Mod(TEXT("cyc_b"));
		FMadModManifest C = Mod(TEXT("cyc_c"));
		A.LoadAfter.Add(FName(TEXT("cyc_b")));
		B.LoadAfter.Add(FName(TEXT("cyc_c")));
		C.LoadAfter.Add(FName(TEXT("cyc_a")));
		FMadModManifest Downstream = Mod(TEXT("downstream"));
		Require(Downstream, TEXT("cyc_a"));
		FMadModManifest SoftDownstream = Mod(TEXT("soft_downstream"));
		SoftDownstream.LoadAfter.Add(FName(TEXT("cyc_b")));
		FMadModManifest Bystander = Mod(TEXT("bystander"));

		const FMadModResolution Result = MadFall::ModResolver::Resolve({ A, B, C, Downstream, SoftDownstream, Bystander }, {});
		TestEqual(TEXT("bystander and the soft dependent survive"), OrderString(Result), FString(TEXT("bystander,soft_downstream")));
		TestTrue(TEXT("cycle spelled out"), HasIssue(Result, TEXT("cyc_b"), TEXT("cyc_a -> cyc_b -> cyc_c")));
		TestTrue(TEXT("hard dependent fails for the right reason"), HasIssue(Result, TEXT("downstream"), TEXT("requires cyc_a, which failed")));
	}

	// --- incompatibility, API version, duplicates ----------------------------------------
	{
		FMadModManifest Hates = Mod(TEXT("hates"));
		Hates.Incompatible.Add(FName(TEXT("hated")));
		FMadModManifest Hated = Mod(TEXT("hated"));
		FMadModManifest Ancient = Mod(TEXT("ancient"));
		Ancient.ApiMajor = MadFall::ModApi::VersionMajor + 7;
		FMadModManifest Dup1 = Mod(TEXT("dup"), TEXT("1.0.0"));
		FMadModManifest Dup2 = Mod(TEXT("dup"), TEXT("2.0.0"));
		Dup2.Directory = TEXT("Mods/dup copy");

		const FMadModResolution Result = MadFall::ModResolver::Resolve({ Hates, Hated, Ancient, Dup1, Dup2 }, {});
		TestEqual(TEXT("declarer fails, target loads, first duplicate wins"), OrderString(Result), FString(TEXT("dup,hated")));
		TestTrue(TEXT("first copy kept"), Result.LoadOrder.Num() == 2 && Result.LoadOrder[0].Version.Major == 1);
		TestTrue(TEXT("incompatibility named"), HasIssue(Result, TEXT("hates"), TEXT("incompatible with hated")));
		TestTrue(TEXT("api version named"), HasIssue(Result, TEXT("ancient"), TEXT("mod API")));
		TestTrue(TEXT("duplicate warned"), HasIssue(Result, TEXT("dup"), TEXT("installed twice")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
