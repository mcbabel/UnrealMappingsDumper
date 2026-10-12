#include "pch.h"

#include "cli.h"
#include "aeskey.h"
#include "app.h"
#include "dumper.h"
#include "memory.h"
#include "process.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{
	bool IsWritableDirectory(const std::filesystem::path& Directory)
	{
		std::error_code Error;

		if (Directory.empty() || !std::filesystem::is_directory(Directory, Error))
			return false;

		auto TestFile = Directory / "UnrealMappingsDumper.write-test";

		{
			std::ofstream File(TestFile, std::ios::binary);

			if (!File)
				return false;
		}

		std::filesystem::remove(TestFile, Error);
		return true;
	}

	void GiveToInvokingUser(const std::filesystem::path& Path)
	{
#ifndef _WIN32
		if (geteuid() != 0)
			return;

		auto Uid = getenv("SUDO_UID");
		auto Gid = getenv("SUDO_GID");

		if (!Uid)
		{
			Uid = getenv("PKEXEC_UID");
			Gid = nullptr;
		}

		if (!Uid)
			return;

		auto UserId = static_cast<uid_t>(atoi(Uid));
		auto GroupId = Gid ? static_cast<gid_t>(atoi(Gid)) : static_cast<gid_t>(-1);

		if (chown(Path.c_str(), UserId, GroupId) != 0)
			UE_LOG("Could not change the owner of %s.", Memory::PathToUtf8(Path).c_str());
#else
		(void)Path;
#endif
	}

	int FindAesKey(const std::filesystem::path& Executable, std::filesystem::path PakDirectory)
	{
		if (PakDirectory.empty())
			PakDirectory = AesKey::GuessPakDirectory(Executable);

		if (PakDirectory.empty())
			UE_LOG("Note: the pak files of the game weren't found, so the keys can't be verified. Pass --paks <directory> to check them.");
		else
			UE_LOG("Pak files: %s", Memory::PathToUtf8(PakDirectory).c_str());

		auto Start = std::chrono::steady_clock::now();
		auto Result = AesKey::Find(PakDirectory);
		auto Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();

		size_t Encrypted = 0;
		size_t Verifiable = 0;

		for (auto& Container : Result.Containers)
		{
			Encrypted += Container.bEncrypted;
			Verifiable += Container.bHasBlock;
		}

		bool bMainKey = false;
		bool bVerified = false;
		bool bCandidates = false;

		for (auto& Key : Result.Keys)
		{
			if (!Key.bVerified)
				continue;

			bVerified = true;

			if (AesKey::IsZeroGuid(Key.Guid))
			{
				bMainKey = true;
				UE_LOG("AES key: %s", Aes::ToHex(Key.Key).c_str());
				UE_LOG("Verified with %zu encrypted %s, found in the %s of the game.", Key.Containers, Key.Containers == 1 ? "file" : "files", Key.Source.c_str());
			}
		}

		for (auto& Key : Result.Keys)
		{
			if (Key.bVerified && !AesKey::IsZeroGuid(Key.Guid))
			{
				UE_LOG("Dynamic AES key %s: %s", AesKey::GuidToString(Key.Guid).c_str(), Aes::ToHex(Key.Key).c_str());
				UE_LOG("Verified with %zu encrypted %s, found in the %s of the game.", Key.Containers, Key.Containers == 1 ? "file" : "files", Key.Source.c_str());
			}
		}

		for (auto& Key : Result.Keys)
		{
			if (!Key.bVerified)
			{
				bCandidates = true;
				UE_LOG("Possible AES key: %s", Aes::ToHex(Key.Key).c_str());
			}
		}

		UE_LOG("Searched for %.1f s.", Seconds);

		if (bVerified)
		{
			if (!bMainKey && Encrypted > 0 && std::any_of(Result.Containers.begin(), Result.Containers.end(), [](const AesKey::FContainer& C) { return C.bHasBlock && AesKey::IsZeroGuid(C.Guid); }))
				UE_LOG("The main AES key wasn't found.");

			return 0;
		}

		if (!Result.PakDirectory.empty() && !Result.Containers.empty() && Encrypted == 0 && !bCandidates)
		{
			UE_LOG("No AES key is needed, the pak files of this game aren't encrypted.");
			return 0;
		}

		if (bCandidates)
		{
			if (Verifiable)
				UE_LOG("None of the possible keys decrypts the pak files of the game.");
			else if (!Result.Containers.empty())
				UE_LOG("The indexes of the pak files aren't encrypted, so the keys can't be verified.");

			return 3;
		}

		UE_LOG("No AES key found.");
		return 1;
	}
}

void Cli::PrintUsage()
{
	printf(
		"Usage: unreal-mappings-dumper [options] [process name]\n"
		"\n"
		"Generates a .usmap mappings file from a running Unreal Engine game. Works with Windows games,\n"
		"native Linux games and Windows games running under Proton or Wine. Nothing is injected into the game.\n"
		"Started without options, the window opens instead.\n"
		"\n"
		"  process name        Name (or part of it) of the game executable. Without it the only running\n"
		"                      *-Win64-Shipping.exe or *-Linux-Shipping process is used.\n"
		"  -p, --pid <pid>     Dump this process.\n"
		"  -o, --output <path> Where to write the mappings, defaults to Mappings.usmap next to the game executable.\n"
		"  -l, --list          List the processes that can be dumped.\n"
		"      --aes           Find the AES key of the game instead of dumping mappings.\n"
		"      --paks <dir>    The pak files to verify the AES key with, found automatically if not set.\n"
		"  -h, --help          Show this help.\n"
#ifndef _WIN32
		"\n"
		"Reading the memory of another process needs root, or the permission set once with:\n"
		"  sudo setcap cap_sys_ptrace=eip $(which unreal-mappings-dumper)\n"
#endif
	);
}

int Cli::Run(const std::vector<std::string>& Args)
{
	std::string Pattern;
	int Pid = 0;
	std::filesystem::path OutputPath;
	bool bListAll = false;
	bool bAes = false;
	std::filesystem::path PakDirectory;

	for (size_t i = 0; i < Args.size(); i++)
	{
		auto& Arg = Args[i];

		if (Arg == "-h" || Arg == "--help")
		{
			PrintUsage();
			return 0;
		}
		else if (Arg == "-l" || Arg == "--list")
		{
			bListAll = true;
		}
		else if ((Arg == "-p" || Arg == "--pid") && i + 1 < Args.size())
		{
			Pid = atoi(Args[++i].c_str());
		}
		else if ((Arg == "-o" || Arg == "--output") && i + 1 < Args.size())
		{
			OutputPath = std::filesystem::path(std::u8string(Args[i + 1].begin(), Args[i + 1].end()));
			i++;
		}
		else if (Arg == "--aes")
		{
			bAes = true;
		}
		else if (Arg == "--paks" && i + 1 < Args.size())
		{
			PakDirectory = std::filesystem::path(std::u8string(Args[i + 1].begin(), Args[i + 1].end()));
			i++;
		}
		else if (Arg == "--cli" || Arg == "--no-gui")
		{
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

	if (bListAll)
	{
		for (auto& Info : Process::List())
			printf("%8d  %s%s\n", Info.Pid, Info.Name.c_str(), Info.bWine ? "  (Wine/Proton)" : "");

		return 0;
	}

	std::optional<Process::FInfo> Target;

	if (Pid)
	{
		Target = Process::Get(Pid);

		if (!Target)
		{
			fprintf(stderr, "Process %d doesn't exist or isn't accessible. Run as root/administrator or as the user the game runs as.\n", Pid);
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
	auto Backend = Process::OpenBackend(*Target, Error);

	if (!Backend)
	{
		fprintf(stderr, "%s\n", Error.c_str());
		fflush(stderr);
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

	if (bAes)
	{
		auto Result = FindAesKey(ModulePath, PakDirectory);
		Log::CloseFile();
		GiveToInvokingUser(LogPath);
		return Result;
	}

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
