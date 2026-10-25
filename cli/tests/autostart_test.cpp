#include "autostart.hpp"

#include <gtest/gtest.h>
#ifdef _WIN32
#include <windows.h>
// After windows.h.
#include <shellapi.h>
#else
#include <sys/stat.h>
#endif

#include <cstring>
#include <fstream>
#include <sstream>

#include "temp_dir.hpp"

namespace ghostdrive::autostart {
namespace {

Spec spec_with(std::string dir) {
    Spec s;
    s.daemon = "/opt/" + dir + "/bin/ghostdrived";
    s.data = "/Users/me/" + dir + "/data";
    s.mount = "/Users/me/" + dir + "/mnt";
    s.log = "/Users/me/" + dir + "/data/ghostdrived.log";
    return s;
}

TEST(Autostart, IdIsStableAndPerDirectory) {
    auto a = data_dir_id("/Users/me/Library/Application Support/GhostDrive");
    EXPECT_EQ(a.size(), 16u);
    EXPECT_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(a, data_dir_id("/Users/me/Library/Application Support/GhostDrive"));
    EXPECT_NE(a, data_dir_id("/Users/me/other"));
    EXPECT_EQ(launchd_label("/x"), "io.ghostdrive.daemon." + data_dir_id("/x"));
    EXPECT_EQ(systemd_unit_name("/x"), "ghostdrive-" + data_dir_id("/x") + ".service");
}

TEST(Autostart, Paths) {
    EXPECT_EQ(
        launch_agent_path("/Users/me", "io.ghostdrive.daemon.ab"),
        std::filesystem::path("/Users/me/Library/LaunchAgents/io.ghostdrive.daemon.ab.plist"));
    EXPECT_EQ(systemd_unit_path("/home/me/.config", "ghostdrive-ab.service"),
              std::filesystem::path("/home/me/.config/systemd/user/ghostdrive-ab.service"));
}

TEST(Autostart, XmlEscape) {
    auto r = xml_escape("a&b<c>d\"e'f g");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), "a&amp;b&lt;c&gt;d&quot;e&apos;f g");
    EXPECT_FALSE(xml_escape("a\nb"));
    EXPECT_FALSE(xml_escape(std::string("a\0b", 3)));
    EXPECT_FALSE(xml_escape("a\x7f"));
    auto utf8 = xml_escape("/Users/me/caf\xc3\xa9");
    ASSERT_TRUE(utf8);
    EXPECT_EQ(utf8.value(), "/Users/me/caf\xc3\xa9");
}

TEST(Autostart, SystemdQuote) {
    auto r = systemd_quote("/home/me/My \"Drive\" 100% $HOME \\x;");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), "\"/home/me/My \\\"Drive\\\" 100%% $$HOME \\\\x;\"");
    EXPECT_FALSE(systemd_quote("a\tb"));
    EXPECT_FALSE(systemd_quote("a\nb"));
}

TEST(Autostart, PlistRunsTheDaemonWithItsPaths) {
    auto spec = spec_with("a & <b>");
    auto plist = launch_agent_plist("io.ghostdrive.daemon.0123", spec);
    ASSERT_TRUE(plist) << plist.error().message;
    const std::string expected =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Label</key>\n"
        "\t<string>io.ghostdrive.daemon.0123</string>\n"
        "\t<key>ProgramArguments</key>\n"
        "\t<array>\n"
        "\t\t<string>/opt/a &amp; &lt;b&gt;/bin/ghostdrived</string>\n"
        "\t\t<string>--data</string>\n"
        "\t\t<string>/Users/me/a &amp; &lt;b&gt;/data</string>\n"
        "\t\t<string>--mount</string>\n"
        "\t\t<string>/Users/me/a &amp; &lt;b&gt;/mnt</string>\n"
        "\t</array>\n"
        "\t<key>RunAtLoad</key>\n"
        "\t<true/>\n"
        "\t<key>KeepAlive</key>\n"
        "\t<dict>\n"
        "\t\t<key>SuccessfulExit</key>\n"
        "\t\t<false/>\n"
        "\t</dict>\n"
        "\t<key>ExitTimeOut</key>\n"
        "\t<integer>30</integer>\n"
        "\t<key>ProcessType</key>\n"
        "\t<string>Standard</string>\n"
        "\t<key>StandardOutPath</key>\n"
        "\t<string>/Users/me/a &amp; &lt;b&gt;/data/ghostdrived.log</string>\n"
        "\t<key>StandardErrorPath</key>\n"
        "\t<string>/Users/me/a &amp; &lt;b&gt;/data/ghostdrived.log</string>\n"
        "</dict>\n"
        "</plist>\n";
    EXPECT_EQ(plist.value(), expected);
}

TEST(Autostart, PlistRejectsControlCharacters) {
    auto spec = spec_with("x");
    spec.mount = "/Users/me/evil\n</string>";
    EXPECT_FALSE(launch_agent_plist("io.ghostdrive.daemon.0123", spec));
}

TEST(Autostart, UnitRunsTheDaemonWithItsPaths) {
    auto spec = spec_with("My Drive %h $x");
    auto unit = systemd_unit(spec);
    ASSERT_TRUE(unit) << unit.error().message;
    const std::string expected =
        "[Unit]\n"
        "Description=GhostDrive daemon\n"
        "\n"
        "[Service]\n"
        "ExecStart=\"/opt/My Drive %%h $$x/bin/ghostdrived\" --data \"/Users/me/My Drive %%h "
        "$$x/data\" --mount \"/Users/me/My Drive %%h $$x/mnt\"\n"
        "Restart=on-failure\n"
        "RestartSec=10\n"
        "TimeoutStopSec=30\n"
        "\n"
        "[Install]\n"
        "WantedBy=default.target\n";
    EXPECT_EQ(unit.value(), expected);
}

TEST(Autostart, UnitRejectsControlCharacters) {
    auto spec = spec_with("x");
    spec.data = "/home/me/a\n[Service]\nExecStartPre=/bin/sh";
    EXPECT_FALSE(systemd_unit(spec));
}

TEST(Autostart, TaskName) {
    EXPECT_EQ(task_name("/x"), "GhostDrive-" + data_dir_id("/x"));
}

TEST(Autostart, WindowsQuote) {
    auto q = [](std::string_view text) {
        auto r = windows_quote(text);
        EXPECT_TRUE(r);
        return r ? r.value() : std::string();
    };
    EXPECT_EQ(q("C:\\Users\\me\\My Drive"), "\"C:\\Users\\me\\My Drive\"");
    EXPECT_EQ(q(""), "\"\"");
    // Before the closing quote, and before a literal quote, backslashes are doubled.
    EXPECT_EQ(q("C:\\"), "\"C:\\\\\"");
    EXPECT_EQ(q("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(q("a\\\"b"), "\"a\\\\\\\"b\"");
    EXPECT_EQ(q("a\\\\b"), "\"a\\\\b\"");
    EXPECT_FALSE(windows_quote("a\nb"));
    EXPECT_FALSE(windows_quote("a\tb"));
}

#ifdef _WIN32
// The quoting is what Windows itself parses back.
TEST(Autostart, WindowsQuoteRoundTrips) {
    for (std::string text : {"C:\\Program Files\\GhostDrive", "C:\\", "X:", "a \"b\" c",
                             "trailing\\\\", "\\\\server\\share\\", "caf\xc3\xa9 & %x%"}) {
        auto quoted = windows_quote(text);
        ASSERT_TRUE(quoted);
        std::string line = "ghostdrived.exe " + quoted.value() + " --next";
        int wide_size = ::MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), wide_size);
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(wide.c_str(), &argc);
        ASSERT_NE(argv, nullptr);
        ASSERT_EQ(argc, 3) << line;
        int size = ::WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
        std::string back(static_cast<std::size_t>(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, back.data(), size, nullptr, nullptr);
        back.pop_back();
        EXPECT_EQ(back, text) << line;
        EXPECT_EQ(std::wstring(argv[2]), L"--next");
        ::LocalFree(argv);
    }
}
#endif

TEST(Autostart, TaskRunsTheDaemonAsTheUser) {
    Spec spec;
    spec.daemon = "C:\\Program Files\\GhostDrive & <Co>\\ghostdrived.exe";
    spec.data = "C:\\Users\\Me Too\\AppData\\Local\\GhostDrive";
    spec.mount = "X:\\";
    WindowsUser user{"S-1-5-21-1-2-3-1001", "PC\\me"};
    auto xml = task_xml(spec, user);
    ASSERT_TRUE(xml) << xml.error().message;
    const std::string expected =
        "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        "  <RegistrationInfo>\n"
        "    <Description>Starts the GhostDrive daemon at logon.</Description>\n"
        "  </RegistrationInfo>\n"
        "  <Triggers>\n"
        "    <LogonTrigger>\n"
        "      <Enabled>true</Enabled>\n"
        "      <UserId>PC\\me</UserId>\n"
        "    </LogonTrigger>\n"
        "  </Triggers>\n"
        "  <Principals>\n"
        "    <Principal id=\"Author\">\n"
        "      <UserId>S-1-5-21-1-2-3-1001</UserId>\n"
        "      <LogonType>InteractiveToken</LogonType>\n"
        "      <RunLevel>LeastPrivilege</RunLevel>\n"
        "    </Principal>\n"
        "  </Principals>\n"
        "  <Settings>\n"
        "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
        "    <RestartOnFailure>\n"
        "      <Interval>PT1M</Interval>\n"
        "      <Count>3</Count>\n"
        "    </RestartOnFailure>\n"
        "  </Settings>\n"
        "  <Actions Context=\"Author\">\n"
        "    <Exec>\n"
        "      <Command>C:\\Program Files\\GhostDrive &amp; &lt;Co&gt;\\ghostdrived.exe</Command>\n"
        "      <Arguments>--data &quot;C:\\Users\\Me Too\\AppData\\Local\\GhostDrive&quot; --mount "
        "&quot;X:\\\\&quot;</Arguments>\n"
        "    </Exec>\n"
        "  </Actions>\n"
        "</Task>\n";
    EXPECT_EQ(xml.value(), expected);
}

TEST(Autostart, TaskRejectsControlCharacters) {
    Spec spec;
    spec.daemon = "C:\\ghostdrived.exe";
    spec.data = "C:\\data\n</Arguments>";
    spec.mount = "X:";
    EXPECT_FALSE(task_xml(spec, WindowsUser{"S-1-5-21-1", "PC\\me"}));
    spec.data = "C:\\data";
    EXPECT_FALSE(task_xml(spec, WindowsUser{"S-1-5-21-1", "PC\\me\x01"}));
}

#ifdef _WIN32
// The Task Scheduler accepts the XML for this user. TASK_VALIDATE_ONLY checks it as registering
// would, but registers nothing.
TEST(Autostart, TheTaskSchedulerAcceptsTheTask) {
    auto user = current_windows_user();
    ASSERT_TRUE(user) << user.error().message;
    EXPECT_NE(user.value().sid.rfind("S-1-", 0), std::string::npos) << user.value().sid;
    EXPECT_NE(user.value().name.find('\\'), std::string::npos) << user.value().name;
    Spec spec;
    spec.daemon = "C:\\Program Files\\GhostDrive\\ghostdrived.exe";
    spec.data = "C:\\Users\\Me Too\\AppData\\Local\\GhostDrive";
    spec.mount = "X:";
    auto xml = task_xml(spec, user.value());
    ASSERT_TRUE(xml) << xml.error().message;
    const std::string name = "GhostDrive-validation-only";
    auto valid = validate_task(name, xml.value());
    ASSERT_TRUE(valid) << valid.error().message;
    auto exists = task_exists(name);
    ASSERT_TRUE(exists) << exists.error().message;
    EXPECT_FALSE(exists.value());

    // And it does check: a restart interval under the schema's minute is refused.
    auto bad = xml.value();
    auto at = bad.find("<Interval>PT1M</Interval>");
    ASSERT_NE(at, std::string::npos);
    bad.replace(at, std::strlen("<Interval>PT1M</Interval>"), "<Interval>PT1S</Interval>");
    EXPECT_FALSE(validate_task(name, bad));
}
#endif

#ifndef _WIN32
TEST(Autostart, WriteFileAtomicIsWorldReadableAndReplaces) {
    test::TempDir dir;
    auto path = dir.path() / "LaunchAgents" / "job.plist";
    ASSERT_TRUE(write_file_atomic(path, "first"));
    ASSERT_TRUE(write_file_atomic(path, "second"));
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_EQ(text.str(), "second");
    struct stat st{};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0644u);
    auto tmp = path;
    tmp += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(tmp));
}
#endif

}  // namespace
}  // namespace ghostdrive::autostart
