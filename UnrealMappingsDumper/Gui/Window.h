#pragma once

namespace Gui::Window
{
	bool Create(const char* Title, int Width, int Height);
	bool PumpEvents(bool bAnimating);
	void BeginFrame();
	void EndFrame(const float ClearColor[4]);
	void Destroy();
	float GetContentScale();
}
