add_library(auth_lib
    auth_service.cpp
    user_repository.cpp
    password_hasher.cpp
)

target_include_directories(auth_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)

target_link_libraries(auth_lib
    PUBLIC
        common_lib
    PRIVATE
        OpenSSL::Crypto
        MySQLClient::MySQLClient
)
