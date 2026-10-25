# Homebrew formula for GhostDrive, for a tap of your own (it is not in homebrew-core).
#
# Dependencies come from vcpkg, pinned by the external/vcpkg submodule and vcpkg.json, not from
# Homebrew formulae. The CMake files look the libraries up through vcpkg's own CMake packages
# (unofficial-sqlite3, asio's config package, ngtcp2's static targets built with its OpenSSL
# backend), which Homebrew's sqlite, asio and libngtcp2 do not provide, and building against the
# same pinned versions as CI keeps a Homebrew build identical to a source build. The cost is a
# slower first install: vcpkg downloads and builds OpenSSL, ngtcp2 and the rest during the build
# (network access is allowed in the build phase by default).
#
# The source is the git tag rather than a tarball because GitHub's tag tarballs leave out the
# vcpkg submodule.
#
# Mounting the drive on macOS needs FUSE-T, which is a cask; formulae cannot depend on casks, so
# install it first:
#   brew install --cask fuse-t
# Without it the build still succeeds, but ghostdrived only runs with --no-mount.
class Ghostdrive < Formula
  desc "Virtual drive stored on your own devices, peer to peer and end-to-end encrypted"
  homepage "https://github.com/riyadfaekanabtawi/GhostDrive"
  url "https://github.com/riyadfaekanabtawi/GhostDrive.git", tag: "v0.1.1"
  license "Apache-2.0"
  head "https://github.com/riyadfaekanabtawi/GhostDrive.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "pkgconf" => :build

  on_linux do
    depends_on "avahi"
    depends_on "libfuse"
  end

  def install
    ENV["VCPKG_DISABLE_METRICS"] = "1"
    ENV["VCPKG_BINARY_SOURCES"] = "clear"
    ENV["VCPKG_DOWNLOADS"] = (buildpath/"vcpkg-downloads").to_s
    system "./external/vcpkg/bootstrap-vcpkg.sh", "-disableMetrics"

    # FUSE-T installs fuse3.pc outside Homebrew's prefix, where superenv's pkg-config does not
    # look.
    fuse_t_pkgconfig = Pathname("/usr/local/lib/pkgconfig")
    if OS.mac? && (fuse_t_pkgconfig/"fuse3.pc").exist?
      ENV.append_path "PKG_CONFIG_PATH", fuse_t_pkgconfig
    else
      opoo "FUSE-T not found; ghostdrived will be built without mount support" if OS.mac?
    end

    args = %W[
      -DCMAKE_TOOLCHAIN_FILE=#{buildpath}/external/vcpkg/scripts/buildsystems/vcpkg.cmake
      -DGHOSTDRIVE_BUILD_TESTS=OFF
    ]
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja", *args, *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      Mounting the drive on macOS needs FUSE-T. Install it before this formula:
        brew install --cask fuse-t
      If you installed GhostDrive first, reinstall it afterwards:
        brew reinstall ghostdrive

      To start the daemon now and at every login:
        ghostdrive autostart on
    EOS
  end

  test do
    assert_equal "ghostdrive #{version}\n", shell_output("#{bin}/ghostdrive --version")
    assert_equal "ghostdrived #{version}\n", shell_output("#{bin}/ghostdrived --version")
  end
end
