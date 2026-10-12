#pragma once

namespace Gui::System
{
	class FChildProcess
	{
		struct FImpl;
		std::unique_ptr<FImpl> Impl;

	public:

		FChildProcess();
		~FChildProcess();

		bool Start(const std::vector<std::string>& Args, bool bElevated, std::string& OutError);
		bool IsRunning();
		int GetExitCode();
		std::vector<std::string> TakeLines();
	};

	void OpenFolder(const std::filesystem::path& File);

	bool CanElevateChild();

	std::vector<std::string> GetGrantPermissionCommand();

	void Restart(bool bElevated);
}
