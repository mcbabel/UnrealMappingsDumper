#include "pch.h"

#include "app.h"
#include "unrealVersion.h"

static bool InitEngine(const App::FGameInfo& Game)
{
	// if an engine version has a different type or offset, set it here

	if (Game.ExecutableName == "FortniteClient-Win64-Shipping.exe"
		and Game.MajorMinor >= 5.0)
	{
		return IUnrealVersion::InitTypes<Version_FortniteLatest>();
	}

	return IUnrealVersion::InitTypes<UnrealVersionBase>();
}

bool App::Init(const FGameInfo& Game)
{
	UE_LOG("Game executable: %s", Game.ExecutableName.c_str());
	UE_LOG("Executable version: %s", Game.FileVersion.empty() ? "unknown" : Game.FileVersion.c_str());

	std::string LowerName = Game.ExecutableName;

	for (auto& C : LowerName)
		C = static_cast<char>(tolower(static_cast<unsigned char>(C)));

	bool bLooksLikeUnreal = false;

	for (auto Marker : { "-win64-", "-wingdk-", "-linux-shipping", "-linux-test", "-linux-development", "-linux-debuggame", "-linuxarm64-" })
	{
		if (LowerName.find(Marker) != std::string::npos)
			bLooksLikeUnreal = true;
	}

	if (!bLooksLikeUnreal)
	{
		UE_LOG("Note: this doesn't look like an Unreal Engine game executable (<Project>-Win64-Shipping.exe or <Project>-Linux-Shipping).");
		UE_LOG("      Make sure this is the actual game process and not a launcher/bootstrap executable.");
	}

	return InitEngine(Game);
}
