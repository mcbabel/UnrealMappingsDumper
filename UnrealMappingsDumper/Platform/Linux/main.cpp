#include "../../pch.h"

#include "../../app.h"
#include "../../dumper.h"
#include "../../memory.h"
#include "Process.h"
#include "RemoteBackend.h"

#include <unistd.h>

namespace
{
	void PrintUsage()
	{
		printf(
			"Usage: unreal-mappings-dumper [options] [process name]\n"
			"\n"
			"Generates a .usmap mappings file from a running Unreal Engine game. Works with native Linux\n"
			"games and with Windows games running under Proton or Wine. Nothing is injected into the game.\n"
			"\n"
			"  process name        Name (or part of it) of the game executable. Without it the only running\n"
			"                      *-Win64-Shipping.exe or *-Linux-Shipping process is used.\n"
			"  -p, --pid <pid>     Dump this process.\n"
			"  -o, --output <path> Where to write the mappings, defaults to Mappings.usmap next to the game executable.\n"
			"  -l, --list          List the processes that can be dumped.\n"
			"  -h, --help          Show this help.\n"
			"\n"
			"Reading the memory of another process needs root, or the permission set once with:\n"
			"  sudo setcap cap_sys_ptrace=eip $(which unreal-mappings-dumper)\n");
	}

	bool IsWritableDirectory(const std::filesystem::path& Directory)
	{
		return !Directory.empty() && access(Directory.c_str(), W_OK) == 0;
	}

	void GiveToInvokingUser(const std::filesystem::path& Path)
	{
		auto Uid = getenv("SUDO_UID");
		auto Gid = getenv("SUDO_GID");

		if (geteuid() == 0 && Uid && Gid)
		{
			if (chown(Path.c_str(), static_cast<uid_t>(atoi(Uid)), static_cast<gid_t>(atoi(Gid))) != 0)
				UE_LOG("Could not change the owner of %s.", Memory::PathToUtf8(Path).c_str());
		}
	}
}

int main(int argc, char** argv)
{
	std::string Pattern;
	int Pid = 0;
	std::filesystem::path OutputPath;

	for (int i = 1; i < argc; i++)
	{
		std::string Arg = argv[i];

		if (Arg == "-h" || Arg == "--help")
		{
			PrintUsage();
			return 0;
		}
		else if (Arg == "-l" || Arg == "--list")
		{
			for (auto& Info : Process::List())
				printf("%8d  %s%s\n", Info.Pid, Info.Name.c_str(), Info.bWine ? "  (Wine/Proton)" : "");

			return 0;
		}
		else if ((Arg == "-p" || Arg == "--pid") && i + 1 < argc)
		{
			Pid = atoi(argv[++i]);
		}
		else if ((Arg == "-o" || Arg == "--output") && i + 1 < argc)
		{
			OutputPath = argv[++i];
		}
		else if (!Arg.empty() && Arg[0] == '-')
		{
			fprintf(stderr, "Unknown option %s\n\n", Arg.c_str());
			PrintUsage();
			return 2;
		}
		else
		{
			Pattern = Arg;
		}
	}

	std::optional<Process::FInfo> Target;

	if (Pid)
	{
		Target = Process::Get(Pid);

		if (!Target)
		{
			std::error_code Error;

			if (std::filesystem::exists(std::format("/proc/{}", Pid), Error))
				fprintf(stderr, "Process %d isn't accessible. Run as root (sudo) or as the user the game runs as.\n", Pid);
			else
				fprintf(stderr, "Process %d doesn't exist.\n", Pid);

			return 1;
		}
	}
	else
	{
		auto Matches = Process::Find(Pattern);

		if (Matches.empty())
		{
			fprintf(stderr, "No matching game process found. Start the game first, or pass its name or --pid. --list shows all processes.\n");
			return 1;
		}

		if (Matches.size() > 1)
		{
			fprintf(stderr, "More than one process matches, pass the exact name or --pid:\n");

			for (auto& Info : Matches)
				fprintf(stderr, "%8d  %s\n", Info.Pid, Info.Name.c_str());

			return 1;
		}

		Target = Matches[0];
	}

	std::string Error;
	auto Backend = FRemoteBackend::Open(*Target, Error);

	if (!Backend)
	{
		fprintf(stderr, "%s\n", Error.c_str());
		return 1;
	}

	Memory::SetBackend(std::move(Backend));

	if (!Memory::GetModuleSize())
	{
		fprintf(stderr, "Could not parse the executable of process %d.\n", Target->Pid);
		return 1;
	}

	auto ModulePath = Memory::Get().GetModule().Path;

	if (OutputPath.empty())
	{
		auto GameDirectory = ModulePath.parent_path();
		OutputPath = (IsWritableDirectory(GameDirectory) ? GameDirectory : std::filesystem::current_path()) / "Mappings.usmap";
	}

	auto LogPath = OutputPath.parent_path() / "UnrealMappingsDumper.log";
	Log::OpenFile(LogPath);

	UE_LOG("Unreal Mappings Dumper created by OutTheShade");
	UE_LOG("Process: %s (pid %d%s)", Target->Name.c_str(), Target->Pid, Target->bWine ? ", Wine/Proton" : "");
	UE_LOG("Executable: %s", Memory::PathToUtf8(ModulePath).c_str());

	App::FGameInfo Game;
	Game.ExecutableName = Target->Name;

	if (!App::Init(Game))
	{
		UE_LOG("Failed to initialize the dumper.");
		Log::CloseFile();
		GiveToInvokingUser(LogPath);
		return 1;
	}

	auto Start = std::chrono::steady_clock::now();

	bool bSuccess = Dumper::Run(ECompressionMethod::None, OutputPath);

	auto End = std::chrono::steady_clock::now();

	if (bSuccess)
		UE_LOG("Successfully generated mappings file in %.02f ms", std::chrono::duration<double, std::milli>(End - Start).count());
	else
		UE_LOG("Failed to generate the mappings file.");

	Log::CloseFile();

	GiveToInvokingUser(LogPath);

	if (bSuccess)
		GiveToInvokingUser(OutputPath);

	return bSuccess ? 0 : 1;
}
