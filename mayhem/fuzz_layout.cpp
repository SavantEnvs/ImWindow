// libFuzzer harness for ImWindow's layout loader: each input is the text of a saved layout, handed to
// ImwWindowManager::LoadLayoutFromString (JsonStthm parser -> ImwPlatformWindow::Load /
// ImwContainer::Load, which rebuilds the dock tree and creates windows by class name). A few
// update/render frames and SaveLayoutToString then walk the loaded tree.
//
// Why the string entry and not LoadLayoutFromFile: JsonStthm's JsonValue::Parse tests every value with
// memcmp(p, "-Infinity", 9) and friends before looking at its first byte, so whenever a value starts
// fewer than 9 bytes before the terminating NUL it compares past the end of the buffer. ASan reports
// that (heap-buffer-overflow in memcmp) for almost every short input, LoadLayoutFromFile's buffer is
// exactly file size + 1, and the fuzzer can then never get past the parser. The input is therefore
// passed as the same C string LoadLayoutFromFile builds (bytes + NUL), followed by 15 more NUL bytes of
// allocation slack so those fixed-length compares stay in bounds. The string the library sees is
// unchanged; any read past the slack is still caught. The over-read itself is reported separately.
//
// Per-input state: a fresh manager (and so fresh ImGui contexts) is created and destroyed for every
// input. Only Dear ImGui's global default font atlas survives across inputs; it is built once from the
// built-in font and never changed by the input.
#include "ImwWindowManager.h"
#include "ImwWindow.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ImWindow;

namespace {
// Headless manager: the stock ImwWindowManager only creates the main platform window, while every
// shipped backend (DX11, OGL, GLFW, BGFX, Sokol) supports secondary windows. This mirrors a backend
// with plain, OS-less ImwPlatformWindow objects so layouts with "PlatformWindows" load as in an app.
// Each platform window keeps its own position/size/state like an OS window does; sizes are clamped to
// what a window system would grant (0..16384), as the backends get them back from the OS.
struct HeadlessPlatformWindow : ImwPlatformWindow {
	ImVec2 m_oPos, m_oSize;
	bool m_bMaximized, m_bMinimized;
	explicit HeadlessPlatformWindow(EPlatformWindowType eType)
		: ImwPlatformWindow(eType, true), m_oPos(0, 0), m_oSize(1280, 720), m_bMaximized(false), m_bMinimized(false) {}
	static float Clamp(int i) { return (float)(i < 0 ? 0 : (i > 16384 ? 16384 : i)); }
	ImVec2 GetPosition() const override { return m_oPos; }
	ImVec2 GetSize() const override { return m_oSize; }
	bool IsWindowMaximized() const override { return m_bMaximized; }
	bool IsWindowMinimized() const override { return m_bMinimized; }
	void SetSize(int iWidth, int iHeight) override { m_oSize = ImVec2(Clamp(iWidth), Clamp(iHeight)); }
	void SetPosition(int iX, int iY) override { m_oPos = ImVec2((float)iX, (float)iY); }
	void SetWindowMaximized(bool b) override { m_bMaximized = b; }
	void SetWindowMinimized(bool b) override { m_bMinimized = b; }
};
struct HeadlessManager : ImwWindowManager {
	bool CanCreateMultipleWindow() override { return true; }
	ImwPlatformWindow* CreatePlatformWindow(EPlatformWindowType eType, ImwPlatformWindow*) override
	{
		return new HeadlessPlatformWindow(eType);
	}
};
struct FuzzWindow : ImwWindow {
	void OnGui() override { ImGui::Text("%s", GetTitle() ? GetTitle() : ""); }
};
// Any class name the layout names can be instantiated, so the loader goes past its integrity pass.
bool CanCreate(const char* pName) { return pName != NULL; }
ImwWindow* Create(const char*) { return new FuzzWindow(); }
const char* ClassName(ImwWindow*) { return "FuzzWindow"; }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* pData, size_t iSize)
{
	std::vector<char> oText(pData, pData + iSize);
	oText.resize(iSize + 16, '\0');

	{
		HeadlessManager oMgr;
		if (!oMgr.Init()) abort();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(1280, 720);
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char* pPixels; int iW, iH;
		io.Fonts->GetTexDataAsRGBA32(&pPixels, &iW, &iH);

		ImwWindowManager::ClassNameFunctions oFuncs;
		oFuncs.m_pGetClassName = ClassName;
		oFuncs.m_pCanCreateWindowByClassName = CanCreate;
		oFuncs.m_pCreateWindowByClassName = Create;
		oMgr.SetClassNameFunctions(&oFuncs);

		oMgr.Dock(new FuzzWindow());
		oMgr.Run(false); oMgr.Run(true);

		// Whether or not the layout was accepted, the application keeps running and saves its layout
		// (typically on exit), so the state the loader left behind is always exercised.
		oMgr.LoadLayoutFromString(oText.data());
		for (int i = 0; i < 2; ++i) { oMgr.Run(false); oMgr.Run(true); }
		ImwString sOut;
		oMgr.SaveLayoutToString(sOut);
		oMgr.Destroy();
	}
	return 0;
}
