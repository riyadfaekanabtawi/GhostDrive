add_library(ghostdrive_discovery src/lan_id.cpp src/rendezvous_codec.cpp)
add_library(ghostdrive::discovery ALIAS ghostdrive_discovery)
target_include_directories(ghostdrive_discovery PUBLIC include)
target_link_libraries(ghostdrive_discovery
    PUBLIC ghostdrive::core ghostdrive::transport asio::asio
    PRIVATE OpenSSL::Crypto ghostdrive_warnings)

# LAN discovery goes through the system's mDNS responder: DNS-SD on macOS; on Linux,
# avahi-daemon through avahi-client, or else an Apple-compatible libdns_sd (mDNSResponder).
# Avahi's DNS-SD compatibility library lacks shared connections and DNSServiceGetAddrInfo, so it
# is rejected here. Without any of these, devices are reached only by stored addresses.
if(APPLE)
    set(GHOSTDRIVE_DNSSD_FOUND TRUE)
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(AVAHI_CLIENT QUIET IMPORTED_TARGET avahi-client)
    endif()
    if(NOT AVAHI_CLIENT_FOUND)
        find_path(DNSSD_INCLUDE_DIR dns_sd.h PATH_SUFFIXES avahi-compat-libdns_sd)
        find_library(DNSSD_LIBRARY dns_sd)
    endif()
    if(DNSSD_INCLUDE_DIR AND DNSSD_LIBRARY)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_INCLUDES ${DNSSD_INCLUDE_DIR})
        set(CMAKE_REQUIRED_LIBRARIES ${DNSSD_LIBRARY})
        check_cxx_source_compiles([[
            #include <dns_sd.h>
            int main() {
                DNSServiceRef ref = nullptr;
                return DNSServiceGetAddrInfo(&ref, kDNSServiceFlagsShareConnection, 0,
                                             kDNSServiceProtocol_IPv4, "", nullptr, nullptr);
            }]] GHOSTDRIVE_DNSSD_USABLE)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(GHOSTDRIVE_DNSSD_USABLE)
            set(GHOSTDRIVE_DNSSD_FOUND TRUE)
        endif()
    endif()
endif()
# GHOSTDRIVE_HAVE_LAN_DISCOVERY tells users of LanDiscovery that start() can succeed. The
# backend macros are public too, since the class layout depends on them.
if(AVAHI_CLIENT_FOUND)
    target_sources(ghostdrive_discovery PRIVATE src/lan_discovery_avahi.cpp)
    target_compile_definitions(ghostdrive_discovery
        PUBLIC GHOSTDRIVE_HAVE_LAN_DISCOVERY=1 GHOSTDRIVE_LAN_AVAHI=1)
    target_link_libraries(ghostdrive_discovery PRIVATE PkgConfig::AVAHI_CLIENT)
    message(STATUS "LAN discovery: avahi-client ${AVAHI_CLIENT_VERSION}")
elseif(GHOSTDRIVE_DNSSD_FOUND)
    target_sources(ghostdrive_discovery PRIVATE src/lan_discovery.cpp)
    target_compile_definitions(ghostdrive_discovery
        PUBLIC GHOSTDRIVE_HAVE_LAN_DISCOVERY=1 GHOSTDRIVE_HAVE_DNSSD=1)
    if(NOT APPLE)
        target_include_directories(ghostdrive_discovery PRIVATE ${DNSSD_INCLUDE_DIR})
        target_link_libraries(ghostdrive_discovery PRIVATE ${DNSSD_LIBRARY})
    endif()
    message(STATUS "LAN discovery: DNS-SD")
else()
    target_sources(ghostdrive_discovery PRIVATE src/lan_discovery_none.cpp)
    message(STATUS "LAN discovery: no avahi-client or usable DNS-SD library found, skipping")
endif()

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
