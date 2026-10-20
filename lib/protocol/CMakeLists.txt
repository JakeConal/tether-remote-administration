find_package(MbedTLS CONFIG QUIET)
find_package(mbedtls CONFIG QUIET)

# vcpkg's mbedtls port has exported several target spellings across its
# versions; pick whichever import target the resolved config provides.
if(TARGET MbedTLS::mbedtls)
    set(TETHER_MBEDTLS_TARGET MbedTLS::mbedtls)
elseif(TARGET mbedtls::mbedtls)
    set(TETHER_MBEDTLS_TARGET mbedtls::mbedtls)
else()
    set(TETHER_MBEDTLS_TARGET mbedtls)
endif()

add_library(tether-protocol STATIC envelope.cpp payloads.cpp handshake.cpp session.cpp)
target_include_directories(tether-protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tether-protocol PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(tether-protocol PRIVATE ${TETHER_MBEDTLS_TARGET})
target_link_libraries(tether-protocol PRIVATE tether-common)
