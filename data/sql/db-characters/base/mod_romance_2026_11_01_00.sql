-- mod-romance: persistent companion data (characters database)
-- Applied automatically by the AzerothCore DB updater (Updates.EnableDatabases includes characters)
-- or manually:  mysql acore_characters < mod_romance_2026_11_01_00.sql
-- All identities are character low GUIDs (characters.guid), never names.

CREATE TABLE IF NOT EXISTS `romance_companion` (
  `owner_guid`          INT UNSIGNED     NOT NULL COMMENT 'real player character (characters.guid)',
  `companion_guid`      INT UNSIGNED     NOT NULL COMMENT 'companion character (characters.guid)',
  `owner_account`       INT UNSIGNED     NOT NULL DEFAULT 0,
  `companion_name`      VARCHAR(12)      NOT NULL DEFAULT '',
  `configured`          TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `stage`               TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'denormalized copy of relationship.stage for admin queries',
  `personality`         MEDIUMTEXT       NOT NULL COMMENT 'JSON PersonalityProfile',
  `relationship`        TEXT             NOT NULL COMMENT 'JSON RelationshipState (server authoritative)',
  `settings`            TEXT             NOT NULL COMMENT 'JSON CompanionSettings (player preferences)',
  `turns_since_summary` INT UNSIGNED     NOT NULL DEFAULT 0,
  `created_at`          BIGINT           NOT NULL DEFAULT 0,
  `updated_at`          BIGINT           NOT NULL DEFAULT 0,
  PRIMARY KEY (`owner_guid`, `companion_guid`),
  KEY `idx_companion` (`companion_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-romance companion configuration and relationship';

CREATE TABLE IF NOT EXISTS `romance_player_profile` (
  `account_id`  INT UNSIGNED NOT NULL,
  `owner_guid`  INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = account-level default profile',
  `profile`     TEXT         NOT NULL COMMENT 'JSON PlayerProfile (player-supplied roleplay data)',
  `updated_at`  BIGINT       NOT NULL DEFAULT 0,
  PRIMARY KEY (`account_id`, `owner_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-romance player self-descriptions';

CREATE TABLE IF NOT EXISTS `romance_memory` (
  `owner_guid`       INT UNSIGNED     NOT NULL,
  `companion_guid`   INT UNSIGNED     NOT NULL,
  `id`               INT UNSIGNED     NOT NULL COMMENT 'per-pair id, never reused',
  `kind`             VARCHAR(16)      NOT NULL COMMENT 'fact|preference|episode|relationship|promise|gift|summary',
  `source`           VARCHAR(16)      NOT NULL COMMENT 'observed|player_stated|model_summary|interpretation|user_edited|profile',
  `importance`       TINYINT UNSIGNED NOT NULL DEFAULT 50,
  `text`             VARCHAR(400)     NOT NULL,
  `pinned`           TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `recall_count`     INT UNSIGNED     NOT NULL DEFAULT 0,
  `created_at`       BIGINT           NOT NULL DEFAULT 0,
  `updated_at`       BIGINT           NOT NULL DEFAULT 0,
  `last_recalled_at` BIGINT           NOT NULL DEFAULT 0,
  PRIMARY KEY (`owner_guid`, `companion_guid`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-romance long-term memories';

CREATE TABLE IF NOT EXISTS `romance_chat_log` (
  `id`             BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  `owner_guid`     INT UNSIGNED     NOT NULL,
  `companion_guid` INT UNSIGNED     NOT NULL,
  `role`           TINYINT UNSIGNED NOT NULL COMMENT '0 player, 1 companion, 2 world event',
  `text`           TEXT             NOT NULL,
  `created_at`     BIGINT           NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_pair` (`owner_guid`, `companion_guid`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-romance short-term conversation history (pruned)';

CREATE TABLE IF NOT EXISTS `romance_gift_log` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `owner_guid`     INT UNSIGNED    NOT NULL,
  `companion_guid` INT UNSIGNED    NOT NULL,
  `item_guid`      INT UNSIGNED    NULL DEFAULT NULL COMMENT 'item instance guid; NULL for gold',
  `item_entry`     INT UNSIGNED    NOT NULL DEFAULT 0,
  `item_count`     INT UNSIGNED    NOT NULL DEFAULT 1,
  `item_name`      VARCHAR(120)    NOT NULL DEFAULT '',
  `category`       VARCHAR(16)     NOT NULL DEFAULT '',
  `score`          FLOAT           NOT NULL DEFAULT 0,
  `created_at`     BIGINT          NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uniq_item_per_companion` (`companion_guid`, `item_guid`),
  KEY `idx_pair_time` (`owner_guid`, `companion_guid`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-romance rewarded gifts (replay/farming protection)';

CREATE TABLE IF NOT EXISTS `romance_relationship_log` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `owner_guid`     INT UNSIGNED    NOT NULL,
  `companion_guid` INT UNSIGNED    NOT NULL,
  `type`           VARCHAR(32)     NOT NULL,
  `detail`         VARCHAR(255)    NOT NULL DEFAULT '',
  `created_at`     BIGINT          NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_pair` (`owner_guid`, `companion_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-romance audit log of relationship changes';
