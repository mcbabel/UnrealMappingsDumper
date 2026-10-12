#include "../pch.h"

#include "System.h"
#include "../process.h"

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

struct Gui::System::FChildProcess::FImpl
{
	pid_t Pid = -1;
	int ReadFd = -1;
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
}

bool Gui::System::FChildProcess::Start(const std::vector<std::string>& Args, bool bElevated, std::string& OutError)
{
	std::vector<std::string> Command;

	if (bElevated)
		Command.push_back("pkexec");

	Command.insert(Command.end(), Args.begin(), Args.end());

	std::vector<char*> Argv;

	for (auto& Arg : Command)
		Argv.push_back(const_cast<char*>(Arg.c_str()));

	Argv.push_back(nullptr);

	int Pipe[2];

	if (pipe(Pipe) != 0)
	{
		OutError = "Could not create a pipe.";
		return false;
	}

	auto Pid = fork();

	if (Pid < 0)
	{
		close(Pipe[0]);
		close(Pipe[1]);
		OutError = "Could not start a process.";
		return false;
	}

	if (Pid == 0)
	{
		dup2(Pipe[1], STDOUT_FILENO);
		dup2(Pipe[1], STDERR_FILENO);
		close(Pipe[0]);
		close(Pipe[1]);

		execvp(Argv[0], Argv.data());

		static const char Message[] = "Could not start the dumper process.\n";
		(void)!write(STDERR_FILENO, Message, sizeof(Message) - 1);
		_exit(127);
	}

	close(Pipe[1]);

	Impl->Pid = Pid;
	Impl->ReadFd = Pipe[0];

	Impl->Reader = std::thread([Impl = Impl.get()]
		{
			std::string Pending;
			char Buffer[4096];

			for (;;)
			{
				auto Count = read(Impl->ReadFd, Buffer, sizeof(Buffer));

				if (Count <= 0)
					break;

				Pending.append(Buffer, Count);

				size_t Newline;

				while ((Newline = Pending.find('\n')) != std::string::npos)
				{
					std::lock_guard Lock(Impl->Mutex);
					Impl->Lines.push_back(Pending.substr(0, Newline));
					Pending.erase(0, Newline + 1);
				}
			}

			if (!Pending.empty())
			{
				std::lock_guard Lock(Impl->Mutex);
				Impl->Lines.push_back(Pending);
			}

			close(Impl->ReadFd);

			int Status = 0;
			waitpid(Impl->Pid, &Status, 0);

			Impl->ExitCode = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
			Impl->bFinished = true;
		});

	return true;
}

bool Gui::System::FChildProcess::IsRunning()
{
	return Impl->Pid > 0 && !Impl->bFinished;
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
	auto Directory = (std::filesystem::is_directory(File) ? File : File.parent_path()).string();

	if (fork() == 0)
	{
		setsid();

		int Null = open("/dev/null", O_RDWR);
		dup2(Null, STDIN_FILENO);
		dup2(Null, STDOUT_FILENO);
		dup2(Null, STDERR_FILENO);

		execlp("xdg-open", "xdg-open", Directory.c_str(), nullptr);
		_exit(127);
	}
}

bool Gui::System::CanElevateChild()
{
	return access("/usr/bin/pkexec", X_OK) == 0 || access("/bin/pkexec", X_OK) == 0;
}

std::vector<std::string> Gui::System::GetGrantPermissionCommand()
{
	return { "setcap", "cap_sys_ptrace=eip", Process::GetSelfPath().string() };
}

void Gui::System::Restart(bool bElevated)
{
	(void)bElevated;

	auto Self = Process::GetSelfPath().string();
	execl(Self.c_str(), Self.c_str(), nullptr);
}
