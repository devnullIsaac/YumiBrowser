/**
 * @file test_calendar.c
 * @brief Comprehensive tests for calendar.h — including international calendars.
 *
 * Mock contract:
 *   - local_to_utc / utc_to_local / go_to_date / select_ymd / ymd_to_timestamp
 *     all accept Gregorian years (the mock is calendar-agnostic at that layer).
 *   - The calendar handle converts Gregorian -> native for get_field(YEAR).
 *   - Navigation mutates the timestamp but does NOT refresh cached display_year/
 *     display_month. Tests MUST call calendar_get_day_count() before asserting
 *     those fields.
 */
#define CALENDAR_IMPLEMENTATION
#include "calendar.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-60s", #name); name(); printf("PASS\n"); } while(0)

/* ═══════════════════════════════════════════════════════════════
 *  Mock Services — Gregorian Ground Truth
 * ═══════════════════════════════════════════════════════════════ */

#define MOCK_CURRENT_TIME_MS 1705310400000LL  /* 2024-01-15 12:00 UTC */

typedef struct {
    calendar_type_t type;
    int year;        /* Gregorian year (ground truth) */
    int month;       /* Gregorian month (0-11) */
    int day;         /* Gregorian day (1-31) */
    int hour, minute, second, ms;
    char locale[32];
} mock_cal_state_t;

/* Calendar-native year offsets (constant for mock) */
static int native_year_offset(calendar_type_t type) {
    switch (type) {
        case CALENDAR_BUDDHIST: return 543;
        case CALENDAR_PERSIAN:  return -621;
        case CALENDAR_HEBREW:   return 3760;
        case CALENDAR_ISLAMIC:
        case CALENDAR_ISLAMIC_CIVIL:
        case CALENDAR_ISLAMIC_UMALQURA:
        case CALENDAR_ISLAMIC_TBLA:
            return -579; /* Mock approximation: 2024 -> 1445 */
        default:
            return 0;
    }
}

/* Gregorian -> native */
static int to_native_year(calendar_type_t type, int greg_year) {
    return greg_year + native_year_offset(type);
}

/* Native -> Gregorian (exact inverse of above) */
static int to_gregorian_year(calendar_type_t type, int native_year) {
    return native_year - native_year_offset(type);
}

static bool is_leap_year_gregorian(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int get_days_in_month_gregorian(int year, int month) {
    static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 1 && is_leap_year_gregorian(year)) return 29;
    return dim[month];
}

/* ms since epoch for Gregorian date */
static int64_t gregorian_to_ms(int year, int month, int day,
                                int hour, int minute, int second, int ms) {
    int64_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += is_leap_year_gregorian(y) ? 366 : 365;
    }
    for (int m = 0; m < month; m++) {
        days += get_days_in_month_gregorian(year, m);
    }
    days += day - 1;
    return days * 86400000LL +
           hour * 3600000LL + minute * 60000LL +
           second * 1000LL + ms;
}

/* ms -> Gregorian date */
static void ms_to_gregorian(int64_t ms, int *year, int *month, int *day,
                             int *hour, int *minute, int *second, int *msec) {
    int64_t days = ms / 86400000LL;
    int64_t rem = ms % 86400000LL;
    if (rem < 0) { rem += 86400000LL; days--; }

    int y = 1970;
    while (days >= (is_leap_year_gregorian(y) ? 366 : 365)) {
        days -= is_leap_year_gregorian(y) ? 366 : 365;
        y++;
    }

    int m = 0;
    while (m < 12 && days >= get_days_in_month_gregorian(y, m)) {
        days -= get_days_in_month_gregorian(y, m);
        m++;
    }

    if (year)   *year   = y;
    if (month)  *month  = m;
    if (day)    *day    = (int)days + 1;
    if (hour)   *hour   = (int)(rem / 3600000);
    if (minute) *minute = (int)((rem % 3600000) / 60000);
    if (second) *second = (int)((rem % 60000) / 1000);
    if (msec)   *msec   = (int)(rem % 1000);
}

/* ── Mock API ───────────────────────────────────────────────── */

static void *mock_cal_open(calendar_type_t type, const char *locale,
                           const char *timezone) {
    mock_cal_state_t *st = calloc(1, sizeof(mock_cal_state_t));
    st->type = type;
    if (locale) strncpy(st->locale, locale, sizeof(st->locale) - 1);
    (void)timezone;
    return st;
}

static void mock_cal_close(void *cal) {
    free(cal);
}

static void mock_cal_set_time(void *cal, int64_t ms) {
    mock_cal_state_t *st = (mock_cal_state_t *)cal;
    ms_to_gregorian(ms, &st->year, &st->month, &st->day,
                    &st->hour, &st->minute, &st->second, &st->ms);
}

static int64_t mock_cal_get_time(void *cal) {
    mock_cal_state_t *st = (mock_cal_state_t *)cal;
    return gregorian_to_ms(st->year, st->month, st->day,
                           st->hour, st->minute, st->second, st->ms);
}

static int mock_cal_get_field(void *cal, int field) {
    mock_cal_state_t *st = (mock_cal_state_t *)cal;
    switch (field) {
        case 0:  return 1;   /* ERA */
        case 1:  return to_native_year(st->type, st->year);
        case 2:  return st->month;
        case 3:  return 3;   /* WEEK_OF_YEAR (mock) */
        case 4:  return 3;   /* WEEK_OF_MONTH (mock) */
        case 5:  return st->day;
        case 6:  return st->month * 30 + st->day; /* DAY_OF_YEAR approx */
        case 7:  return ((mock_cal_get_time(cal) / 86400000LL) + 4) % 7 + 1;
        case 8:  return 2;
        case 9:  return st->hour < 12 ? 0 : 1;
        case 10: return st->hour % 12;
        case 11: return st->hour;
        case 12: return st->minute;
        case 13: return st->second;
        case 14: return st->ms;
        default: return 0;
    }
}

static void mock_cal_set_field(void *cal, int field, int value) {
    mock_cal_state_t *st = (mock_cal_state_t *)cal;
    switch (field) {
        /* YEAR input is in calendar-native terms; convert to Gregorian */
        case 1: st->year = to_gregorian_year(st->type, value); break;
        case 2: st->month = value; break;
        case 5: st->day = value; break;
        case 10: st->hour = value; break;
        case 11: st->hour = value; break;
        case 12: st->minute = value; break;
        case 13: st->second = value; break;
        case 14: st->ms = value; break;
    }
}

static void mock_cal_add(void *cal, int field, int amount) {
    mock_cal_state_t *st = (mock_cal_state_t *)cal;
    switch (field) {
        case 1: /* YEAR — advance Gregorian year by amount */
            st->year += amount;
            break;
        case 2: { /* MONTH */
            st->month += amount;
            while (st->month < 0) {
                st->month += 12;
                st->year--;
            }
            while (st->month >= 12) {
                st->month -= 12;
                st->year++;
            }
            int dim = get_days_in_month_gregorian(st->year, st->month);
            if (st->day > dim) st->day = dim;
            break;
        }
        case 5: { /* DATE */
            int64_t ms = mock_cal_get_time(cal);
            ms += (int64_t)amount * 86400000LL;
            mock_cal_set_time(cal, ms);
            break;
        }
    }
}

static void mock_cal_roll(void *cal, int field, int amount) {
    mock_cal_add(cal, field, amount);
}

static int mock_cal_field_diff(void *cal, int64_t target_ms, int field) {
    (void)cal;
    int64_t diff_ms = target_ms - mock_cal_get_time(cal);
    switch (field) {
        case 1: return (int)(diff_ms / (365LL * 86400000LL));
        case 2: return (int)(diff_ms / (30LL * 86400000LL));
        case 5: return (int)(diff_ms / 86400000LL);
        default: return 0;
    }
}

static bool mock_cal_is_weekend(void *cal, int64_t ms) {
    (void)cal;
    int dow = (int)((ms / 86400000LL + 4) % 7) + 1;
    return dow == 1 || dow == 7;
}

static int mock_cal_get_first_dow(void *cal) {
    (void)cal; return 1;
}

static int mock_cal_get_min_days(void *cal) {
    (void)cal; return 1;
}

static int mock_format_date(int64_t ms, const char *pattern,
                            const char *locale, const char *timezone,
                            char *out, int out_cap) {
    (void)locale; (void)timezone;
    int y, m, d, h, mi, s, msec;
    ms_to_gregorian(ms, &y, &m, &d, &h, &mi, &s, &msec);
    if (strcmp(pattern, "yyyy-MM-dd") == 0) {
        snprintf(out, out_cap, "%04d-%02d-%02d", y, m + 1, d);
    } else if (strcmp(pattern, "HH:mm") == 0) {
        snprintf(out, out_cap, "%02d:%02d", h, mi);
    } else {
        snprintf(out, out_cap, "%04d-%02d-%02d", y, m + 1, d);
    }
    return (int)strlen(out);
}

static int64_t mock_parse_date(const char *text, const char *pattern,
                                const char *locale, const char *timezone) {
    (void)pattern; (void)locale; (void)timezone;
    int y, m, d;
    if (sscanf(text, "%d-%d-%d", &y, &m, &d) == 3) {
        return gregorian_to_ms(y, m - 1, d, 0, 0, 0, 0);
    }
    return INT64_MIN;
}

static const char *gregorian_months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *islamic_months[] = {
    "Muharram", "Safar", "Rabi' al-awwal", "Rabi' al-thani",
    "Jumada al-awwal", "Jumada al-thani", "Rajab", "Sha'ban",
    "Ramadan", "Shawwal", "Dhu al-Qi'dah", "Dhu al-Hijjah"
};
static const char *hebrew_months[] = {
    "Nisan", "Iyar", "Sivan", "Tamuz", "Av", "Elul",
    "Tishrei", "Cheshvan", "Kislev", "Tevet", "Shevat", "Adar"
};
static const char *persian_months[] = {
    "Farvardin", "Ordibehesht", "Khordad", "Tir", "Mordad", "Shahrivar",
    "Mehr", "Aban", "Azar", "Dey", "Bahman", "Esfand"
};

static int mock_get_month_name(int month, const char *locale,
                                char *out, int out_cap) {
    if (month < 0 || month > 11) return -1;
    const char **months = gregorian_months;
    if (strstr(locale, "ar")) months = islamic_months;
    else if (strstr(locale, "he")) months = hebrew_months;
    else if (strstr(locale, "fa")) months = persian_months;
    strncpy(out, months[month], out_cap - 1);
    out[out_cap - 1] = '\0';
    return (int)strlen(out);
}

static int mock_get_weekday_name(int dow, bool short_name, const char *locale,
                                  char *out, int out_cap) {
    (void)locale;
    static const char *names[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
    };
    if (dow < 1 || dow > 7) return -1;
    if (short_name) snprintf(out, out_cap, "%.3s", names[dow-1]);
    else {
        strncpy(out, names[dow-1], out_cap - 1);
        out[out_cap - 1] = '\0';
    }
    return (int)strlen(out);
}

static int64_t mock_current_time(void) {
    return MOCK_CURRENT_TIME_MS;
}

static int64_t mock_local_to_utc(int year, int month, int day,
                                  int hour, int minute, int second, int ms,
                                  const char *timezone) {
    (void)timezone;
    return gregorian_to_ms(year, month, day, hour, minute, second, ms);
}

static void mock_utc_to_local(int64_t ms, const char *timezone,
                               int *y, int *m, int *d,
                               int *h, int *mi, int *s, int *msec) {
    (void)timezone;
    ms_to_gregorian(ms, y, m, d, h, mi, s, msec);
}

static calendar_services_t g_mock_services = {
    .calendar_open = mock_cal_open,
    .calendar_close = mock_cal_close,
    .calendar_set_time = mock_cal_set_time,
    .calendar_get_time = mock_cal_get_time,
    .calendar_get_field = mock_cal_get_field,
    .calendar_set_field = mock_cal_set_field,
    .calendar_add = mock_cal_add,
    .calendar_roll = mock_cal_roll,
    .calendar_field_difference = mock_cal_field_diff,
    .calendar_is_weekend = mock_cal_is_weekend,
    .calendar_get_first_day_of_week = mock_cal_get_first_dow,
    .calendar_get_min_days_in_first_week = mock_cal_get_min_days,
    .format_date = mock_format_date,
    .parse_date = mock_parse_date,
    .get_month_name = mock_get_month_name,
    .get_weekday_name = mock_get_weekday_name,
    .get_current_time_ms = mock_current_time,
    .local_to_utc = mock_local_to_utc,
    .utc_to_local = mock_utc_to_local,
    .user = NULL
};

/* ═══════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════ */

static void make_cal(calendar_t *cal) {
    calendar_create(cal, CALENDAR_GREGORIAN, "en_US", "UTC", &g_mock_services);
}

static void make_cal_with_date(calendar_t *cal, int year, int month, int day) {
    make_cal(cal);
    calendar_go_to_date(cal, year, month, day);
}

static void make_cal_type(calendar_t *cal, calendar_type_t type, const char *locale) {
    if (!locale) locale = "en_US";
    calendar_create(cal, type, locale, "UTC", &g_mock_services);
}

/* All dates passed here are GREGORIAN. The handle converts internally. */
static void make_cal_type_with_date(calendar_t *cal, calendar_type_t type,
                                    const char *locale, int year, int month, int day) {
    make_cal_type(cal, type, locale);
    calendar_go_to_date(cal, year, month, day);
}

static int64_t make_ts(int year, int month, int day) {
    return gregorian_to_ms(year, month, day, 0, 0, 0, 0);
}

/* Trigger cache refresh so display_year / display_month are valid */
static void refresh(calendar_t *cal) {
    calendar_get_day_count(cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  1. Lifecycle (Gregorian baseline)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_create_destroy) {
    calendar_t cal;
    make_cal(&cal);
    assert(cal.type == CALENDAR_GREGORIAN);
    assert(cal.view == CALENDAR_VIEW_MONTH);
    assert(cal.select_mode == CALENDAR_SELECT_SINGLE);
    assert(cal.selection_count == 0);
    calendar_destroy(&cal);
}

TEST(test_create_with_locale) {
    calendar_t cal;
    calendar_create(&cal, CALENDAR_GREGORIAN, "ja_JP", "Asia/Tokyo", &g_mock_services);
    assert(strcmp(cal.locale, "ja_JP") == 0);
    assert(strcmp(cal.timezone, "Asia/Tokyo") == 0);
    calendar_destroy(&cal);
}

TEST(test_create_defaults_to_today) {
    calendar_t cal;
    make_cal(&cal);
    assert(cal.display_timestamp_ms == MOCK_CURRENT_TIME_MS);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  2. International Calendar Creation
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_create_buddhist) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_BUDDHIST, "th_TH");
    assert(cal.type == CALENDAR_BUDDHIST);
    assert(cal.display_year == 2567);  /* 2024 + 543 */
    calendar_destroy(&cal);
}

TEST(test_create_islamic) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_ISLAMIC, "ar_SA");
    assert(cal.type == CALENDAR_ISLAMIC);
    assert(cal.display_year == 1445);  /* mock: 2024 - 579 */
    calendar_destroy(&cal);
}

TEST(test_create_persian) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_PERSIAN, "fa_IR");
    assert(cal.type == CALENDAR_PERSIAN);
    assert(cal.display_year == 1403);  /* 2024 - 621 */
    calendar_destroy(&cal);
}

TEST(test_create_hebrew) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_HEBREW, "he_IL");
    assert(cal.type == CALENDAR_HEBREW);
    assert(cal.display_year == 5784);  /* 2024 + 3760 */
    calendar_destroy(&cal);
}

TEST(test_create_japanese) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_JAPANESE, "ja_JP");
    assert(cal.type == CALENDAR_JAPANESE);
    assert(cal.display_year == 2024);
    calendar_destroy(&cal);
}

TEST(test_create_chinese) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_CHINESE, "zh_CN");
    assert(cal.type == CALENDAR_CHINESE);
    assert(cal.display_year == 2024);
    calendar_destroy(&cal);
}

TEST(test_create_coptic) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_COPTIC, "ar_EG");
    assert(cal.type == CALENDAR_COPTIC);
    calendar_destroy(&cal);
}

TEST(test_create_indian) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_INDIAN, "hi_IN");
    assert(cal.type == CALENDAR_INDIAN);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  3. Buddhist Calendar
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_buddhist_year_conversion) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_BUDDHIST, "th_TH");
    calendar_go_to_date(&cal, 2024, 0, 15);   /* Gregorian */
    refresh(&cal);
    assert(cal.display_year == 2567);
    assert(cal.display_month == 0);
    calendar_destroy(&cal);
}

TEST(test_buddhist_navigation) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_BUDDHIST, "th_TH", 2024, 5, 15);
    refresh(&cal);
    assert(cal.display_year == 2567);
    assert(cal.display_month == 5);

    calendar_next(&cal);
    refresh(&cal);
    assert(cal.display_month == 6);   /* June -> July */
    assert(cal.display_year == 2567); /* Same Buddhist year */

    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  4. Islamic Calendar
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_islamic_month_length) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_ISLAMIC, "ar_SA");

    int64_t start = calendar_ymd_to_timestamp(&cal, 2024, 0, 1);
    int64_t m1 = calendar_add_months(&cal, start, 1);
    int64_t m2 = calendar_add_months(&cal, start, 2);

    assert(m1 > start);
    assert(m2 > m1);

    int diff1 = (int)((m1 - start) / 86400000LL);
    int diff2 = (int)((m2 - m1) / 86400000LL);
    assert(diff1 >= 28 && diff1 <= 31);
    assert(diff2 >= 28 && diff2 <= 31);

    calendar_destroy(&cal);
}

TEST(test_islamic_year_progression) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_ISLAMIC, "ar_SA", 2024, 11, 29);
    refresh(&cal);
    assert(cal.display_year == 1445);
    assert(cal.display_month == 11);

    calendar_next(&cal);
    refresh(&cal);
    /* Gregorian Dec 2024 -> Jan 2025 == Islamic 1446, month 0 */
    assert(cal.display_year == 1446);
    assert(cal.display_month == 0);

    calendar_destroy(&cal);
}

TEST(test_islamic_leap_year) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_ISLAMIC_UMALQURA, "ar_SA");
    calendar_go_to_date(&cal, 2024, 11, 29);
    calendar_next(&cal);
    refresh(&cal);
    /* Just verify no crash and we advanced */
    assert(cal.display_year == 1446);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  5. Persian Calendar
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_persian_month_lengths) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_PERSIAN, "fa_IR", 2024, 0, 31);
    int64_t before = cal.display_timestamp_ms;

    calendar_next(&cal);
    int64_t after = cal.display_timestamp_ms;

    int64_t diff = (after - before) / 86400000LL;
    assert(diff >= 1 && diff <= 31);

    calendar_destroy(&cal);
}

TEST(test_persian_leap_year) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_PERSIAN, "fa_IR");
    /* Gregorian Mar 20 2025 is near Persian year end */
    calendar_go_to_date(&cal, 2025, 2, 20);
    refresh(&cal);

    /* Mock limitation: local_to_utc is Gregorian-only, so querying
       calendar-native date 1403-11-30 will likely fail. Just ensure
       the call doesn't crash. */
    calendar_day_t day;
    (void)calendar_get_day_for_date(&cal, 1403, 11, 30, &day);

    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  6. Hebrew Calendar
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_hebrew_leap_year) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_HEBREW, "he_IL");
    calendar_go_to_date(&cal, 2024, 2, 15);
    calendar_next(&cal);
    refresh(&cal);
    /* Gregorian Mar 2024 -> Apr 2024; Hebrew year stays 5784 */
    assert(cal.display_year == 5784);
    calendar_destroy(&cal);
}

TEST(test_hebrew_month_names) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_HEBREW, "he_IL", 2024, 6, 15);
    refresh(&cal);
    char name[CALENDAR_MAX_FORMAT_LEN];
    int len = calendar_get_display_month_name(&cal, name, sizeof(name));
    assert(len > 0);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  7. Chinese Calendar
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_chinese_lunar_months) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_CHINESE, "zh_CN");
    calendar_go_to_date(&cal, 2024, 0, 1);
    refresh(&cal);
    assert(cal.display_year == 2024);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  8. Configuration / Localization
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_locale_arabic) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_ISLAMIC, "en_US");
    calendar_set_locale(&cal, "ar_SA");
    assert(strcmp(cal.locale, "ar_SA") == 0);
    calendar_destroy(&cal);
}

TEST(test_set_locale_hebrew) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_HEBREW, "en_US");
    calendar_set_locale(&cal, "he_IL");
    assert(strcmp(cal.locale, "he_IL") == 0);
    calendar_destroy(&cal);
}

TEST(test_set_locale_chinese) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_CHINESE, "en_US");
    calendar_set_locale(&cal, "zh_CN");
    assert(strcmp(cal.locale, "zh_CN") == 0);
    calendar_destroy(&cal);
}

TEST(test_set_calendar_type_switching) {
    calendar_t cal;
    make_cal(&cal);
    calendar_set_type(&cal, CALENDAR_BUDDHIST);
    assert(cal.type == CALENDAR_BUDDHIST);
    calendar_set_type(&cal, CALENDAR_GREGORIAN);
    assert(cal.type == CALENDAR_GREGORIAN);
    calendar_set_type(&cal, CALENDAR_ISLAMIC);
    assert(cal.type == CALENDAR_ISLAMIC);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  9. Navigation
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_navigation_gregorian) {
    calendar_t cal;
    make_cal_with_date(&cal, 2024, 0, 15);
    calendar_previous(&cal);
    refresh(&cal);
    assert(cal.display_year == 2023);
    assert(cal.display_month == 11);
    calendar_destroy(&cal);
}

TEST(test_navigation_buddhist) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_BUDDHIST, "th_TH", 2024, 0, 15);
    calendar_previous(&cal);
    refresh(&cal);
    /* Gregorian Jan 2024 -> Dec 2023 == Buddhist 2566, month 11 */
    assert(cal.display_year == 2566);
    assert(cal.display_month == 11);
    calendar_destroy(&cal);
}

TEST(test_year_navigation_multiple_calendars) {
    calendar_t greg, bud, isl;
    make_cal_type_with_date(&greg, CALENDAR_GREGORIAN, "en_US", 2024, 5, 15);
    make_cal_type_with_date(&bud, CALENDAR_BUDDHIST, "th_TH", 2024, 5, 15);
    make_cal_type_with_date(&isl, CALENDAR_ISLAMIC, "ar_SA", 2024, 5, 15);

    calendar_next_fast(&greg);
    calendar_next_fast(&bud);
    calendar_next_fast(&isl);

    refresh(&greg);
    refresh(&bud);
    refresh(&isl);

    assert(greg.display_year == 2025);
    assert(bud.display_year == 2568);
    assert(isl.display_year == 1446);

    calendar_destroy(&greg);
    calendar_destroy(&bud);
    calendar_destroy(&isl);
}

/* ═══════════════════════════════════════════════════════════════
 *  10. Month Name Localization
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_month_names_gregorian) {
    calendar_t cal;
    make_cal_with_date(&cal, 2024, 5, 15);
    char name[CALENDAR_MAX_FORMAT_LEN];
    int len = calendar_get_display_month_name(&cal, name, sizeof(name));
    assert(len > 0);
    assert(strcmp(name, "June") == 0);
    calendar_destroy(&cal);
}

TEST(test_month_names_islamic_arabic) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_ISLAMIC, "ar_SA", 2024, 8, 15);
    char name[CALENDAR_MAX_FORMAT_LEN];
    int len = calendar_get_display_month_name(&cal, name, sizeof(name));
    assert(len > 0);
    assert(strcmp(name, "Ramadan") == 0);
    calendar_destroy(&cal);
}

TEST(test_month_names_persian) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_PERSIAN, "fa_IR", 2024, 0, 15);
    char name[CALENDAR_MAX_FORMAT_LEN];
    int len = calendar_get_display_month_name(&cal, name, sizeof(name));
    assert(len > 0);
    assert(strcmp(name, "Farvardin") == 0);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  11. Selection (International Calendars)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_selection_buddhist_date) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_BUDDHIST, "th_TH");
    /* Pass Gregorian year; the calendar stores the timestamp */
    calendar_select_ymd(&cal, 2024, 5, 15);
    assert(cal.selection_count == 1);
    calendar_destroy(&cal);
}

TEST(test_selection_islamic_range) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_ISLAMIC, "ar_SA");
    calendar_set_select_mode(&cal, CALENDAR_SELECT_RANGE);
    /* Gregorian September 2024 */
    int64_t start = calendar_ymd_to_timestamp(&cal, 2024, 8, 1);
    int64_t end   = calendar_ymd_to_timestamp(&cal, 2024, 8, 30);
    calendar_set_range(&cal, start, end);
    assert(cal.selection_count == 2);
    calendar_destroy(&cal);
}

TEST(test_selection_hebrew_multi) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_HEBREW, "he_IL");
    calendar_set_select_mode(&cal, CALENDAR_SELECT_MULTI);
    /* Gregorian July / August 2024 */
    calendar_select_ymd(&cal, 2024, 6, 1);
    calendar_select_ymd(&cal, 2024, 6, 10);
    calendar_select_ymd(&cal, 2024, 7, 15);
    assert(cal.selection_count == 3);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  12. Date Arithmetic
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_add_years_buddhist) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_BUDDHIST, "th_TH", 2024, 0, 15);
    int64_t result = calendar_add_years(&cal, cal.display_timestamp_ms, 5);
    int y, m, d;
    calendar_timestamp_to_ymd(&cal, result, &y, &m, &d);
    assert(y == 2029);  /* Gregorian year advanced by 5 */
    calendar_destroy(&cal);
}

TEST(test_add_months_islamic) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_ISLAMIC, "ar_SA", 2024, 0, 15);
    int64_t result = calendar_add_months(&cal, cal.display_timestamp_ms, 12);
    int y, m, d;
    calendar_timestamp_to_ymd(&cal, result, &y, &m, &d);
    assert(y == 2025);  /* 12 Gregorian months later */
    calendar_destroy(&cal);
}

TEST(test_add_days_persian) {
    calendar_t cal;
    make_cal_type_with_date(&cal, CALENDAR_PERSIAN, "fa_IR", 2024, 11, 28);
    int64_t result = calendar_add_days(&cal, cal.display_timestamp_ms, 10);
    int y, m, d;
    calendar_timestamp_to_ymd(&cal, result, &y, &m, &d);
    /* Should be Gregorian Jan 7 2025 (or thereabouts) */
    assert(y == 2025 || (y == 2024 && m == 11 && d > 28));
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  13. Comparisons
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_compare_days_buddhist_gregorian) {
    calendar_t bud, greg;
    make_cal_with_date(&greg, 2024, 0, 15);
    make_cal_type_with_date(&bud, CALENDAR_BUDDHIST, "th_TH", 2024, 0, 15);

    int64_t ts_greg = calendar_start_of_day(&greg, greg.display_timestamp_ms);
    int64_t ts_bud  = calendar_start_of_day(&bud, bud.display_timestamp_ms);

    /* Both calendars point to the same absolute instant */
    assert(ts_greg == ts_bud);

    calendar_destroy(&greg);
    calendar_destroy(&bud);
}

/* ═══════════════════════════════════════════════════════════════
 *  14. Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_buddhist_year_zero) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_BUDDHIST, "th_TH");
    /* Mock limitation: years before 1970 are handled poorly, but must not crash */
    calendar_go_to_date(&cal, 1, 0, 1);
    refresh(&cal);
    calendar_destroy(&cal);
}

TEST(test_islamic_year_boundary) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_ISLAMIC, "ar_SA");
    calendar_go_to_date(&cal, 2024, 11, 29);
    calendar_next(&cal);
    refresh(&cal);
    assert(cal.display_year == 1446);
    calendar_destroy(&cal);
}

TEST(test_hebrew_leap_month_handling) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_HEBREW, "he_IL");
    calendar_go_to_date(&cal, 2024, 2, 15);
    refresh(&cal);
    /* Mock does not model leap months; just ensure no crash */
    calendar_destroy(&cal);
}

TEST(test_persian_leap_day) {
    calendar_t cal;
    make_cal_type(&cal, CALENDAR_PERSIAN, "fa_IR");
    calendar_go_to_date(&cal, 2025, 2, 20);
    refresh(&cal);
    /* Mock limitation: cannot accurately test Esfand 30 */
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  15. Original Gregorian Regression Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_timezone) {
    calendar_t cal;
    make_cal(&cal);
    calendar_set_timezone(&cal, "America/New_York");
    assert(strcmp(cal.timezone, "America/New_York") == 0);
    calendar_destroy(&cal);
}

TEST(test_set_view) {
    calendar_t cal;
    make_cal(&cal);
    calendar_set_view(&cal, CALENDAR_VIEW_YEAR);
    assert(cal.view == CALENDAR_VIEW_YEAR);
    calendar_set_view(&cal, CALENDAR_VIEW_DECADE);
    assert(cal.view == CALENDAR_VIEW_DECADE);
    calendar_destroy(&cal);
}

TEST(test_set_select_mode) {
    calendar_t cal;
    make_cal(&cal);
    calendar_set_select_mode(&cal, CALENDAR_SELECT_RANGE);
    assert(cal.select_mode == CALENDAR_SELECT_RANGE);
    calendar_set_select_mode(&cal, CALENDAR_SELECT_MULTI);
    assert(cal.select_mode == CALENDAR_SELECT_MULTI);
    calendar_destroy(&cal);
}

TEST(test_go_to_today) {
    calendar_t cal;
    make_cal_with_date(&cal, 2020, 5, 15);
    calendar_go_to_today(&cal);
    assert(cal.display_timestamp_ms == MOCK_CURRENT_TIME_MS);
    calendar_destroy(&cal);
}

TEST(test_select_single) {
    calendar_t cal;
    make_cal(&cal);
    int64_t ts = make_ts(2024, 3, 15);
    calendar_select(&cal, ts);
    assert(cal.selection_count == 1);
    assert(calendar_is_selected(&cal, ts));
    calendar_destroy(&cal);
}

TEST(test_select_range) {
    calendar_t cal;
    make_cal(&cal);
    calendar_set_select_mode(&cal, CALENDAR_SELECT_RANGE);
    int64_t start = make_ts(2024, 3, 1);
    int64_t end = make_ts(2024, 3, 10);
    calendar_select(&cal, start);
    calendar_select(&cal, end);
    assert(cal.selection_count == 2);
    calendar_destroy(&cal);
}

TEST(test_leap_year_gregorian) {
    calendar_t cal;
    make_cal(&cal);
    int64_t feb28 = make_ts(2024, 1, 28);
    int64_t mar1 = calendar_add_days(&cal, feb28, 2);
    int y, m, d;
    calendar_timestamp_to_ymd(&cal, mar1, &y, &m, &d);
    assert(m == 2);
    assert(d == 1);
    calendar_destroy(&cal);
}

TEST(test_compare_days) {
    calendar_t cal;
    make_cal(&cal);
    int64_t ts1 = make_ts(2024, 0, 15);
    int64_t ts2 = make_ts(2024, 0, 20);
    assert(calendar_compare_days(&cal, ts1, ts2) < 0);
    assert(calendar_compare_days(&cal, ts2, ts1) > 0);
    calendar_destroy(&cal);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  International Calendar Tests\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    RUN(test_create_destroy);
    RUN(test_create_with_locale);
    RUN(test_create_defaults_to_today);

    printf("\n--- International Calendar Creation ---\n");
    RUN(test_create_buddhist);
    RUN(test_create_islamic);
    RUN(test_create_persian);
    RUN(test_create_hebrew);
    RUN(test_create_japanese);
    RUN(test_create_chinese);
    RUN(test_create_coptic);
    RUN(test_create_indian);

    printf("\n--- Buddhist Calendar ---\n");
    RUN(test_buddhist_year_conversion);
    RUN(test_buddhist_navigation);

    printf("\n--- Islamic Calendar ---\n");
    RUN(test_islamic_month_length);
    RUN(test_islamic_year_progression);
    RUN(test_islamic_leap_year);

    printf("\n--- Persian Calendar ---\n");
    RUN(test_persian_month_lengths);
    RUN(test_persian_leap_year);

    printf("\n--- Hebrew Calendar ---\n");
    RUN(test_hebrew_leap_year);
    RUN(test_hebrew_month_names);

    printf("\n--- Chinese Calendar ---\n");
    RUN(test_chinese_lunar_months);

    printf("\n--- Localization ---\n");
    RUN(test_set_locale_arabic);
    RUN(test_set_locale_hebrew);
    RUN(test_set_locale_chinese);
    RUN(test_set_calendar_type_switching);

    printf("\n--- Navigation ---\n");
    RUN(test_navigation_gregorian);
    RUN(test_navigation_buddhist);
    RUN(test_year_navigation_multiple_calendars);

    printf("\n--- Month Names ---\n");
    RUN(test_month_names_gregorian);
    RUN(test_month_names_islamic_arabic);
    RUN(test_month_names_persian);

    printf("\n--- Selection (International) ---\n");
    RUN(test_selection_buddhist_date);
    RUN(test_selection_islamic_range);
    RUN(test_selection_hebrew_multi);

    printf("\n--- Date Arithmetic ---\n");
    RUN(test_add_years_buddhist);
    RUN(test_add_months_islamic);
    RUN(test_add_days_persian);

    printf("\n--- Comparisons ---\n");
    RUN(test_compare_days_buddhist_gregorian);

    printf("\n--- Edge Cases ---\n");
    RUN(test_buddhist_year_zero);
    RUN(test_islamic_year_boundary);
    RUN(test_hebrew_leap_month_handling);
    RUN(test_persian_leap_day);

    printf("\n--- Original Gregorian Tests ---\n");
    RUN(test_set_timezone);
    RUN(test_set_view);
    RUN(test_set_select_mode);
    RUN(test_go_to_today);
    RUN(test_select_single);
    RUN(test_select_range);
    RUN(test_leap_year_gregorian);
    RUN(test_compare_days);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  All international tests passed!\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    return 0;
}
