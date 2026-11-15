/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "ReportQueue.h"
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace anticheat
{
std::string SqlText(std::string const& value, std::size_t maxBytes)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string sql = "CONVERT(X'";
    std::size_t written = 0;
    for (std::size_t i = 0; i < value.size() && written < maxBytes;)
    {
        unsigned char first = static_cast<unsigned char>(value[i]);
        std::size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = length && i + length <= value.size();
        for (std::size_t j = 1; valid && j < length; ++j)
        {
            auto next = static_cast<unsigned char>(value[i + j]);
            valid = next >= 0x80 && next <= 0xbf;
        }
        if (valid && length >= 3)
        {
            auto second = static_cast<unsigned char>(value[i + 1]);
            valid = !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second >= 0xa0) &&
                !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second >= 0x90);
        }
        if (!valid)
        {
            sql += "3f"; // Replace malformed UTF-8; never poison the whole batch.
            ++i;
            ++written;
            continue;
        }
        if (written + length > maxBytes)
            break;
        for (std::size_t j = 0; j < length; ++j)
        {
            auto byte = static_cast<unsigned char>(value[i + j]);
            sql += hex[byte >> 4];
            sql += hex[byte & 0x0f];
        }
        i += length;
        written += length;
    }
    sql += "' USING utf8mb4)";
    return sql;
}

std::string BuildReportInsert(ReportRecord const& record)
{
    if (Index(record.type) >= DetectionCount || !std::isfinite(record.x) ||
        !std::isfinite(record.y) || !std::isfinite(record.z) || !std::isfinite(record.score) ||
        !std::isfinite(record.confidence) || record.eventKey.empty())
        throw std::invalid_argument("Invalid anticheat report");
    std::ostringstream sql;
    sql.imbue(std::locale::classic());
    sql << std::setprecision(std::numeric_limits<float>::max_digits10);
    sql << "INSERT INTO `anticheat_events_v2` (`event_key`,`guid`,`account`,`name`,`cheat_type`,"
        "`map_id`,`position_x`,`position_y`,`position_z`,`confidence`,`score`,`actionable`,"
        "`action`,`details`,`created_at`) VALUES ("
        << SqlText(record.eventKey, 64) << ',' << record.guid << ',' << record.account << ','
        << SqlText(record.name, 48) << ',' << Index(record.type) << ',' << record.mapId << ','
        << record.x << ',' << record.y << ',' << record.z << ',' << record.confidence << ','
        << record.score << ',' << (record.actionable ? 1 : 0) << ','
        << static_cast<unsigned>(record.action) << ',' << SqlText(record.details, 1024) << ','
        << record.createdAt << ") ON DUPLICATE KEY UPDATE `event_key`=VALUES(`event_key`)";
    return sql.str();
}
}
