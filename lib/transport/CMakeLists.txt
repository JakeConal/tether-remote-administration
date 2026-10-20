find_package(asio CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)

add_library(tether-transport STATIC
    transport.cpp
    tcp.cpp
    gmail.cpp
    gmail_cpr.cpp)
target_include_directories(tether-transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# asio is the wire layer behind the whole transport; cpr is confined to the
# Gmail HttpCurlClient/GmailClient implementations (gmail_cpr.cpp).
target_link_libraries(tether-transport PUBLIC asio::asio)
target_link_libraries(tether-transport PRIVATE cpr::cpr)
target_link_libraries(tether-transport PRIVATE tether-common tether-protocol nlohmann_json::nlohmann_json)
