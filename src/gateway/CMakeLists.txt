add_library(gateway_lib proxyGateway.cpp)
target_link_libraries(gateway_lib PUBLIC common_lib rpc_lib proto_lib)
