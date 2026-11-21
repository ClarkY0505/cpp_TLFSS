add_library(transfer_lib
    storage_root_lock.cpp
    object_coordinator.cpp
    verify_engine.cpp
    object_storage_engine.cpp
    disk_executor.cpp
    async_file_receiver.cpp
    async_file_source.cpp
)
target_link_libraries(transfer_lib PUBLIC common_lib OpenSSL::Crypto)

add_library(metadata_lib 
    metadata_service.cpp 
    metadata_root.cpp 
    upload_registration.cpp
    directory_creation.cpp
    directory_deletion.cpp
    file_deletion.cpp
    object_repository.cpp
    object_gc_transaction.cpp
    subtree_coordinator.cpp
    subtree_versions.cpp
    file_rename.cpp
    file_move.cpp
)
target_link_libraries(metadata_lib PUBLIC common_lib)

add_library(storage_business_lib
    server.cpp
    upload_engine.cpp
    download_engine.cpp
)
target_link_libraries(storage_business_lib PUBLIC auth_lib metadata_lib transfer_lib)

add_library(ftp_lib
    object_gc_worker.cpp
    object_garbage_collector.cpp
    object_backfill.cpp
    transfer_task.cpp
    tcp_storage_server.cpp
    tcp_transfer_tasks.cpp
    storage_runtime.cpp
    storage_recovery_engine.cpp
    client.cpp
)
target_link_libraries(ftp_lib PUBLIC storage_business_lib metadata_lib transfer_lib common_lib auth_lib rpc_lib OpenSSL::SSL OpenSSL::Crypto)
