add_library(ftp_lib server.cpp service_engine.cpp)
target_link_libraries(ftp_lib PUBLIC common_lib auth_lib rpc_lib OpenSSL::SSL OpenSSL::Crypto)
