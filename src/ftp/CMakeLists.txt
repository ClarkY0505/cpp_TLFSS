add_library(ftp_lib server.cpp)
target_link_libraries(ftp_lib PUBLIC common_lib rpc_lib OpenSSL::SSL OpenSSL::Crypto)
