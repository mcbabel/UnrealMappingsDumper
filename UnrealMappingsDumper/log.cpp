#include "pch.h"

#include "log.h"

#ifdef _WIN32
#include <share.h>
#endif

static FILE* LogFile = nullptr;
static std::filesystem::path LogFilePath;
static std::mutex LogMutex;

bool Log::OpenFile(const std::filesystem::path& Path)
{
	std::lock_guard Lock(LogMutex);

	if (LogFile)
		fclose(LogFile);

#ifdef _WIN32
	LogFile = _wfsopen(Path.c_str(), L"w", _SH_DENYWR);
#else
	LogFile = fopen(Path.c_str(), "w");
#endif

	if (LogFile)
		LogFilePath = Path;

	return LogFile != nullptr;
}

void Log::CloseFile()
{
	std::lock_guard Lock(LogMutex);

	if (LogFile)
	{
		fclose(LogFile);
		LogFile = nullptr;
	}
}

const std::filesystem::path& Log::GetFilePath()
{
	return LogFilePath;
}

void UE_LOG(const char* Format, ...)
{
	char Buffer[2048];

	va_list Args;
	va_start(Args, Format);
	vsnprintf(Buffer, sizeof(Buffer), Format, Args);
	va_end(Args);

	std::lock_guard Lock(LogMutex);

	printf("[=] %s\n", Buffer);
	fflush(stdout);

	if (LogFile)
	{
		fprintf(LogFile, "[=] %s\n", Buffer);
		fflush(LogFile);
	}
}
