# Finds the DNS-SD (Bonjour) API.
#   macOS:   part of libSystem, headers in the SDK.
#   Linux:   avahi's compatibility layer (libavahi-compat-libdnssd).
#   Windows: Apple's Bonjour SDK (BONJOUR_SDK_HOME, e.g. C:/Program Files/Bonjour SDK).
# Defines DNSSD::DNSSD.

if(APPLE)
    find_path(DNSSD_INCLUDE_DIR dns_sd.h)
    set(DNSSD_LIBRARY "")
    set(_dnssd_required DNSSD_INCLUDE_DIR)
else()
    find_path(DNSSD_INCLUDE_DIR dns_sd.h
        HINTS $ENV{BONJOUR_SDK_HOME}/Include
        PATH_SUFFIXES avahi-compat-libdns_sd)
    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_dnssd_arch x64)
        else()
            set(_dnssd_arch Win32)
        endif()
        find_library(DNSSD_LIBRARY dnssd HINTS $ENV{BONJOUR_SDK_HOME}/Lib/${_dnssd_arch})
    else()
        find_library(DNSSD_LIBRARY dns_sd)
    endif()
    set(_dnssd_required DNSSD_INCLUDE_DIR DNSSD_LIBRARY)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DNSSD REQUIRED_VARS ${_dnssd_required})

if(DNSSD_FOUND AND NOT TARGET DNSSD::DNSSD)
    add_library(DNSSD::DNSSD INTERFACE IMPORTED)
    target_include_directories(DNSSD::DNSSD INTERFACE ${DNSSD_INCLUDE_DIR})
    if(DNSSD_LIBRARY)
        target_link_libraries(DNSSD::DNSSD INTERFACE ${DNSSD_LIBRARY})
    endif()
endif()
mark_as_advanced(DNSSD_INCLUDE_DIR DNSSD_LIBRARY)
