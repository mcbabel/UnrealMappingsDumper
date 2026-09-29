#pragma once

namespace Log
{
	bool OpenFile(const std::filesystem::path& Path);
	void CloseFile();

	const std::filesystem::path& GetFilePath();
}

void UE_LOG(const char* Format, ...);
