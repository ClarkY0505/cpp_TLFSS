CREATE TABLE storage_objects (
    object_hash CHAR(64)
        CHARACTER SET ascii
        COLLATE ascii_bin
        NOT NULL,

    state ENUM('LIVE', 'DELETING', 'UNAVAILABLE')
        NOT NULL DEFAULT 'LIVE',

    unreferenced_at DATETIME(6) DEFAULT NULL,

    PRIMARY KEY (object_hash),

    KEY idx_objects_gc (state, unreferenced_at)
) ENGINE=InnoDB;
