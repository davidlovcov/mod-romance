-- mod-romance: custom personality presets (characters database)
-- Players can save a companion personality as their own preset.

CREATE TABLE IF NOT EXISTS `romance_preset` (
  `account_id`  INT UNSIGNED NOT NULL,
  `preset_id`   INT UNSIGNED NOT NULL COMMENT 'n of "custom:<n>", per account',
  `label`       VARCHAR(64)  NOT NULL DEFAULT '',
  `description` VARCHAR(255) NOT NULL DEFAULT '',
  `profile`     MEDIUMTEXT   NOT NULL COMMENT 'JSON PersonalityProfile (without name/nickname)',
  `updated_at`  BIGINT       NOT NULL DEFAULT 0,
  PRIMARY KEY (`account_id`, `preset_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-romance custom personality presets';
