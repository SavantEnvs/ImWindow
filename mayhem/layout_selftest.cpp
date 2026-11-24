// Functional oracle for ImWindow's layout serialization (no sanitizers): loads known layouts through
// LoadLayoutFromFile / LoadLayoutFromString and asserts what was loaded and what SaveLayoutToString
// writes back. Prints one "PASS <name>" / "FAIL <name>: why" line per check.
#include "ImwWindowManager.h"
#include "ImwWindow.h"
#include <cstdio>
#include <cstring>
#include <string>

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
int g_iPass = 0, g_iFail = 0;
std::string g_sLastClass;
struct TestWindow : ImwWindow {
	std::string m_sClass;
	void OnGui() override { ImGui::Text("t"); }
};
bool CanCreate(const char* p) { return p && (strcmp(p, "A") == 0 || strcmp(p, "B") == 0 || strcmp(p, "C") == 0 || strcmp(p, "S") == 0); }
ImwWindow* Create(const char* p) { TestWindow* w = new TestWindow(); w->m_sClass = p; return w; }
const char* ClassName(ImwWindow* w) { return static_cast<TestWindow*>(w)->m_sClass.c_str(); }

void Check(const char* pName, bool b, const std::string& sWhy = "")
{
	if (b) { ++g_iPass; printf("PASS %s\n", pName); }
	else { ++g_iFail; printf("FAIL %s: %s\n", pName, sWhy.c_str()); }
}

struct Mgr {
	HeadlessManager o;
	Mgr() {
		o.Init();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(1280, 720);
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char* p; int w, h; io.Fonts->GetTexDataAsRGBA32(&p, &w, &h);
		ImwWindowManager::ClassNameFunctions f;
		f.m_pGetClassName = ClassName; f.m_pCanCreateWindowByClassName = CanCreate; f.m_pCreateWindowByClassName = Create;
		o.SetClassNameFunctions(&f);
		o.Run(false); o.Run(true);
	}
	~Mgr() { o.Destroy(); }
	std::string Save() { ImwString s; o.SaveLayoutToString(s, true); return s; }
};

int CountOf(const std::string& s, const char* p)
{
	int n = 0; size_t i = 0, l = strlen(p);
	while ((i = s.find(p, i)) != std::string::npos) { ++n; i += l; }
	return n;
}
}

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <dir with valid.json>\n", argv[0]); return 2; }
	std::string sDir = argv[1];
	std::string sSaved;
	setvbuf(stdout, NULL, _IONBF, 0);

	{
		Mgr m;
		bool b = m.o.LoadLayoutFromFile((sDir + "/valid.json").c_str());
		Check("load_valid_file", b, "LoadLayoutFromFile returned false");
		for (int i = 0; i < 2; ++i) { m.o.Run(false); m.o.Run(true); }
		std::string s = m.Save();
		Check("save_has_three_windows", CountOf(s, "\"Class\"") == 3, "saved layout: " + s);
		Check("save_keeps_classes", s.find("\"A\"") != std::string::npos && s.find("\"B\"") != std::string::npos && s.find("\"C\"") != std::string::npos, s);
		Check("save_keeps_split", s.find("\"Splits\"") != std::string::npos && s.find("\"Vertical\":true") != std::string::npos, s);
		Check("save_keeps_split_ratio", s.find("\"SplitRatio\":0.30000001192092896") != std::string::npos && s.find("\"SplitRatio\":0.5") != std::string::npos, s);
		Check("save_keeps_size", s.find("\"Width\":800") != std::string::npos && s.find("\"Height\":600") != std::string::npos, s);
		Check("windows_created", m.o.GetWindowList().size() == 3, "window count " + std::to_string(m.o.GetWindowList().size()));
		sSaved = s;
	}
	{
		// Round trip: what was saved loads again into the same layout. (One manager at a time: the
		// manager is a singleton, ImwWindowManager::GetInstance.)
		Mgr m2;
		bool b2 = m2.o.LoadLayoutFromString(sSaved.c_str());
		m2.o.Run(false); m2.o.Run(true);
		Check("roundtrip_load", b2, "reload of saved layout failed");
		Check("roundtrip_equal", m2.Save() == sSaved, "second save differs");
	}
	{
		Mgr m;
		bool b = m.o.LoadLayoutFromFile((sDir + "/platform.json").c_str());
		m.o.Run(false); m.o.Run(true);
		std::string s = m.Save();
		Check("load_platform_windows", b, "layout with a secondary platform window rejected");
		Check("platform_windows_saved", CountOf(s, "\"Class\"") == 3 && s.find("\"SpecialWindow\":{\"Class\":\"S\"}") != std::string::npos && s.find("\"Width\":300") != std::string::npos, s);
	}
	{
		Mgr m;
		Check("reject_truncated", !m.o.LoadLayoutFromString("{\"MainPlatformWindow\":{\"Width\":800,"), "truncated JSON accepted");
		Check("reject_unknown_class", !m.o.LoadLayoutFromString(
			"{\"MainPlatformWindow\":{\"Width\":8,\"Height\":6,\"Left\":0,\"Top\":0,\"Mode\":0,"
			"\"Container\":{\"Vertical\":true,\"SplitRatio\":0.5,\"Windows\":[{\"Class\":\"Z\"}]}},\"PlatformWindows\":[]}          "),
			"unknown window class accepted");
		Check("reject_missing_file", !m.o.LoadLayoutFromFile((sDir + "/does-not-exist.json").c_str()), "missing file accepted");
		Check("reject_not_object", !m.o.LoadLayoutFromString("[1,2,3]          "), "array root accepted");
	}
	printf("SUMMARY passed=%d failed=%d\n", g_iPass, g_iFail);
	return g_iFail ? 1 : 0;
}
