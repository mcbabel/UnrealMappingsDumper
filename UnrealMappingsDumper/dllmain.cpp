#include "pch.h"

#include "app.h"
#include "dumper.h"
#include "memory.h"
#include "Platform/Windows/LocalBackend.h"

#pragma comment(lib, "Version.lib")

static_assert(sizeof(void*) == 8, "UnrealMappingsDumper only supports 64 bit games, build the x64 configuration.");

constexpr bool OpenConsole = true;

constexpr ECompressionMethod Compression = ECompressionMethod::None;

static std::filesystem::path GetGamePath()
{
	wchar_t GameFilePath[MAX_PATH];
	GetModuleFileNameW(nullptr, GameFilePath, MAX_PATH);

	return std::filesystem::path(GameFilePath);
}

static App::FGameInfo GetGameInfo()
{
	App::FGameInfo Ret;

	auto GamePath = GetGamePath();
	Ret.ExecutableName = Memory::PathToUtf8(GamePath.filename());

	DWORD verHandle = 0;
	DWORD verSize = GetFileVersionInfoSizeW(GamePath.c_str(), &verHandle);

	if (!verSize)
		return Ret;

	std::string VerData(verSize, '\0');
	UINT size = 0;
	VS_FIXEDFILEINFO* VersionInfo = nullptr;

	if (!GetFileVersionInfoW(GamePath.c_str(), verHandle, verSize, VerData.data()) ||
		!VerQueryValueW(VerData.data(), L"\\", reinterpret_cast<LPVOID*>(&VersionInfo), &size) ||
		!size ||
		VersionInfo->dwSignature != 0xfeef04bd)
	{
		return Ret;
	}

	auto Major = (VersionInfo->dwFileVersionMS >> 16) & 0xffff;
	auto Minor = (VersionInfo->dwFileVersionMS >> 0) & 0xffff;

	Ret.FileVersion = std::format("{:d}.{:d}.{:d}.{:d}",
		Major,
		Minor,
		(VersionInfo->dwFileVersionLS >> 16) & 0xffff,
		(VersionInfo->dwFileVersionLS >> 0) & 0xffff);

	Ret.MajorMinor = Major + Minor / (Minor >= 10 ? 100.f : 10.f);

	return Ret;
}

static bool IsWritableDirectory(const std::filesystem::path& Directory)
{
	std::error_code Error;

	if (Directory.empty() || !std::filesystem::is_directory(Directory, Error))
		return false;

	auto TestFile = Directory / L"UnrealMappingsDumper.write-test";
	auto Handle = CreateFileW(TestFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);

	if (Handle == INVALID_HANDLE_VALUE)
		return false;

	CloseHandle(Handle);
	return true;
}

static std::filesystem::path GetOutputDirectory()
{
	static std::filesystem::path Directory = []
		{
			auto GameDirectory = GetGamePath().parent_path();

			if (IsWritableDirectory(GameDirectory))
				return GameDirectory;

			HMODULE Self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetOutputDirectory), &Self);

			wchar_t DllPath[MAX_PATH];

			if (Self && GetModuleFileNameW(Self, DllPath, MAX_PATH))
			{
				auto DllDirectory = std::filesystem::path(DllPath).parent_path();

				if (IsWritableDirectory(DllDirectory))
					return DllDirectory;
			}

			std::error_code Error;
			auto TempDirectory = std::filesystem::temp_directory_path(Error) / L"UnrealMappingsDumper";
			std::filesystem::create_directories(TempDirectory, Error);

			return TempDirectory;
		}();

	return Directory;
}

static bool RunGuarded(const char* Step, std::function<bool()> Function)
{
	bool bResult = false;
	Memory::FaultInfo Fault;

	if (!Memory::TryInvoke([&] { bResult = Function(); }, &Fault))
	{
		UE_LOG("%s crashed with exception 0x%08X at %s.", Step, Fault.Code, Memory::DescribeAddress(Fault.Address).c_str());
		UE_LOG("The game was kept alive, but the offsets or signatures don't match this game. Please include the log file when reporting this.");
		return false;
	}

	return bResult;
}

static void ShowResult(bool bSuccess)
{
	auto LogPath = Memory::PathToUtf8(Log::GetFilePath());

	auto Message = bSuccess
		? std::format("Mappings were written to:\n{}\n\nLog: {}", Memory::PathToUtf8(GetOutputDirectory() / "Mappings.usmap"), LogPath)
		: std::format("Failed to generate mappings.\n\nSee the log for details:\n{}", LogPath);

	MessageBoxA(nullptr, Message.c_str(), "UnrealMappingsDumper", MB_OK | MB_SETFOREGROUND | (bSuccess ? MB_ICONINFORMATION : MB_ICONERROR));
}

void WINAPI Main(HMODULE Module)
{
	if constexpr (OpenConsole)
	{
		AllocConsole();
		FILE* f;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	Memory::SetBackend(std::make_unique<FLocalBackend>());

	Log::OpenFile(GetOutputDirectory() / "UnrealMappingsDumper.log");

	UE_LOG("Unreal Mappings Dumper created by OutTheShade");
	UE_LOG("Output directory: %s", Memory::PathToUtf8(GetOutputDirectory()).c_str());

	auto Game = GetGameInfo();

	if (!RunGuarded("Initialization", [&] { return App::Init(Game); }))
	{
		UE_LOG("Failed to initialize the dumper. Returning.");
		ShowResult(false);

		Log::CloseFile();
		FreeLibraryAndExitThread(Module, 0);
		return;
	}

	auto Start = std::chrono::steady_clock::now();

	bool bSuccess = RunGuarded("Dumping", [] { return Dumper::Run(Compression, GetOutputDirectory() / "Mappings.usmap"); });

	auto End = std::chrono::steady_clock::now();

	if (bSuccess)
		UE_LOG("Successfully generated mappings file in %.02f ms", std::chrono::duration<double, std::milli>(End - Start).count());
	else
		UE_LOG("Failed to generate the mappings file.");

	ShowResult(bSuccess);

	Log::CloseFile();
	FreeLibraryAndExitThread(Module, 0);
}

BOOL APIENTRY DllMain(
	HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		CloseHandle(CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)Main, hModule, 0, nullptr));
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
