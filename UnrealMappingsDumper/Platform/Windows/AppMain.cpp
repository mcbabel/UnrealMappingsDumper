#include "../../pch.h"

#include "../../cli.h"
#include "../../memory.h"
#include "../../Gui/Gui.h"

#include <shellapi.h>

static void AttachToConsole()
{
	auto Output = GetStdHandle(STD_OUTPUT_HANDLE);

	if (Output && Output != INVALID_HANDLE_VALUE && GetFileType(Output) != FILE_TYPE_UNKNOWN)
		return;

	if (AttachConsole(ATTACH_PARENT_PROCESS))
	{
		FILE* File = nullptr;
		freopen_s(&File, "CONOUT$", "w", stdout);
		freopen_s(&File, "CONOUT$", "w", stderr);
	}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int Count = 0;
	auto Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);

	std::vector<std::string> Args;

	for (int i = 1; i < Count; i++)
		Args.push_back(Memory::PathToUtf8(std::filesystem::path(Arguments[i])));

	LocalFree(Arguments);

	bool bForceGui = Args.size() == 1 && Args[0] == "--gui";

	if (Args.empty() || bForceGui)
	{
		auto Result = Gui::Run();

		if (Result >= 0)
			return Result;

		MessageBoxA(nullptr, "Could not create the window (DirectX 11 is required).", "Unreal Mappings Dumper", MB_OK | MB_ICONERROR);
		return 1;
	}

	AttachToConsole();

	auto Result = Cli::Run(Args);

	fflush(stdout);
	fflush(stderr);

	return Result;
}
