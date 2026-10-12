#include "../../pch.h"

#include "../../cli.h"

#ifdef UMD_WITH_GUI
#include "../../Gui/Gui.h"
#endif

#include <unistd.h>

int main(int argc, char** argv)
{
	std::vector<std::string> Args(argv + 1, argv + argc);

#ifdef UMD_WITH_GUI
	bool bForceGui = Args.size() == 1 && Args[0] == "--gui";
	bool bHasDisplay = getenv("DISPLAY") || getenv("WAYLAND_DISPLAY");

	if ((Args.empty() && geteuid() != 0 && bHasDisplay) || bForceGui)
	{
		auto Result = Gui::Run();

		if (Result >= 0)
			return Result;

		fprintf(stderr, "Could not open the window, using the command line instead.\n\n");

		if (bForceGui)
			return 1;
	}
#endif

	return Cli::Run(Args);
}
