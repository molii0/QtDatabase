#include "DBManager.h"

#include <QDateTime>

// ============================================================================
// 分时电价(峰谷平)
// ----------------------------------------------------------------------------
// 不同时段电价不同: 峰段最贵、谷段最便宜、平段居中。充电账单按"充电发生的
// 每一分钟落在哪个时段"切段计价(功率恒定, 每度电只按所在分钟取一次价)。
//
// 时段划分(minuteOfDay = 当天第 0..1439 分钟):
//   峰(peak):  08:00-11:59、17:00-20:59
//   谷(valley):23:00-06:59(含深夜与凌晨)
//   平(flat):  其余(07、12-16、21-22 点)
// ============================================================================

DBManager::PriceBand DBManager::priceBandOfMinute(int minuteOfDay)
{
    const int h = (minuteOfDay / 60) % 24;      // 防御: 即使跨天累加也归一
    if (h >= 23 || h < 7)
        return BandValley;
    if ((h >= 8 && h < 12) || (h >= 17 && h < 21))
        return BandPeak;
    return BandFlat;
}

double DBManager::avgPriceOf(double ratePeak, double rateFlat, double rateValley,
                             const QDateTime &start, qint64 durationSecs)
{
    if (durationSecs <= 0 || !start.isValid())
        return rateFlat;

    auto rateOf = [&](int minuteOfDay) {
        switch (priceBandOfMinute(minuteOfDay)) {
        case BandPeak:   return ratePeak;
        case BandValley: return rateValley;
        case BandFlat:   break;
        }
        return rateFlat;
    };

    const int startMinute = start.time().hour() * 60 + start.time().minute();
    const qint64 fullMinutes = durationSecs / 60;
    const double tailMin = (durationSecs % 60) / 60.0;   // 不足 1 分钟的尾巴(比例)

    double sum = 0.0;
    double denom = 0.0;
    for (qint64 m = 0; m < fullMinutes; ++m) {
        sum += rateOf((startMinute + static_cast<int>(m)) % 1440);
        denom += 1.0;
    }
    if (tailMin > 0.0) {
        sum += rateOf((startMinute + static_cast<int>(fullMinutes)) % 1440) * tailMin;
        denom += tailMin;
    }
    return denom > 0.0 ? sum / denom : rateFlat;
}
