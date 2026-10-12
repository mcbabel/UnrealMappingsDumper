#include "../pch.h"

#include "Window.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"

#include <GLFW/glfw3.h>

namespace
{
	GLFWwindow* MainWindow = nullptr;
}

bool Gui::Window::Create(const char* Title, int Width, int Height)
{
	if (!glfwInit())
		return false;

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

	MainWindow = glfwCreateWindow(Width, Height, Title, nullptr, nullptr);

	if (!MainWindow)
	{
		glfwTerminate();
		return false;
	}

	glfwMakeContextCurrent(MainWindow);
	glfwSwapInterval(1);

	ImGui::CreateContext();

	ImGui_ImplGlfw_InitForOpenGL(MainWindow, true);
	ImGui_ImplOpenGL3_Init("#version 130");

	return true;
}

bool Gui::Window::PumpEvents(bool bAnimating)
{
	glfwWaitEventsTimeout(bAnimating ? 1.0 / 60.0 : 0.5);
	return !glfwWindowShouldClose(MainWindow);
}

void Gui::Window::BeginFrame()
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}

void Gui::Window::EndFrame(const float ClearColor[4])
{
	ImGui::Render();

	int Width = 0, Height = 0;
	glfwGetFramebufferSize(MainWindow, &Width, &Height);

	glViewport(0, 0, Width, Height);
	glClearColor(ClearColor[0], ClearColor[1], ClearColor[2], ClearColor[3]);
	glClear(GL_COLOR_BUFFER_BIT);

	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	glfwSwapBuffers(MainWindow);
}

void Gui::Window::Destroy()
{
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(MainWindow);
	glfwTerminate();
	MainWindow = nullptr;
}

float Gui::Window::GetContentScale()
{
	float X = 1.0f, Y = 1.0f;
	glfwGetWindowContentScale(MainWindow, &X, &Y);
	return std::max(1.0f, X);
}
