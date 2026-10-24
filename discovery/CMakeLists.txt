add_library(ghostdrive_discovery src/lan_id.cpp src/rendezvous_codec.cpp)
add_library(ghostdrive::discovery ALIAS ghostdrive_discovery)
target_include_directories(ghostdrive_discovery PUBLIC include)
target_link_libraries(ghostdrive_discovery
    PUBLIC ghostdrive::core ghostdrive::transport asio::asio
    PRIVATE OpenSSL::Crypto ghostdrive_warnings)

# LAN discovery goes through the system's DNS-SD API: built into macOS, and provided on Linux
# by Avahi's compatibility library (libavahi-compat-libdnssd-dev). Without it, devices are
# reached only by stored addresses.
if(APPLE)
    set(GHOSTDRIVE_DNSSD_FOUND TRUE)
else()
    find_path(DNSSD_INCLUDE_DIR dns_sd.h PATH_SUFFIXES avahi-compat-libdns_sd)
    find_library(DNSSD_LIBRARY dns_sd)
    if(DNSSD_INCLUDE_DIR AND DNSSD_LIBRARY)
        set(GHOSTDRIVE_DNSSD_FOUND TRUE)
    endif()
endif()
if(GHOSTDRIVE_DNSSD_FOUND)
    target_sources(ghostdrive_discovery PRIVATE src/lan_discovery.cpp)
    target_compile_definitions(ghostdrive_discovery PUBLIC GHOSTDRIVE_HAVE_DNSSD=1)
    if(NOT APPLE)
        target_include_directories(ghostdrive_discovery PRIVATE ${DNSSD_INCLUDE_DIR})
        target_link_libraries(ghostdrive_discovery PRIVATE ${DNSSD_LIBRARY})
    endif()
    message(STATUS "LAN discovery: DNS-SD")
else()
    message(STATUS "LAN discovery: no DNS-SD library found, skipping")
endif()

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
