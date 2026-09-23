add_library(rpc_lib
    grpc_server.cpp
    grpc_client.cpp
    interceptors/auth_interceptors.cpp
)
target_link_libraries(rpc_lib PUBLIC common_lib proto_lib)
