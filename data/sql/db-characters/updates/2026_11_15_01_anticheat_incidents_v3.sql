-- Incident lifecycle snapshots are immutable and idempotent; v2 history remains untouched.
-- Daily aggregates count incident openings and independent Warden records only.
CREATE TABLE IF NOT EXISTS `anticheat_events_v3` (
    `event_key` VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `incident_key` VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT '',
    `event_kind` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=opened,1=lifecycle,2=alert,3=decision,4=Warden',
    `guid` INT UNSIGNED NOT NULL,
    `account` INT UNSIGNED NOT NULL,
    `name` VARCHAR(48) NOT NULL DEFAULT '',
    `cheat_type` TINYINT UNSIGNED NOT NULL,
    `map_id` INT UNSIGNED NOT NULL,
    `position_x` FLOAT NOT NULL,
    `position_y` FLOAT NOT NULL,
    `position_z` FLOAT NOT NULL,
    `confidence` FLOAT NOT NULL,
    `score` FLOAT NOT NULL COMMENT 'Current decayed session score after the observation',
    `actionable` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `action` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=evidence,1=kick decision,2=temporary ban decision',
    `details` VARCHAR(1024) NOT NULL DEFAULT '',
    `created_at` BIGINT UNSIGNED NOT NULL COMMENT 'UTC Unix seconds at enqueue',
    PRIMARY KEY (`event_key`),
    KEY `idx_guid_created` (`guid`,`created_at`),
    KEY `idx_account_created` (`account`,`created_at`),
    KEY `idx_created` (`created_at`),
    KEY `idx_incident_created` (`incident_key`,`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- The day is UTC and independent of the database session timezone.
-- This is an event-sampled mean, not a time-weighted mean of player activity.
CREATE OR REPLACE VIEW `anticheat_daily_stats_v3` AS
SELECT `guid`, `account`, FLOOR(`created_at` / 86400) AS `utc_day`,
    COUNT(*) AS `total_reports`,
    SUM(`cheat_type`=0) AS `speed_reports`,
    SUM(`cheat_type`=1) AS `fly_reports`,
    SUM(`cheat_type`=2) AS `teleport_reports`,
    SUM(`cheat_type`=3) AS `climb_reports`,
    SUM(`cheat_type`=4) AS `jump_reports`,
    SUM(`cheat_type`=5) AS `gravity_reports`,
    SUM(`cheat_type`=6) AS `waterwalk_reports`,
    SUM(`cheat_type`=7) AS `nofall_reports`,
    SUM(`cheat_type`=8) AS `ignorecontrol_reports`,
    SUM(`cheat_type`=9) AS `fakeping_reports`,
    SUM(`cheat_type`=10) AS `noclip_reports`,
    SUM(`cheat_type`=11) AS `warden_reports`,
    AVG(`score`) AS `average_score`, MAX(`created_at`) AS `last_updated`
FROM `anticheat_events_v3` WHERE `action`=0 AND `event_kind` IN (0,4)
GROUP BY `guid`, `account`, FLOOR(`created_at` / 86400);
