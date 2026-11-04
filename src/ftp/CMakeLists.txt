add_library(transfer_lib
    verify_engine.cpp
    object_storage_engine.cpp
    disk_executor.cpp
    async_file_receiver.cpp
    async_file_source.cpp
)
target_link_libraries(transfer_lib PUBLIC common_lib OpenSSL::Crypto)

add_library(metadata_lib metadata_service.cpp metadata_root.cpp)
target_link_libraries(metadata_lib PUBLIC common_lib)

add_library(ftp_lib
    transfer_task.cpp
    upload_engine.cpp
    download_engine.cpp
    server.cpp
    client.cpp
)
target_link_libraries(ftp_lib PUBLIC metadata_lib transfer_lib common_lib auth_lib rpc_lib OpenSSL::SSL OpenSSL::Crypto)
