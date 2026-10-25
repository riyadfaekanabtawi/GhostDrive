#include "autostart.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>

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

}  // namespace
}  // namespace ghostdrive::autostart
