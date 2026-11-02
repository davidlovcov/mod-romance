-- mod-true-companion: the module was renamed from mod-romance. Its tables are renamed from romance_* to
-- true_companion_*; nothing is lost. (The older files in this folder keep their names so the DB updater does
-- not run them again; on a new database they create the romance_* tables and this file renames them.)
--
-- Each rename runs only when the old table exists and the new one doesn't, so applying this file twice, or on
-- a database that already has the new tables, changes nothing.

SET @tc_old = 'romance_companion', @tc_new = 'true_companion_companion';
SET @tc_sql = (SELECT IF(SUM(table_name = @tc_old) = 1 AND SUM(table_name = @tc_new) = 0,
                         CONCAT('RENAME TABLE `', @tc_old, '` TO `', @tc_new, '`'), 'DO 0')
               FROM information_schema.tables WHERE table_schema = DATABASE());
PREPARE tc_stmt FROM @tc_sql; EXECUTE tc_stmt; DEALLOCATE PREPARE tc_stmt;

SET @tc_old = 'romance_player_profile', @tc_new = 'true_companion_player_profile';
SET @tc_sql = (SELECT IF(SUM(table_name = @tc_old) = 1 AND SUM(table_name = @tc_new) = 0,
                         CONCAT('RENAME TABLE `', @tc_old, '` TO `', @tc_new, '`'), 'DO 0')
               FROM information_schema.tables WHERE table_schema = DATABASE());
PREPARE tc_stmt FROM @tc_sql; EXECUTE tc_stmt; DEALLOCATE PREPARE tc_stmt;

SET @tc_old = 'romance_memory', @tc_new = 'true_companion_memory';
SET @tc_sql = (SELECT IF(SUM(table_name = @tc_old) = 1 AND SUM(table_name = @tc_new) = 0,
                         CONCAT('RENAME TABLE `', @tc_old, '` TO `', @tc_new, '`'), 'DO 0')
               FROM information_schema.tables WHERE table_schema = DATABASE());
PREPARE tc_stmt FROM @tc_sql; EXECUTE tc_stmt; DEALLOCATE PREPARE tc_stmt;

SET @tc_old = 'romance_chat_log', @tc_new = 'true_companion_chat_log';
SET @tc_sql = (SELECT IF(SUM(table_name = @tc_old) = 1 AND SUM(table_name = @tc_new) = 0,
                         CONCAT('RENAME TABLE `', @tc_old, '` TO `', @tc_new, '`'), 'DO 0')
               FROM information_schema.tables WHERE table_schema = DATABASE());
PREPARE tc_stmt FROM @tc_sql; EXECUTE tc_stmt; DEALLOCATE PREPARE tc_stmt;

SET @tc_old = 'romance_gift_log', @tc_new = 'true_companion_gift_log';
SET @tc_sql = (SELECT IF(SUM(table_name = @tc_old) = 1 AND SUM(table_name = @tc_new) = 0,
                         CONCAT('RENAME TABLE `', @tc_old, '` TO `', @tc_new, '`'), 'DO 0')
               FROM information_schema.tables WHERE table_schema = DATABASE());
PREPARE tc_stmt FROM @tc_sql; EXECUTE tc_stmt; DEALLOCATE PREPARE tc_stmt;

SET @tc_old = 'romance_relationship_log', @tc_new = 'true_companion_relationship_log';
SET @tc_sql = (SELECT IF(SUM(table_name = @tc_old) = 1 AND SUM(table_name = @tc_new) = 0,
                         CONCAT('RENAME TABLE `', @tc_old, '` TO `', @tc_new, '`'), 'DO 0')
               FROM information_schema.tables WHERE table_schema = DATABASE());
PREPARE tc_stmt FROM @tc_sql; EXECUTE tc_stmt; DEALLOCATE PREPARE tc_stmt;

SET @tc_old = 'romance_preset', @tc_new = 'true_companion_preset';
SET @tc_sql = (SELECT IF(SUM(table_name = @tc_old) = 1 AND SUM(table_name = @tc_new) = 0,
                         CONCAT('RENAME TABLE `', @tc_old, '` TO `', @tc_new, '`'), 'DO 0')
               FROM information_schema.tables WHERE table_schema = DATABASE());
PREPARE tc_stmt FROM @tc_sql; EXECUTE tc_stmt; DEALLOCATE PREPARE tc_stmt;
