#include "../pch.h"

#include "System.h"
#include "../process.h"

#include <shellapi.h>

namespace
{
	std::wstring ToWide(const std::string& Utf8)
	{
		auto Size = MultiByteToWideChar(CP_UTF8, 0, Utf8.c_str(), static_cast<int>(Utf8.size()), nullptr, 0);
		std::wstring Ret(Size, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, Utf8.c_str(), static_cast<int>(Utf8.size()), Ret.data(), Size);
		return Ret;
	}

	std::wstring QuoteArgument(const std::wstring& Arg)
	{
		if (!Arg.empty() && Arg.find_first_of(L" \t\"") == std::wstring::npos)
			return Arg;

		std::wstring Ret = L"\"";
		size_t Backslashes = 0;

		for (auto C : Arg)
		{
			if (C == L'\\')
			{
				Backslashes++;
				continue;
			}

			if (C == L'"')
				Ret.append(Backslashes * 2 + 1, L'\\');
			else
				Ret.append(Backslashes, L'\\');

			Backslashes = 0;
			Ret += C;
		}

		Ret.append(Backslashes * 2, L'\\');
		Ret += L"\"";
		return Ret;
	}
}

struct Gui::System::FChildProcess::FImpl
{
	HANDLE Process = nullptr;
	HANDLE ReadPipe = nullptr;
	std::thread Reader;
	std::mutex Mutex;
	std::vector<std::string> Lines;
	std::atomic<bool> bFinished = false;
	int ExitCode = -1;
};

Gui::System::FChildProcess::FChildProcess() : Impl(std::make_unique<FImpl>())
{
}

Gui::System::FChildProcess::~FChildProcess()
{
	if (Impl->Reader.joinable())
		Impl->Reader.join();

	if (Impl->Process)
		CloseHandle(Impl->Process);
}

bool Gui::System::FChildProcess::Start(const std::vector<std::string>& Args, bool bElevated, std::string& OutError)
{
	(void)bElevated;

	SECURITY_ATTRIBUTES Security{ sizeof(Security), nullptr, TRUE };
	HANDLE ReadPipe = nullptr, WritePipe = nullptr;

	if (!CreatePipe(&ReadPipe, &WritePipe, &Security, 0))
	{
		OutError = "Could not create a pipe.";
		return false;
	}

	SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0);

	std::wstring CommandLine;

	for (auto& Arg : Args)
	{
		if (!CommandLine.empty())
			CommandLine += L" ";

		CommandLine += QuoteArgument(ToWide(Arg));
	}

	STARTUPINFOW Startup{};
	Startup.cb = sizeof(Startup);
	Startup.dwFlags = STARTF_USESTDHANDLES;
	Startup.hStdOutput = WritePipe;
	Startup.hStdError = WritePipe;
	Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION Info{};

	auto Application = ToWide(Args[0]);

	if (!CreateProcessW(Application.c_str(), CommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Info))
	{
		OutError = std::format("Could not start {} (error {}).", Args[0], GetLastError());
		CloseHandle(ReadPipe);
		CloseHandle(WritePipe);
		return false;
	}

	CloseHandle(WritePipe);
	CloseHandle(Info.hThread);

	Impl->Process = Info.hProcess;
	Impl->ReadPipe = ReadPipe;

	Impl->Reader = std::thread([Impl = Impl.get()]
		{
			std::string Pending;
			char Buffer[4096];
			DWORD Count = 0;

			while (ReadFile(Impl->ReadPipe, Buffer, sizeof(Buffer), &Count, nullptr) && Count > 0)
			{
				Pending.append(Buffer, Count);

				size_t Newline;

				while ((Newline = Pending.find('\n')) != std::string::npos)
				{
					auto Line = Pending.substr(0, Newline);

					if (!Line.empty() && Line.back() == '\r')
						Line.pop_back();

					std::lock_guard Lock(Impl->Mutex);
					Impl->Lines.push_back(std::move(Line));
					Pending.erase(0, Newline + 1);
				}
			}

			if (!Pending.empty())
			{
				std::lock_guard Lock(Impl->Mutex);
				Impl->Lines.push_back(Pending);
			}

			CloseHandle(Impl->ReadPipe);

			WaitForSingleObject(Impl->Process, INFINITE);

			DWORD ExitCode = 0;
			GetExitCodeProcess(Impl->Process, &ExitCode);

			Impl->ExitCode = static_cast<int>(ExitCode);
			Impl->bFinished = true;
		});

	return true;
}

bool Gui::System::FChildProcess::IsRunning()
{
	return Impl->Process && !Impl->bFinished;
}

int Gui::System::FChildProcess::GetExitCode()
{
	return Impl->ExitCode;
}

std::vector<std::string> Gui::System::FChildProcess::TakeLines()
{
	std::lock_guard Lock(Impl->Mutex);
	return std::exchange(Impl->Lines, {});
}

void Gui::System::OpenFolder(const std::filesystem::path& File)
{
	if (std::filesystem::is_directory(File))
	{
		ShellExecuteW(nullptr, L"open", File.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}

	auto Arguments = L"/select,\"" + File.wstring() + L"\"";
	ShellExecuteW(nullptr, L"open", L"explorer.exe", Arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

bool Gui::System::CanElevateChild()
{
	return false;
}

std::vector<std::string> Gui::System::GetGrantPermissionCommand()
{
	return {};
}

void Gui::System::Restart(bool bElevated)
{
	auto Self = Process::GetSelfPath().wstring();

	SHELLEXECUTEINFOW Info{};
	Info.cbSize = sizeof(Info);
	Info.lpVerb = bElevated ? L"runas" : L"open";
	Info.lpFile = Self.c_str();
	Info.nShow = SW_SHOWNORMAL;

	if (ShellExecuteExW(&Info))
		ExitProcess(0);
}
