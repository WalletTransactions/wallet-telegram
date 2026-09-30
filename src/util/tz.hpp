// Часовой пояс пользователя (tg_link.tz, IANA): даты трат и граница «сегодня»
// для отчётов — operation.date хранится без TZ, в локальном времени клиента
// (org/docs/TODO_telegram_bot.md, §0).
//
// В C++17 нет своей базы поясов (std::chrono::tzdb — C++20), поэтому —
// системная tzdata (/usr/share/zoneinfo, пакет tzdata в runtime-образе) через
// переменную TZ + localtime_r. Подмена TZ процесса не потокобезопасна: звать
// только из потока поллинга (он единственный, кто работает с датами).
#pragma once

#include <ctime>
#include <string>

namespace tz {

// Имя вида "Europe/Moscow": только [A-Za-z0-9_+-/], без "..", без ведущего
// '/', и файл zoneinfo_dir/<name> существует. zoneinfo_dir — ради тестов.
bool is_valid_name(const std::string& name, const std::string& zoneinfo_dir = "/usr/share/zoneinfo");

// Локальная дата "YYYY-MM-DD" в поясе tz для момента now + days_offset
// суток. Неизвестный пояс — UTC (glibc так и трактует незнакомое TZ).
std::string local_date(const std::string& tz, std::time_t now, int days_offset = 0);

// Локальное время "HH:MM" в поясе tz для момента now — показать в ответе на
// /tz, чтобы пользователь сразу сверил пояс со своими часами.
std::string local_time(const std::string& tz, std::time_t now);

// То же с секундами, "HH:MM:SS" — время траты «сегодня» для парсера.
std::string local_time_seconds(const std::string& tz, std::time_t now);

} // namespace tz
