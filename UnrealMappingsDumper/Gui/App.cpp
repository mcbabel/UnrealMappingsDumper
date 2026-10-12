#include "../pch.h"

#include "Gui.h"
#include "Window.h"
#include "System.h"
#include "../process.h"

#include "imgui.h"

extern const unsigned char UmdFontRoboto[];
extern const size_t UmdFontRobotoSize;
extern const unsigned char UmdFontCousine[];
extern const size_t UmdFontCousineSize;

#ifndef UMD_VERSION
#define UMD_VERSION "dev"
#endif

namespace
{
	namespace Palette
	{
		constexpr ImVec4 Background{ 0.071f, 0.078f, 0.094f, 1.0f };
		constexpr ImVec4 Card{ 0.106f, 0.118f, 0.141f, 1.0f };
		constexpr ImVec4 CardHover{ 0.137f, 0.153f, 0.184f, 1.0f };
		constexpr ImVec4 Border{ 0.180f, 0.196f, 0.231f, 1.0f };
		constexpr ImVec4 Text{ 0.910f, 0.922f, 0.937f, 1.0f };
		constexpr ImVec4 Muted{ 0.553f, 0.584f, 0.635f, 1.0f };
		constexpr ImVec4 Accent{ 0.357f, 0.549f, 1.000f, 1.0f };
		constexpr ImVec4 AccentHover{ 0.455f, 0.624f, 1.000f, 1.0f };
		constexpr ImVec4 AccentActive{ 0.290f, 0.475f, 0.910f, 1.0f };
		constexpr ImVec4 Selected{ 0.157f, 0.216f, 0.361f, 1.0f };
		constexpr ImVec4 Success{ 0.247f, 0.725f, 0.314f, 1.0f };
		constexpr ImVec4 Warning{ 0.824f, 0.600f, 0.133f, 1.0f };
		constexpr ImVec4 Error{ 0.973f, 0.318f, 0.286f, 1.0f };
		constexpr ImVec4 Windows{ 0.231f, 0.510f, 0.965f, 1.0f };
		constexpr ImVec4 Proton{ 0.639f, 0.443f, 0.969f, 1.0f };
		constexpr ImVec4 Linux{ 0.247f, 0.725f, 0.314f, 1.0f };
	}

	ImU32 ToU32(const ImVec4& Color, float Alpha = 1.0f)
	{
		return ImGui::ColorConvertFloat4ToU32(ImVec4(Color.x, Color.y, Color.z, Color.w * Alpha));
	}

	enum class EAccess
	{
		Unknown,
		Ok,
		Denied,
		Failed
	};

	enum class ETask
	{
		None,
		Dump,
		Aes,
		Grant
	};

	enum class EResult
	{
		None,
		Success,
		Warning,
		Info,
		Failed,
		Cancelled
	};

	enum class EIcon
	{
		Check,
		Cross,
		Warning,
		Info
	};

	struct FLogLine
	{
		std::string Text;
		ImVec4 Color;
	};

	struct FFoundKey
	{
		std::string Label;
		std::string Key;
	};

	struct FState
	{
		std::vector<Process::FInfo> Processes;
		std::chrono::steady_clock::time_point LastRefresh;
		bool bShowAll = false;
		char Filter[128] = "";
		int SelectedPid = 0;

		EAccess Access = EAccess::Unknown;
		int AccessPid = 0;
		std::string AccessError;
		bool bAlwaysElevate = false;

		char OutputPath[1024] = "";

		std::unique_ptr<Gui::System::FChildProcess> Child;
		ETask Task = ETask::None;
		bool bChildElevated = false;
		std::chrono::steady_clock::time_point TaskStart;

		std::vector<FLogLine> Log;
		bool bScrollLog = false;

		EResult Result = EResult::None;
		std::string ResultTitle;
		std::string ResultText;
		std::filesystem::path ResultPath;

		std::vector<FFoundKey> Keys;
		bool bNoKeyNeeded = false;
	};

	ImFont* BodyFont = nullptr;
	ImFont* TitleFont = nullptr;
	ImFont* SmallFont = nullptr;
	ImFont* MonoFont = nullptr;
	float Scale = 1.0f;

	float S(float Value)
	{
		return Value * Scale;
	}

	void LoadFonts()
	{
		auto& IO = ImGui::GetIO();
		IO.Fonts->Clear();

		ImFontConfig Config;
		Config.FontDataOwnedByAtlas = false;
		Config.OversampleH = 2;

		auto Roboto = const_cast<unsigned char*>(UmdFontRoboto);
		auto Cousine = const_cast<unsigned char*>(UmdFontCousine);

		BodyFont = IO.Fonts->AddFontFromMemoryTTF(Roboto, static_cast<int>(UmdFontRobotoSize), std::round(S(16.0f)), &Config);
		TitleFont = IO.Fonts->AddFontFromMemoryTTF(Roboto, static_cast<int>(UmdFontRobotoSize), std::round(S(24.0f)), &Config);
		SmallFont = IO.Fonts->AddFontFromMemoryTTF(Roboto, static_cast<int>(UmdFontRobotoSize), std::round(S(13.0f)), &Config);
		MonoFont = IO.Fonts->AddFontFromMemoryTTF(Cousine, static_cast<int>(UmdFontCousineSize), std::round(S(13.5f)), &Config);

		IO.FontDefault = BodyFont;
	}

	void SetupStyle()
	{
		auto& Style = ImGui::GetStyle();
		Style = ImGuiStyle();

		Style.WindowPadding = ImVec2(22, 20);
		Style.FramePadding = ImVec2(10, 7);
		Style.CellPadding = ImVec2(10, 6);
		Style.ItemSpacing = ImVec2(10, 10);
		Style.ItemInnerSpacing = ImVec2(8, 6);
		Style.ScrollbarSize = 12;
		Style.GrabMinSize = 10;

		Style.WindowBorderSize = 0;
		Style.ChildBorderSize = 1;
		Style.FrameBorderSize = 0;
		Style.PopupBorderSize = 1;

		Style.WindowRounding = 0;
		Style.ChildRounding = 10;
		Style.FrameRounding = 7;
		Style.PopupRounding = 8;
		Style.ScrollbarRounding = 6;
		Style.GrabRounding = 6;
		Style.TabRounding = 6;

		auto Colors = Style.Colors;

		Colors[ImGuiCol_Text] = Palette::Text;
		Colors[ImGuiCol_TextDisabled] = Palette::Muted;
		Colors[ImGuiCol_WindowBg] = Palette::Background;
		Colors[ImGuiCol_ChildBg] = Palette::Card;
		Colors[ImGuiCol_PopupBg] = Palette::Card;
		Colors[ImGuiCol_Border] = Palette::Border;
		Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
		Colors[ImGuiCol_FrameBg] = ImVec4(0.141f, 0.157f, 0.188f, 1.0f);
		Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.169f, 0.188f, 0.227f, 1.0f);
		Colors[ImGuiCol_FrameBgActive] = ImVec4(0.196f, 0.220f, 0.267f, 1.0f);
		Colors[ImGuiCol_TitleBg] = Palette::Background;
		Colors[ImGuiCol_TitleBgActive] = Palette::Background;
		Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
		Colors[ImGuiCol_ScrollbarGrab] = Palette::Border;
		Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.251f, 0.275f, 0.322f, 1.0f);
		Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.302f, 0.329f, 0.384f, 1.0f);
		Colors[ImGuiCol_CheckMark] = Palette::Accent;
		Colors[ImGuiCol_Button] = ImVec4(0.161f, 0.180f, 0.216f, 1.0f);
		Colors[ImGuiCol_ButtonHovered] = ImVec4(0.204f, 0.227f, 0.271f, 1.0f);
		Colors[ImGuiCol_ButtonActive] = ImVec4(0.231f, 0.259f, 0.310f, 1.0f);
		Colors[ImGuiCol_Header] = Palette::Selected;
		Colors[ImGuiCol_HeaderHovered] = Palette::CardHover;
		Colors[ImGuiCol_HeaderActive] = Palette::Selected;
		Colors[ImGuiCol_Separator] = Palette::Border;
		Colors[ImGuiCol_TableHeaderBg] = Palette::Card;
		Colors[ImGuiCol_TableBorderLight] = Palette::Border;
		Colors[ImGuiCol_TableBorderStrong] = Palette::Border;
		Colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
		Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.015f);
		Colors[ImGuiCol_TextSelectedBg] = ImVec4(Palette::Accent.x, Palette::Accent.y, Palette::Accent.z, 0.35f);
		Colors[ImGuiCol_NavHighlight] = Palette::Accent;

		Style.ScaleAllSizes(Scale);
	}

	std::string FriendlyName(const std::string& Name)
	{
		auto Lower = Process::ToLower(Name);
		size_t Cut = Name.size();

		for (auto Marker : { "-win64-", "-wingdk-", "-linux-", "-linuxarm64-" })
		{
			auto Found = Lower.find(Marker);

			if (Found != std::string::npos)
				Cut = std::min(Cut, Found);
		}

		if (Cut == Name.size() && Lower.ends_with(".exe"))
			Cut = Name.size() - 4;

		auto Ret = Name.substr(0, Cut);
		std::replace(Ret.begin(), Ret.end(), '_', ' ');
		return Ret;
	}

	const char* PlatformLabel(const Process::FInfo& Info, ImVec4& OutColor)
	{
#ifdef _WIN32
		OutColor = Palette::Windows;
		return "Windows";
#else
		if (Info.bWine)
		{
			OutColor = Palette::Proton;
			return "Proton / Wine";
		}

		OutColor = Palette::Linux;
		return "Linux";
#endif
	}

	void DrawIcon(ImDrawList* DrawList, ImVec2 Center, float Radius, EIcon Icon, const ImVec4& Color)
	{
		DrawList->AddCircleFilled(Center, Radius, ToU32(Color, 0.18f), 32);
		DrawList->AddCircle(Center, Radius, ToU32(Color, 0.9f), 32, S(1.5f));

		auto Thickness = S(2.0f);
		auto R = Radius * 0.45f;
		auto C = ToU32(Color);

		switch (Icon)
		{
		case EIcon::Check:
		{
			ImVec2 Points[] = { { Center.x - R, Center.y }, { Center.x - R * 0.25f, Center.y + R * 0.7f }, { Center.x + R, Center.y - R * 0.6f } };
			DrawList->AddPolyline(Points, 3, C, 0, Thickness);
			break;
		}
		case EIcon::Cross:
			DrawList->AddLine({ Center.x - R * 0.8f, Center.y - R * 0.8f }, { Center.x + R * 0.8f, Center.y + R * 0.8f }, C, Thickness);
			DrawList->AddLine({ Center.x + R * 0.8f, Center.y - R * 0.8f }, { Center.x - R * 0.8f, Center.y + R * 0.8f }, C, Thickness);
			break;
		case EIcon::Warning:
		case EIcon::Info:
		{
			bool bWarning = Icon == EIcon::Warning;
			auto Top = bWarning ? Center.y - R : Center.y - R * 0.1f;
			auto Bottom = bWarning ? Center.y + R * 0.1f : Center.y + R;
			auto DotY = bWarning ? Center.y + R * 0.75f : Center.y - R * 0.75f;

			DrawList->AddLine({ Center.x, Top }, { Center.x, Bottom }, C, Thickness);
			DrawList->AddCircleFilled({ Center.x, DotY }, Thickness * 0.7f, C);
			break;
		}
		}
	}

	void DrawSpinner(ImDrawList* DrawList, ImVec2 Center, float Radius, const ImVec4& Color)
	{
		auto Time = static_cast<float>(ImGui::GetTime());
		auto Start = Time * 6.0f;

		DrawList->PathClear();
		DrawList->PathArcTo(Center, Radius, Start, Start + 4.2f, 24);
		DrawList->PathStroke(ToU32(Color), 0, S(2.5f));
	}

	void SectionLabel(const char* Text)
	{
		ImGui::PushFont(SmallFont);
		ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
		ImGui::TextUnformatted(Text);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	bool AccentButton(const char* Label, const ImVec2& Size, bool bEnabled)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, bEnabled ? Palette::Accent : ImVec4(0.180f, 0.200f, 0.243f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bEnabled ? Palette::AccentHover : ImVec4(0.180f, 0.200f, 0.243f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, bEnabled ? Palette::AccentActive : ImVec4(0.180f, 0.200f, 0.243f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, bEnabled ? ImVec4(1, 1, 1, 1) : Palette::Muted);

		ImGui::BeginDisabled(!bEnabled);
		bool bClicked = ImGui::Button(Label, Size);
		ImGui::EndDisabled();

		ImGui::PopStyleColor(4);
		return bClicked;
	}

	bool BeginBanner(const char* Id, const ImVec4& Color, EIcon Icon, const char* Title, const std::string& Text, int ButtonRows, ImFont* TextFont = nullptr)
	{
		if (!TextFont)
			TextFont = SmallFont;

		ImGui::PushFont(TextFont);
		auto Width = ImGui::GetContentRegionAvail().x;
		auto TextHeight = ImGui::CalcTextSize(Text.c_str(), nullptr, false, Width - S(70)).y;
		ImGui::PopFont();

		auto Height = S(18) + ImGui::GetTextLineHeight() + S(4) + TextHeight + S(16) + (ButtonRows ? ImGui::GetFrameHeight() + S(10) : 0);

		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(Color.x, Color.y, Color.z, 0.08f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(Color.x, Color.y, Color.z, 0.45f));

		ImGui::BeginChild(Id, ImVec2(0, Height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

		auto Origin = ImGui::GetCursorScreenPos();
		DrawIcon(ImGui::GetWindowDrawList(), ImVec2(Origin.x + S(14), Origin.y + S(12)), S(12), Icon, Color);

		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(38));
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		ImGui::TextUnformatted(Title);
		ImGui::PopStyleColor();

		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(38));
		ImGui::PushFont(TextFont);
		ImGui::PushStyleColor(ImGuiCol_Text, TextFont == MonoFont ? Palette::Text : Palette::Muted);
		ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(10));
		ImGui::TextUnformatted(Text.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopFont();

		if (ButtonRows)
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(38));

		return true;
	}

	void EndBanner()
	{
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
	}

	ImVec4 ClassifyLine(const std::string& Line)
	{
		auto Lower = Process::ToLower(Line);

		if (Lower.starts_with("possible aes key"))
			return Palette::Warning;

		if (Lower.find("successfully") != std::string::npos || Lower.starts_with("wrote ") ||
			Lower.starts_with("aes key:") || Lower.starts_with("dynamic aes key") || Lower.starts_with("no aes key is needed"))
			return Palette::Success;

		if (Lower.find("failed") != std::string::npos || Lower.find("could not") != std::string::npos ||
			Lower.find("not allowed") != std::string::npos || Lower.find("crashed") != std::string::npos ||
			Lower.find("error") != std::string::npos)
			return Palette::Error;

		if (Lower.find("warning") != std::string::npos || Lower.find("note:") != std::string::npos)
			return Palette::Warning;

		if (Lower.starts_with("found ") || Lower.starts_with("process:") || Lower.starts_with("executable:"))
			return Palette::Text;

		return Palette::Muted;
	}

	void AddLog(FState& State, std::string Line)
	{
		if (Line.starts_with("[=] "))
			Line.erase(0, 4);

		auto Color = ClassifyLine(Line);
		State.Log.push_back({ std::move(Line), Color });
		State.bScrollLog = true;
	}

	const Process::FInfo* GetSelected(const FState& State)
	{
		for (auto& Info : State.Processes)
		{
			if (Info.Pid == State.SelectedPid)
				return &Info;
		}

		return nullptr;
	}

	void ProbeAccess(FState& State)
	{
		auto Selected = GetSelected(State);

		if (!Selected)
		{
			State.Access = EAccess::Unknown;
			State.AccessPid = 0;
			return;
		}

		std::string Error;
		Process::EOpenError Code = Process::EOpenError::None;

		auto Backend = Process::OpenBackend(*Selected, Error, &Code);

		State.AccessPid = Selected->Pid;
		State.AccessError = Error;

		if (Backend)
			State.Access = EAccess::Ok;
		else if (Code == Process::EOpenError::PermissionDenied)
			State.Access = EAccess::Denied;
		else
			State.Access = EAccess::Failed;
	}

	void RefreshProcesses(FState& State)
	{
		State.LastRefresh = std::chrono::steady_clock::now();

		auto All = Process::List();
		std::vector<Process::FInfo> Visible;

		auto Filter = Process::ToLower(State.Filter);

		for (auto& Info : All)
		{
			if (!State.bShowAll && !Process::IsUnrealName(Info.Name))
				continue;

			if (!Filter.empty() && Process::ToLower(Info.Name).find(Filter) == std::string::npos)
				continue;

			Visible.push_back(std::move(Info));
		}

		std::stable_sort(Visible.begin(), Visible.end(), [](const Process::FInfo& A, const Process::FInfo& B)
			{
				return Process::IsUnrealName(A.Name) > Process::IsUnrealName(B.Name);
			});

		State.Processes = std::move(Visible);

		if (!GetSelected(State))
		{
			State.SelectedPid = 0;

			int UnrealCount = 0;
			int UnrealPid = 0;

			for (auto& Info : State.Processes)
			{
				if (Process::IsUnrealName(Info.Name))
				{
					UnrealCount++;
					UnrealPid = Info.Pid;
				}
			}

			if (UnrealCount == 1)
				State.SelectedPid = UnrealPid;
		}

		if (State.SelectedPid != State.AccessPid || State.Access == EAccess::Denied || State.Access == EAccess::Failed)
			ProbeAccess(State);
	}

	bool NeedsElevation(const FState& State)
	{
#ifdef _WIN32
		(void)State;
		return false;
#else
		return State.Access == EAccess::Denied || State.bAlwaysElevate;
#endif
	}

	void StartDump(FState& State, ETask Task = ETask::Dump)
	{
		auto Selected = GetSelected(State);

		if (!Selected || State.Task != ETask::None)
			return;

		std::vector<std::string> Args =
		{
			Memory::PathToUtf8(Process::GetSelfPath()),
			"--cli",
			"--pid",
			std::to_string(Selected->Pid),
		};

		if (Task == ETask::Aes)
		{
			Args.push_back("--aes");
		}
		else if (State.OutputPath[0])
		{
			Args.push_back("--output");
			Args.push_back(State.OutputPath);
		}

		State.Log.clear();
		State.Result = EResult::None;
		State.ResultPath.clear();
		State.Keys.clear();
		State.bNoKeyNeeded = false;
		State.bChildElevated = NeedsElevation(State);

		if (Task == ETask::Aes)
			AddLog(State, std::format("Searching the AES key of {} (pid {})", Selected->Name, Selected->Pid));
		else
			AddLog(State, std::format("Dumping {} (pid {})", Selected->Name, Selected->Pid));

		if (State.bChildElevated)
			AddLog(State, "Waiting for the password prompt...");

		State.Child = std::make_unique<Gui::System::FChildProcess>();

		std::string Error;

		if (!State.Child->Start(Args, State.bChildElevated, Error))
		{
			State.Child.reset();
			State.Result = EResult::Failed;
			State.ResultTitle = "Could not start the dump";
			State.ResultText = Error;
			return;
		}

		State.Task = Task;
		State.TaskStart = std::chrono::steady_clock::now();
	}

#ifndef _WIN32
	void StartGrant(FState& State)
	{
		auto Command = Gui::System::GetGrantPermissionCommand();

		if (Command.empty() || State.Task != ETask::None)
			return;

		State.Log.clear();
		State.Result = EResult::None;

		AddLog(State, "Allowing UnrealMappingsDumper to read the memory of other programs...");

		State.Child = std::make_unique<Gui::System::FChildProcess>();

		std::string Error;

		if (!State.Child->Start(Command, true, Error))
		{
			State.Child.reset();
			State.Result = EResult::Failed;
			State.ResultTitle = "Could not set the permission";
			State.ResultText = Error;
			return;
		}

		State.Task = ETask::Grant;
		State.bChildElevated = true;
		State.TaskStart = std::chrono::steady_clock::now();
	}
#endif

	void PollChild(FState& State)
	{
		if (!State.Child)
			return;

		for (auto& Line : State.Child->TakeLines())
		{
			auto Clean = Line.starts_with("[=] ") ? Line.substr(4) : Line;

			if (Clean.starts_with("Wrote "))
			{
				auto End = Clean.rfind(" (");
				State.ResultPath = std::filesystem::path(std::u8string(Clean.begin() + 6, End == std::string::npos ? Clean.end() : Clean.begin() + End));
			}
			else if (Clean.starts_with("AES key: "))
			{
				State.Keys.push_back({ "Main key", Clean.substr(9) });
			}
			else if (Clean.starts_with("Dynamic AES key ") && Clean.find(": ") != std::string::npos)
			{
				auto Colon = Clean.find(": ");
				State.Keys.push_back({ "Dynamic key " + Clean.substr(16, Colon - 16), Clean.substr(Colon + 2) });
			}
			else if (Clean.starts_with("Possible AES key: "))
			{
				State.Keys.push_back({ "Possible key", Clean.substr(18) });
			}
			else if (Clean.starts_with("No AES key is needed"))
			{
				State.bNoKeyNeeded = true;
			}

			AddLog(State, std::move(Line));
		}

		if (State.Child->IsRunning())
			return;

		for (auto& Line : State.Child->TakeLines())
			AddLog(State, std::move(Line));

		auto ExitCode = State.Child->GetExitCode();
		auto Task = State.Task;

		State.Child.reset();
		State.Task = ETask::None;

		bool bCancelled = State.bChildElevated && (ExitCode == 126 || ExitCode == 127);

		if (Task == ETask::Grant)
		{
			if (ExitCode == 0)
			{
				AddLog(State, "Permission granted, restarting...");
				Gui::System::Restart(false);
				return;
			}

			State.Result = bCancelled ? EResult::Cancelled : EResult::Failed;
			State.ResultTitle = bCancelled ? "Cancelled" : "Could not set the permission";
			State.ResultText = bCancelled
				? "The password prompt was cancelled."
				: "setcap failed. Drives like NTFS or FAT can't store the permission, move UnrealMappingsDumper into your home directory, or use \"Ask every time\".";
			return;
		}

		if (Task == ETask::Aes && !bCancelled)
		{
			std::string Keys;

			for (auto& Key : State.Keys)
			{
				if (!Keys.empty())
					Keys += "\n";

				if (State.Keys.size() > 1 && Key.Label != "Possible key")
					Keys += Key.Label + "\n";

				Keys += Key.Key;
			}

			if (ExitCode == 0 && !State.Keys.empty())
			{
				State.Result = EResult::Success;
				State.ResultTitle = State.Keys.size() > 1 ? "AES keys found" : "AES key found";
				State.ResultText = Keys;
			}
			else if (ExitCode == 0)
			{
				State.Result = EResult::Info;
				State.ResultTitle = "No AES key needed";
				State.ResultText = "The pak files of this game aren't encrypted.";
			}
			else if (ExitCode == 3)
			{
				State.Result = EResult::Warning;
				State.ResultTitle = "Possible AES keys";
				State.ResultText = Keys;
			}
			else
			{
				State.Result = EResult::Failed;
				State.ResultTitle = "No AES key found";
				State.ResultText = "See the log below for details. If you report this, please include the log.";
			}

			ProbeAccess(State);
			return;
		}

		if (ExitCode == 0)
		{
			State.Result = EResult::Success;
			State.ResultTitle = "Mappings written";
			State.ResultText = State.ResultPath.empty() ? std::string("Done.") : Memory::PathToUtf8(State.ResultPath);
		}
		else if (bCancelled)
		{
			State.Result = EResult::Cancelled;
			State.ResultTitle = "Cancelled";
			State.ResultText = Task == ETask::Aes ? "The password prompt was cancelled." : "The password prompt was cancelled, nothing was dumped.";
		}
		else
		{
			State.Result = EResult::Failed;
			State.ResultTitle = "Dumping failed";
			State.ResultText = "See the log below for details. If you report this, please include the log.";
		}

		ProbeAccess(State);
	}

	void DrawHeader()
	{
		auto DrawList = ImGui::GetWindowDrawList();
		auto Origin = ImGui::GetCursorScreenPos();
		auto Size = S(38);

		DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Size, Origin.y + Size), ToU32(Palette::Accent), S(9));

		auto Center = ImVec2(Origin.x + Size * 0.5f, Origin.y + Size * 0.5f);
		auto R = Size * 0.26f;

		ImVec2 Diamond[] = { { Center.x, Center.y - R }, { Center.x + R, Center.y }, { Center.x, Center.y + R }, { Center.x - R, Center.y } };
		DrawList->AddConvexPolyFilled(Diamond, 4, IM_COL32(255, 255, 255, 235));
		DrawList->AddCircleFilled(Center, R * 0.32f, ToU32(Palette::Accent));

		ImGui::SetCursorScreenPos(ImVec2(Origin.x + Size + S(14), Origin.y - S(3)));
		ImGui::PushFont(TitleFont);
		ImGui::TextUnformatted("Unreal Mappings Dumper");
		ImGui::PopFont();

		ImGui::SetCursorScreenPos(ImVec2(Origin.x + Size + S(14), ImGui::GetCursorScreenPos().y - S(8)));
		ImGui::PushFont(SmallFont);
		ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
		ImGui::TextUnformatted("Generate .usmap mappings from running Unreal Engine games");
		ImGui::PopStyleColor();

		auto Version = "v" UMD_VERSION;
		auto VersionWidth = ImGui::CalcTextSize(Version).x;
		auto Right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

		ImGui::GetWindowDrawList()->AddText(SmallFont, SmallFont->FontSize, ImVec2(Right - VersionWidth, Origin.y + S(4)), ToU32(Palette::Muted), Version);
		ImGui::PopFont();

		ImGui::SetCursorScreenPos(ImVec2(Origin.x, Origin.y + Size + S(18)));
	}

	void DrawGameList(FState& State)
	{
		auto LineY = ImGui::GetCursorPosY();
		auto FrameHeight = ImGui::GetFrameHeight();

		ImGui::PushFont(SmallFont);
		ImGui::SetCursorPosY(LineY + (FrameHeight - ImGui::GetTextLineHeight()) * 0.5f);
		ImGui::PopFont();
		SectionLabel("GAME");

		auto RefreshWidth = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2;
		auto CheckboxWidth = FrameHeight + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("All processes").x;
		auto SearchWidth = S(190);
		auto Spacing = ImGui::GetStyle().ItemSpacing.x;
		auto ToolbarWidth = SearchWidth + CheckboxWidth + RefreshWidth + Spacing * 2;

		ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - ToolbarWidth, LineY));

		ImGui::SetNextItemWidth(SearchWidth);

		if (ImGui::InputTextWithHint("##filter", "Search", State.Filter, sizeof(State.Filter)))
			RefreshProcesses(State);

		ImGui::SameLine();

		if (ImGui::Checkbox("All processes", &State.bShowAll))
			RefreshProcesses(State);

		ImGui::SameLine();

		if (ImGui::Button("Refresh"))
			RefreshProcesses(State);

		auto RowHeight = S(46);
		auto Needed = static_cast<float>(State.Processes.size()) * (RowHeight + ImGui::GetStyle().CellPadding.y * 2) + ImGui::GetStyle().WindowPadding.y * 2 + S(4);
		auto ListHeight = std::clamp(Needed, S(150), std::max(S(150), ImGui::GetContentRegionAvail().y * 0.4f));

		ImGui::BeginChild("##games", ImVec2(0, ListHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

		if (State.Processes.empty())
		{
			auto Region = ImGui::GetContentRegionAvail();
			auto Center = ImVec2(ImGui::GetCursorScreenPos().x + Region.x * 0.5f, ImGui::GetCursorScreenPos().y + Region.y * 0.5f);

			DrawSpinner(ImGui::GetWindowDrawList(), ImVec2(Center.x, Center.y - S(28)), S(11), Palette::Muted);

			const char* Title = State.bShowAll || State.Filter[0] ? "No matching process" : "No Unreal Engine game running";
			const char* Hint = "Start your game and wait until the main menu, it shows up here automatically.";

			auto TitleSize = ImGui::CalcTextSize(Title);
			ImGui::GetWindowDrawList()->AddText(ImVec2(Center.x - TitleSize.x * 0.5f, Center.y - S(6)), ToU32(Palette::Text), Title);

			auto HintSize = SmallFont->CalcTextSizeA(SmallFont->FontSize, FLT_MAX, 0, Hint);
			ImGui::GetWindowDrawList()->AddText(SmallFont, SmallFont->FontSize, ImVec2(Center.x - HintSize.x * 0.5f, Center.y + S(18)), ToU32(Palette::Muted), Hint);

			ImGui::EndChild();
			return;
		}

		if (ImGui::BeginTable("##gametable", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX, ImVec2(0, 0)))
		{
			ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Platform", ImGuiTableColumnFlags_WidthFixed, S(130));
			ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, S(70));

			for (auto& Info : State.Processes)
			{
				ImGui::PushID(Info.Pid);
				ImGui::TableNextRow(0, RowHeight);
				ImGui::TableSetColumnIndex(0);

				auto RowStart = ImGui::GetCursorScreenPos();
				bool bSelected = Info.Pid == State.SelectedPid;

				ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));

				if (ImGui::Selectable("##row", bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, RowHeight)))
				{
					if (State.SelectedPid != Info.Pid)
					{
						State.SelectedPid = Info.Pid;
						ProbeAccess(State);
					}

					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && State.Access != EAccess::Failed)
						StartDump(State);
				}

				ImGui::PopStyleVar();

				auto DrawList = ImGui::GetWindowDrawList();

				if (bSelected)
					DrawList->AddRectFilled(RowStart, ImVec2(RowStart.x + S(3), RowStart.y + RowHeight), ToU32(Palette::Accent), S(2));

				auto Friendly = FriendlyName(Info.Name);
				DrawList->AddText(BodyFont, BodyFont->FontSize, ImVec2(RowStart.x + S(12), RowStart.y + S(6)), ToU32(Palette::Text), Friendly.c_str());
				DrawList->AddText(SmallFont, SmallFont->FontSize, ImVec2(RowStart.x + S(12), RowStart.y + S(26)), ToU32(Palette::Muted), Info.Name.c_str());

				ImGui::TableSetColumnIndex(1);

				ImVec4 BadgeColor;
				auto Label = PlatformLabel(Info, BadgeColor);
				auto BadgePos = ImGui::GetCursorScreenPos();
				auto LabelSize = SmallFont->CalcTextSizeA(SmallFont->FontSize, FLT_MAX, 0, Label);
				auto BadgeMin = ImVec2(BadgePos.x, BadgePos.y + (RowHeight - LabelSize.y - S(8)) * 0.5f);
				auto BadgeMax = ImVec2(BadgeMin.x + LabelSize.x + S(16), BadgeMin.y + LabelSize.y + S(8));

				DrawList->AddRectFilled(BadgeMin, BadgeMax, ToU32(BadgeColor, 0.16f), S(20));
				DrawList->AddText(SmallFont, SmallFont->FontSize, ImVec2(BadgeMin.x + S(8), BadgeMin.y + S(4)), ToU32(BadgeColor), Label);

				ImGui::TableSetColumnIndex(2);

				auto Pid = std::to_string(Info.Pid);
				auto PidPos = ImGui::GetCursorScreenPos();
				auto PidSize = SmallFont->CalcTextSizeA(SmallFont->FontSize, FLT_MAX, 0, Pid.c_str());
				DrawList->AddText(SmallFont, SmallFont->FontSize, ImVec2(PidPos.x, PidPos.y + (RowHeight - PidSize.y) * 0.5f), ToU32(Palette::Muted), Pid.c_str());

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::EndChild();
	}

	void DrawAccessBanner(FState& State)
	{
		if (!GetSelected(State) || State.Task != ETask::None)
			return;

		if (State.Access == EAccess::Failed)
		{
			BeginBanner("##accessfailed", Palette::Error, EIcon::Cross, "This process can't be dumped", State.AccessError, 0);
			EndBanner();
			return;
		}

		if (State.Access != EAccess::Denied || State.bAlwaysElevate)
			return;

#ifdef _WIN32
		BeginBanner("##accessdenied", Palette::Warning, EIcon::Warning, "Administrator rights needed",
			"Windows doesn't allow reading the memory of this game with your current rights.", 1);

		if (AccentButton("Restart as administrator", ImVec2(0, 0), true))
			Gui::System::Restart(true);

		EndBanner();
#else
		bool bCanElevate = Gui::System::CanElevateChild();

		BeginBanner("##accessdenied", Palette::Warning, EIcon::Warning, "Permission needed",
			bCanElevate
			? "Linux only lets programs read the memory of other programs with extra permission. Allow it once for UnrealMappingsDumper, or confirm with your password for every dump."
			: "Linux only lets programs read the memory of other programs with extra permission. Run it from a terminal with: sudo ./unreal-mappings-dumper",
			bCanElevate ? 1 : 0);

		if (bCanElevate)
		{
			if (AccentButton("Allow permanently", ImVec2(0, 0), true))
				StartGrant(State);

			ImGui::SameLine();

			if (ImGui::Button("Ask every time"))
				State.bAlwaysElevate = true;
		}

		EndBanner();
#endif
	}

	void DrawOutput(FState& State)
	{
		SectionLabel("OUTPUT");

		auto ButtonWidth = State.OutputPath[0] ? ImGui::CalcTextSize("Default").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x : 0;

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth);
		ImGui::InputTextWithHint("##output", "Mappings.usmap next to the game executable", State.OutputPath, sizeof(State.OutputPath));

		if (State.OutputPath[0])
		{
			ImGui::SameLine();

			if (ImGui::Button("Default"))
				State.OutputPath[0] = '\0';
		}
	}

	void DrawDumpButton(FState& State)
	{
		bool bBusy = State.Task != ETask::None;
		bool bCanDump = GetSelected(State) && !bBusy && State.Access != EAccess::Failed &&
			(State.Access != EAccess::Denied || State.bAlwaysElevate);

		auto Size = ImVec2(ImGui::GetContentRegionAvail().x, S(46));

		if (bBusy)
		{
			auto Pos = ImGui::GetCursorScreenPos();
			AccentButton("##busy", Size, false);

			auto Label = State.Task == ETask::Grant ? "Waiting for permission..." : State.Task == ETask::Aes ? "Searching for the AES key..." : "Dumping mappings...";
			auto LabelSize = ImGui::CalcTextSize(Label);
			auto Start = ImVec2(Pos.x + (Size.x - LabelSize.x) * 0.5f + S(14), Pos.y + (Size.y - LabelSize.y) * 0.5f);

			DrawSpinner(ImGui::GetWindowDrawList(), ImVec2(Start.x - S(18), Pos.y + Size.y * 0.5f), S(8), Palette::Text);
			ImGui::GetWindowDrawList()->AddText(Start, ToU32(Palette::Text), Label);
			return;
		}

		auto AesWidth = S(170);
		Size.x -= AesWidth + ImGui::GetStyle().ItemSpacing.x;

		ImGui::PushFont(BodyFont);

		if (AccentButton(State.bAlwaysElevate && State.Access == EAccess::Denied ? "Dump mappings (asks for your password)" : "Dump mappings", Size, bCanDump))
			StartDump(State);

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, bCanDump ? Palette::Text : Palette::Muted);
		ImGui::BeginDisabled(!bCanDump);

		if (ImGui::Button("Find AES key", ImVec2(AesWidth, Size.y)))
			StartDump(State, ETask::Aes);

		ImGui::EndDisabled();
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	void DrawResult(FState& State)
	{
		if (State.Result == EResult::None || State.Task != ETask::None)
			return;

		auto CopyKeys = [&]
			{
				if (State.Keys.empty())
					return;

				if (ImGui::Button(State.Keys.size() > 1 ? "Copy keys" : "Copy key"))
				{
					std::string All;

					for (auto& Key : State.Keys)
						All += (State.Keys.size() > 1 && Key.Label != "Possible key" ? Key.Label + ": " : std::string()) + Key.Key + (State.Keys.size() > 1 ? "\n" : "");

					ImGui::SetClipboardText(All.c_str());
				}
			};

		switch (State.Result)
		{
		case EResult::Success:
		case EResult::Warning:
		{
			if (!State.Keys.empty())
			{
				bool bWarning = State.Result == EResult::Warning;
				BeginBanner("##result", bWarning ? Palette::Warning : Palette::Success, bWarning ? EIcon::Warning : EIcon::Check, State.ResultTitle.c_str(), State.ResultText, 1, MonoFont);
				CopyKeys();

				if (bWarning)
				{
					ImGui::SameLine();
					ImGui::PushFont(SmallFont);
					ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted("The pak files couldn't be found or checked, one of these is likely the key.");
					ImGui::PopStyleColor();
					ImGui::PopFont();
				}

				EndBanner();
				break;
			}

			BeginBanner("##result", Palette::Success, EIcon::Check, State.ResultTitle.c_str(), State.ResultText, State.ResultPath.empty() ? 0 : 1);

			if (!State.ResultPath.empty())
			{
				if (ImGui::Button("Open folder"))
					Gui::System::OpenFolder(State.ResultPath);

				ImGui::SameLine();

				if (ImGui::Button("Copy path"))
					ImGui::SetClipboardText(Memory::PathToUtf8(State.ResultPath).c_str());
			}

			EndBanner();
			break;
		}
		case EResult::Info:
			BeginBanner("##result", Palette::Accent, EIcon::Info, State.ResultTitle.c_str(), State.ResultText, 0);
			EndBanner();
			break;
		case EResult::Cancelled:
			BeginBanner("##result", Palette::Muted, EIcon::Info, State.ResultTitle.c_str(), State.ResultText, 0);
			EndBanner();
			break;
		default:
		{
			BeginBanner("##result", Palette::Error, EIcon::Cross, State.ResultTitle.c_str(), State.ResultText, 1);

			if (ImGui::Button("Copy log"))
			{
				std::string All;

				for (auto& Line : State.Log)
					All += Line.Text + "\n";

				ImGui::SetClipboardText(All.c_str());
			}

			EndBanner();
			break;
		}
		}
	}

	void DrawLog(FState& State)
	{
		SectionLabel("LOG");

		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.086f, 0.094f, 0.114f, 1.0f));
		ImGui::BeginChild("##log", ImVec2(0, std::max(S(90), ImGui::GetContentRegionAvail().y)), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		ImGui::PushFont(MonoFont);

		if (State.Log.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
			ImGui::TextUnformatted("Select a game and click \"Dump mappings\" or \"Find AES key\". The output shows up here.");
			ImGui::PopStyleColor();
		}

		ImGuiListClipper Clipper;
		Clipper.Begin(static_cast<int>(State.Log.size()));

		while (Clipper.Step())
		{
			for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; i++)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, State.Log[i].Color);
				ImGui::TextUnformatted(State.Log[i].Text.c_str());
				ImGui::PopStyleColor();
			}
		}

		if (State.bScrollLog)
		{
			ImGui::SetScrollHereY(1.0f);
			State.bScrollLog = false;
		}

		ImGui::PopFont();
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	void DrawFrame(FState& State)
	{
		auto Viewport = ImGui::GetMainViewport();

		ImGui::SetNextWindowPos(Viewport->WorkPos);
		ImGui::SetNextWindowSize(Viewport->WorkSize);

		ImGui::Begin("##main", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

		DrawHeader();
		DrawGameList(State);
		DrawAccessBanner(State);
		DrawOutput(State);
		DrawDumpButton(State);
		DrawResult(State);
		DrawLog(State);

		ImGui::End();
	}
}

int Gui::Run()
{
	if (!Window::Create("Unreal Mappings Dumper", 920, 700))
		return -1;

	ImGui::GetIO().IniFilename = nullptr;

	Scale = Window::GetContentScale();

	if (auto Override = getenv("UMD_UI_SCALE"))
		Scale = std::max(0.5f, static_cast<float>(atof(Override)));

	LoadFonts();
	SetupStyle();

	FState State;
	RefreshProcesses(State);

	const float Clear[4] = { Palette::Background.x, Palette::Background.y, Palette::Background.z, 1.0f };

	while (Window::PumpEvents(State.Task != ETask::None || State.Processes.empty()))
	{
		PollChild(State);

		if (State.Task == ETask::None && std::chrono::steady_clock::now() - State.LastRefresh > std::chrono::seconds(2))
			RefreshProcesses(State);

		Window::BeginFrame();
		DrawFrame(State);
		Window::EndFrame(Clear);
	}

	Window::Destroy();
	return 0;
}
