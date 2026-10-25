#include "ghostdrive/app/cli.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::app {
namespace {

const std::string kId = "AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GGGG-HHHH-IIII-JJJJ-KKKK-LLLL-MMMA";

TEST(Commands, UseCliDefaultsWhenNothingIsSet) {
    Paths p;
    EXPECT_EQ(commands::status(p), (Argv{"status", "--json"}));
    EXPECT_EQ(commands::id(p), (Argv{"id"}));
    EXPECT_EQ(commands::init(p, "desk"), (Argv{"init", "--name", "desk"}));
    EXPECT_EQ(commands::join(p, "gd1-AB", "lap top"),
              (Argv{"join", "gd1-AB", "--name", "lap top"}));
    EXPECT_EQ(commands::invite(p, 600), (Argv{"invite", "--ttl", "600"}));
    EXPECT_EQ(commands::remove(p, kId), (Argv{"remove", kId}));
    EXPECT_EQ(commands::stop(p), (Argv{"stop"}));
    EXPECT_EQ(commands::autostart_on(p), (Argv{"autostart", "on"}));
    EXPECT_EQ(commands::autostart_off(p), (Argv{"autostart", "off"}));
    EXPECT_EQ(commands::autostart_status(p), (Argv{"autostart", "status"}));
    EXPECT_EQ(commands::rendezvous_show(p), (Argv{"rendezvous"}));
    EXPECT_EQ(commands::rendezvous_set(p, "rv:7374", kId), (Argv{"rendezvous", "rv:7374", kId}));
    EXPECT_TRUE(commands::daemon(p).empty());
}

TEST(Commands, PassDataAndMount) {
    Paths p{"/d ir", "/m"};
    EXPECT_EQ(commands::status(p), (Argv{"status", "--json", "--data", "/d ir"}));
    EXPECT_EQ(commands::init(p, "-x"), (Argv{"init", "--name", "-x", "--data", "/d ir"}));
    EXPECT_EQ(commands::stop(p), (Argv{"stop", "--data", "/d ir"}));
    // Only Start and autostart take the mountpoint.
    EXPECT_EQ(commands::autostart_on(p),
              (Argv{"autostart", "on", "--data", "/d ir", "--mount", "/m"}));
    EXPECT_EQ(commands::autostart_off(p), (Argv{"autostart", "off", "--data", "/d ir"}));
    EXPECT_EQ(commands::daemon(p), (Argv{"--data", "/d ir", "--mount", "/m"}));
    EXPECT_EQ(commands::daemon(Paths{std::nullopt, "/m"}), (Argv{"--mount", "/m"}));
}

TEST(Validation, DeviceNames) {
    EXPECT_FALSE(device_name_problem("desk"));
    EXPECT_FALSE(device_name_problem("Riyad's laptop \xc3\xa9"));
    EXPECT_TRUE(device_name_problem(""));
    EXPECT_TRUE(device_name_problem("   "));
    EXPECT_TRUE(device_name_problem(std::string(65, 'x')));
    EXPECT_FALSE(device_name_problem(std::string(64, 'x')));
    EXPECT_TRUE(device_name_problem("a\nb"));
    EXPECT_TRUE(device_name_problem("evil\xe2\x80\xae"));
    EXPECT_TRUE(device_name_problem("bad\xff"));
}

TEST(Validation, Paths) {
    EXPECT_FALSE(path_problem(""));
    EXPECT_FALSE(path_problem("/home/me/drive"));
    EXPECT_FALSE(path_problem("~"));
    EXPECT_FALSE(path_problem("~/GhostDrive"));
    EXPECT_TRUE(path_problem("relative/dir"));
    EXPECT_TRUE(path_problem("~other/dir"));
    EXPECT_TRUE(path_problem("--data"));
    EXPECT_TRUE(path_problem("/a\nb"));
    EXPECT_EQ(expand_path(" ~/G ", "/home/me"), "/home/me/G");
    EXPECT_EQ(expand_path("~", "/home/me"), "/home/me");
    EXPECT_EQ(expand_path("/x", "/home/me"), "/x");
    EXPECT_FALSE(expand_path("  ", "/home/me"));
}

TEST(Validation, RendezvousAddresses) {
    EXPECT_FALSE(rendezvous_address_problem("rv.example.org:7374"));
    EXPECT_FALSE(rendezvous_address_problem("203.0.113.5:1"));
    EXPECT_FALSE(rendezvous_address_problem("[2001:db8::1]:7374"));
    EXPECT_TRUE(rendezvous_address_problem("rv.example.org"));
    EXPECT_TRUE(rendezvous_address_problem(":7374"));
    EXPECT_TRUE(rendezvous_address_problem("rv:"));
    EXPECT_TRUE(rendezvous_address_problem("rv:0"));
    EXPECT_TRUE(rendezvous_address_problem("rv:65536"));
    EXPECT_TRUE(rendezvous_address_problem("rv:12x"));
    EXPECT_TRUE(rendezvous_address_problem("r v:7374"));
    EXPECT_TRUE(rendezvous_address_problem("rv;rm:7374"));
}

TEST(Validation, Fingerprints) {
    std::string compact = "AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIIIJJJJKKKKLLLLMMMA";
    EXPECT_EQ(normalize_fingerprint(compact), kId);
    EXPECT_EQ(normalize_fingerprint(kId), kId);
    EXPECT_EQ(
        normalize_fingerprint("aaaa bbbb-cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmma"),
        kId);
    EXPECT_FALSE(normalize_fingerprint(compact.substr(1)));
    EXPECT_FALSE(normalize_fingerprint(compact + "A"));
    EXPECT_FALSE(normalize_fingerprint("1AAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIIIJJJJKKKKLLLLMMMA"));
}

TEST(Invite, ExtractsCodeFromPastedText) {
    EXPECT_EQ(extract_invite_code("gd1-ABCD-EF23"), "gd1-ABCD-EF23");
    EXPECT_EQ(extract_invite_code("  ghostdrive join gd1-abcd-ef23\n"), "gd1-ABCD-EF23");
    EXPECT_EQ(extract_invite_code("GD1-ABCD-EF23-."), "gd1-ABCD-EF23");
    EXPECT_FALSE(extract_invite_code("hello"));
    EXPECT_FALSE(extract_invite_code("gd1-"));
    EXPECT_FALSE(extract_invite_code("gd1--AB"));
    EXPECT_TRUE(is_invite_code("gd1-A2"));
    EXPECT_FALSE(is_invite_code("gd1-A1"));
    EXPECT_FALSE(is_invite_code("gd1-AB--CD"));
    EXPECT_FALSE(is_invite_code("gd2-AB"));
}

TEST(Invite, ParsesCliOutput) {
    // cli/ghostdrive.cpp, cmd_invite.
    auto invite = parse_invite(
        "On the other device, run:\n\n  ghostdrive join gd1-ABCDEFGH-IJKLMNOP-QRST2345\n"
        "The code works once, for 10 minutes. Anyone who has it can join, so send it "
        "privately.\n");
    ASSERT_TRUE(invite);
    EXPECT_EQ(invite->code, "gd1-ABCDEFGH-IJKLMNOP-QRST2345");
    EXPECT_EQ(invite->minutes, 10);

    auto one = parse_invite("ghostdrive join gd1-AB\nThe code works once, for 1 minute");
    ASSERT_TRUE(one);
    EXPECT_EQ(one->minutes, 1);
    auto bare = parse_invite("gd1-AB");
    ASSERT_TRUE(bare);
    EXPECT_FALSE(bare->minutes);
    EXPECT_FALSE(parse_invite("ghostdrive: the GhostDrive daemon is not running"));
    EXPECT_FALSE(parse_invite("for 5"));
}

TEST(Invite, Countdown) {
    EXPECT_EQ(format_countdown(600), "10:00");
    EXPECT_EQ(format_countdown(545), "9:05");
    EXPECT_EQ(format_countdown(59), "0:59");
    EXPECT_EQ(format_countdown(0), "0:00");
    EXPECT_EQ(format_countdown(-3), "0:00");
}

TEST(Autostart, ParsesStatus) {
    auto off = parse_autostart_status(
        "login item: not set up\nsystemd:    inactive\ndaemon:     /usr/bin/ghostdrived\n");
    ASSERT_TRUE(off);
    EXPECT_FALSE(off->configured);
    EXPECT_FALSE(off->active);
    auto on = parse_autostart_status(
        "login item: /home/me/.config/systemd/user/ghostdrive-x.service\nsystemd:    active\n"
        "daemon:     not found\n");
    ASSERT_TRUE(on);
    EXPECT_TRUE(on->configured);
    EXPECT_TRUE(on->active);
    auto mac = parse_autostart_status("login item: /x.plist\nlaunchd:    loaded\n");
    ASSERT_TRUE(mac);
    EXPECT_TRUE(mac->active);
    EXPECT_FALSE(parse_autostart_status("ghostdrive: HOME is not set\n"));
}

TEST(Rendezvous, ParsesSetting) {
    auto none = parse_rendezvous_setting("none\n");
    ASSERT_TRUE(none);
    EXPECT_FALSE(none->configured);
    auto set = parse_rendezvous_setting("rv.example:7374 " + kId + "\n");
    ASSERT_TRUE(set);
    EXPECT_TRUE(set->configured);
    EXPECT_EQ(set->address, "rv.example:7374");
    EXPECT_EQ(set->fingerprint, kId);
    EXPECT_FALSE(parse_rendezvous_setting("garbage"));
}

TEST(Errors, NotRunning) {
    CliResult r{true, false, 1, "",
                "ghostdrive: the GhostDrive daemon is not running (No such file or directory)\n"
                "Start it with `ghostdrived`.\n"};
    EXPECT_TRUE(says_not_running(r));
    EXPECT_EQ(plain_error(r), "GhostDrive is not running.\nStart it on the Home page.");
    CliResult other{true, false, 1, "", "ghostdrive: no device called x in this drive\n"};
    EXPECT_FALSE(says_not_running(other));
}

TEST(Errors, PlainWords) {
    CliResult join{true, false, 1, "Contacting the inviting device...\n",
                   "ghostdrive: could not join: timed out\nThe invite may have expired or been "
                   "used already; ask for a new one with `ghostdrive invite`, and check that both "
                   "devices are on the same network.\n"};
    EXPECT_EQ(plain_error(join),
              "Could not join: timed out\nThe invite may have expired or been used already; ask "
              "for a new one (Add device, on a device of the drive), and check that both devices "
              "are on the same network.");

    // Daemon replies come on stdout with "error: ".
    CliResult reply{true, false, 1, "error: this device cannot remove itself\x1b[0m\n", ""};
    EXPECT_EQ(plain_error(reply), "This device cannot remove itself?[0m");

    CliResult autostart{
        true, false, 1, "",
        "ghostdrive: no drive at /d; run `ghostdrive init` or `ghostdrive join` first\n"};
    EXPECT_EQ(plain_error(autostart), "No drive at /d; create or join a drive first");

    EXPECT_NE(plain_error(CliResult{false, false, -1, "", "No such file"}).find("Could not run"),
              std::string::npos);
    EXPECT_EQ(plain_error(CliResult{true, true, -1, "", ""}),
              "The ghostdrive tool did not finish in time.");
    EXPECT_NE(
        plain_error(CliResult{true, false, 2, "", "usage: ghostdrive <command>"}).find("older"),
        std::string::npos);
    EXPECT_EQ(plain_error(CliResult{true, false, 3, "", ""}),
              "ghostdrive failed with exit status 3.");
}

TEST(Output, RemoveKeepsTheHint) {
    CliResult r{true, false, 0,
                "Removed old (" + kId +
                    "). It can no longer connect to this drive's devices.\nghost (" + kId +
                    ") was added by it. If you do not recognise it, remove it too: ghostdrive "
                    "remove " +
                    kId + "\n",
                ""};
    auto text = plain_output(r);
    EXPECT_NE(text.find("Removed old"), std::string::npos);
    EXPECT_NE(text.find("was added by it"), std::string::npos);
    EXPECT_NE(text.find("ghostdrive remove " + kId), std::string::npos);
}

TEST(Locate, CliCandidates) {
    EXPECT_EQ(cli_candidates("/opt/gd/bin", "/usr/bin::.:relative:/usr/local/bin:/usr/bin"),
              (std::vector<std::string>{"/opt/gd/bin/ghostdrive", "/usr/bin/ghostdrive",
                                        "/usr/local/bin/ghostdrive"}));
    EXPECT_EQ(cli_candidates("", ""), std::vector<std::string>{});
}

TEST(Locate, DaemonCandidates) {
    EXPECT_EQ(daemon_candidates("/b/cli/ghostdrive", "/usr/bin"),
              (std::vector<std::string>{"/b/cli/ghostdrived", "/b/daemon/ghostdrived",
                                        "/usr/bin/ghostdrived"}));
}

TEST(Mounts, ReadsMountinfo) {
    constexpr std::string_view info =
        "22 1 0:21 / /proc rw,nosuid - proc proc rw\n"
        "98 29 0:52 / /home/me/GhostDrive rw,nosuid,nodev - fuse.ghostdrive ghostdrive rw\n"
        "99 29 0:53 / /home/me/Ghost\\040Drive rw - fuse.ghostdrive ghostdrive rw\n";
    EXPECT_TRUE(mountinfo_has(info, "/home/me/GhostDrive"));
    EXPECT_TRUE(mountinfo_has(info, "/home/me/GhostDrive/"));
    EXPECT_TRUE(mountinfo_has(info, "/home/me/Ghost Drive"));
    EXPECT_FALSE(mountinfo_has(info, "/home/me/Ghost\\040Drive"));
    EXPECT_FALSE(mountinfo_has(info, "/home/me"));
    EXPECT_FALSE(mountinfo_has("", "/proc"));
    EXPECT_FALSE(mountinfo_has("garbage\n", "/proc"));
}

TEST(Defaults, MatchTheCli) {
    EXPECT_EQ(default_data_dir("/home/me", ""), "/home/me/.local/share/ghostdrive");
    EXPECT_EQ(default_data_dir("/home/me", "/xdg"), "/xdg/ghostdrive");
    EXPECT_EQ(default_mountpoint("/home/me"), "/home/me/GhostDrive");
    EXPECT_EQ(drive_marker("/d"), "/d/drive");
    EXPECT_EQ(drive_marker("/d/"), "/d/drive");
}

TEST(Settings, RoundTrip) {
    AppSettings s{"/data dir", "~/G"};
    EXPECT_EQ(parse_settings(format_settings(s)), s);
    EXPECT_EQ(parse_settings(""), AppSettings{});
    EXPECT_EQ(parse_settings("# c\nunknown=1\nmountpoint = /m \n"), (AppSettings{"", "/m"}));
    AppSettings broken{"/a\nmountpoint=/evil", ""};
    EXPECT_EQ(parse_settings(format_settings(broken)).mountpoint, "");
}

}  // namespace
}  // namespace ghostdrive::app
