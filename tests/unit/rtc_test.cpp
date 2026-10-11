// Host-side tests for the calendar arithmetic behind the real-time clock:
// the kernel turns the date the CMOS chip reports into seconds since 1970.

#include <cstdio>
#include <ctime>

#define SIRIUS_HOST_TEST
#include <drivers/rtc.cpp>

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

int main() {
    printf("epoch:\n");
    expect(rtc::to_epoch({1970, 1, 1, 0, 0, 0}) == 0, "the epoch is zero");
    expect(rtc::to_epoch({1970, 1, 2, 0, 0, 1}) == 86401, "a day and a second later");
    expect(rtc::to_epoch({2000, 3, 1, 0, 0, 0}) == 951868800, "past the leap day of 2000, a leap year by the 400 rule");
    expect(rtc::to_epoch({2026, 10, 10, 12, 34, 56}) == 1791635696, "a date in this project's present");
    expect(rtc::to_epoch({2100, 3, 1, 0, 0, 0}) == 4107542400ULL, "2100 is not a leap year");
    expect(rtc::to_epoch({1969, 12, 31, 23, 59, 59}) == 0 && rtc::to_epoch({2026, 13, 1, 0, 0, 0}) == 0 &&
           rtc::to_epoch({2026, 0, 1, 0, 0, 0}) == 0, "dates that cannot be are rejected");

    // Against the host's own calendar, every day for sixty years.
    bool agree = true;
    for (time_t t = 946684800; t < 946684800 + 60LL * 366 * 86400 && agree; t += 86400 + 3661) {
        tm parts{};
        gmtime_r(&t, &parts);
        const rtc::DateTime date{ static_cast<u16>(parts.tm_year + 1900), static_cast<u8>(parts.tm_mon + 1),
                                  static_cast<u8>(parts.tm_mday), static_cast<u8>(parts.tm_hour),
                                  static_cast<u8>(parts.tm_min), static_cast<u8>(parts.tm_sec) };
        agree = rtc::to_epoch(date) == static_cast<u64>(t);
    }
    expect(agree, "it agrees with gmtime() from 2000 to 2060");

    printf("registers:\n");
    expect(rtc::decode(0x59, true) == 59 && rtc::decode(0x09, true) == 9 && rtc::decode(0x26, true) == 26, "BCD registers decode");
    expect(rtc::decode(59, false) == 59, "binary registers are taken as they are");

    if (g_failures) {
        printf("rtc_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("rtc_test: all passed\n");
    return 0;
}
