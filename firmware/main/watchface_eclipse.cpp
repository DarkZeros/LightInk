#include "core.h"
#include "settings.h"
#include "watchface_eclipse.h"

#include <Fonts/FreeMonoBold9pt7b.h>
#include "fonts/DSEG7_Classic_Bold_53.h"
#include "fonts/Seven_Segment10pt7b.h"
#include "fonts/DSEG7_Classic_Regular_15.h"
#include "fonts/DSEG7_Classic_Bold_25.h"
#include "fonts/DSEG7_Classic_Regular_39.h"
#include "icons.h"

#include <sunset.h>

#include <math.h>
#include <algorithm>

/*
 * Calculate the percentage of the Sun's disk that is covered
 * by the Moon.
 *
 * rs = Sun radius
 * rm = Moon radius
 * d  = distance between the two centers
 *
 * Result: 0.0 .. 100.0
 */
static float calculateEclipseCoverage(
    float rs,
    float rm,
    float d
) {
    /*
     * No overlap.
     */
    if (d >= rs + rm)
        return 0.0f;

    /*
     * Moon completely covers Sun.
     *
     * This is the condition we want during totality.
     */
    if (d <= fabsf(rm - rs) && rm >= rs)
        return 100.0f;

    /*
     * Sun completely inside Moon.
     */
    if (d <= fabsf(rm - rs) && rs > rm) {
        return (rm * rm) / (rs * rs) * 100.0f;
    }

    /*
     * Partial overlap.
     *
     * Standard circle-circle intersection area.
     */
    const float rs2 = rs * rs;
    const float rm2 = rm * rm;

    float cosSun =
        (d * d + rs2 - rm2) /
        (2.0f * d * rs);

    float cosMoon =
        (d * d + rm2 - rs2) /
        (2.0f * d * rm);

    /*
     * Protect acos() against tiny floating-point errors.
     */
    cosSun = std::max(-1.0f, std::min(1.0f, cosSun));
    cosMoon = std::max(-1.0f, std::min(1.0f, cosMoon));

    const float partSun =
        rs2 * acosf(cosSun);

    const float partMoon =
        rm2 * acosf(cosMoon);

    const float triangle =
        0.5f *
        sqrtf(
            (-d + rs + rm) *
            ( d + rs - rm) *
            ( d - rs + rm) *
            ( d + rs + rm)
        );

    const float overlap =
        partSun + partMoon - triangle;

    const float sunArea =
        PI * rs2;

    float percentage =
        (overlap / sunArea) * 100.0f;

    return std::max(
        0.0f,
        std::min(100.0f, percentage)
    );
}

std::vector<Rect> EclipseWatchface::render() {
    std::vector<Rect> rects;

    auto& mNow = mCore.mNow;
    bool color = mSettings.mConst.mDisplay.mInvert;

    /*
     * ------------------------------------------------------------
     * ECLIPSE DATA
     * ------------------------------------------------------------
     */

    constexpr int PARTIAL_BEGIN =
        19 * 3600 + 34 * 60 + 31;

    constexpr int TOTAL_BEGIN =
        20 * 3600 + 29 * 60 + 35;

    constexpr int MAXIMUM =
        20 * 3600 + 30 * 60 + 25;

    constexpr int TOTAL_END =
        20 * 3600 + 31 * 60 + 14;

    constexpr int PARTIAL_END =
        21 * 3600 + 16 * 60;


    /*
     * ------------------------------------------------------------
     * CURRENT TIME
     * ------------------------------------------------------------
     */

    const int nowSeconds =
        mNow.Hour * 3600 +
        mNow.Minute * 60 +
        mNow.Second;


    /*
     * ------------------------------------------------------------
     * CLEAR DISPLAY FIRST
     * ------------------------------------------------------------
     */

    mDisplay.writeFillRect(
        0,
        0,
        200,
        200,
        0
    );


    /*
     * ------------------------------------------------------------
     * CENTER ECLIPSE GRAPHIC
     *
     * IMPORTANT:
     *
     * Sun = WHITE
     * Moon = BLACK
     *
     * Both circles get borders.
     * Text is drawn AFTER this section.
     * ------------------------------------------------------------
     */

    constexpr int cx = 100;
    constexpr int cy = 106;

    /*
     * Moon is deliberately slightly larger than Sun.
     *
     * This gives us a proper totality condition:
     *
     * Moon radius >= Sun radius + center separation
     */
    constexpr float sunRadius  = 43.0f;
    constexpr float moonRadius = 45.0f;

    /*
     * Distance between Sun and Moon centers.
     *
     * At:
     *
     *   partial begin -> sunRadius + moonRadius
     *   total begin   -> moonRadius - sunRadius
     *   maximum       -> 0
     *   total end     -> moonRadius - sunRadius
     *   partial end   -> sunRadius + moonRadius
     */
    float centerDistance =
        sunRadius + moonRadius;


    /*
     * ------------------------------------------------------------
     * BEFORE PARTIAL ECLIPSE
     * ------------------------------------------------------------
     */

    if (nowSeconds < PARTIAL_BEGIN) {

        centerDistance =
            sunRadius + moonRadius;
    }


    /*
     * ------------------------------------------------------------
     * FIRST PARTIAL PHASE
     * ------------------------------------------------------------
     */

    else if (
        nowSeconds >= PARTIAL_BEGIN &&
        nowSeconds < TOTAL_BEGIN
    ) {

        const float progress =
            static_cast<float>(
                nowSeconds - PARTIAL_BEGIN
            ) /
            static_cast<float>(
                TOTAL_BEGIN - PARTIAL_BEGIN
            );

        /*
         * Moon approaches from the left.
         *
         * Start:
         *   no overlap
         *
         * End:
         *   complete coverage
         */
        centerDistance =
            (sunRadius + moonRadius) -
            progress *
            (
                (sunRadius + moonRadius) -
                (moonRadius - sunRadius)
            );
    }


    /*
     * ------------------------------------------------------------
     * TOTALITY
     * ------------------------------------------------------------
     */

    else if (
        nowSeconds >= TOTAL_BEGIN &&
        nowSeconds <= TOTAL_END
    ) {

        /*
         * Keep Moon completely covering Sun.
         *
         * At maximum it is centered.
         */
        if (nowSeconds <= MAXIMUM) {

            const float progress =
                static_cast<float>(
                    nowSeconds - TOTAL_BEGIN
                ) /
                static_cast<float>(
                    MAXIMUM - TOTAL_BEGIN
                );

            centerDistance =
                (moonRadius - sunRadius) *
                (1.0f - progress);

        } else {

            const float progress =
                static_cast<float>(
                    nowSeconds - MAXIMUM
                ) /
                static_cast<float>(
                    TOTAL_END - MAXIMUM
                );

            centerDistance =
                (moonRadius - sunRadius) *
                progress;
        }
    }


    /*
     * ------------------------------------------------------------
     * SECOND PARTIAL PHASE
     * ------------------------------------------------------------
     */

    else if (
        nowSeconds > TOTAL_END &&
        nowSeconds <= PARTIAL_END
    ) {

        const float progress =
            static_cast<float>(
                nowSeconds - TOTAL_END
            ) /
            static_cast<float>(
                PARTIAL_END - TOTAL_END
            );

        centerDistance =
            (moonRadius - sunRadius) +
            progress *
            (
                (sunRadius + moonRadius) -
                (moonRadius - sunRadius)
            );
    }


    /*
     * ------------------------------------------------------------
     * AFTER ECLIPSE
     * ------------------------------------------------------------
     */

    else {

        centerDistance =
            sunRadius + moonRadius;
    }


    /*
     * ------------------------------------------------------------
     * MOON DIRECTION
     * ------------------------------------------------------------
     *
     * Left -> center -> right
     */

    float moonX = cx;

    if (nowSeconds < MAXIMUM) {

        moonX =
            cx + centerDistance;

    } else {

        moonX =
            cx - centerDistance;
    }


    /*
     * ------------------------------------------------------------
     * CALCULATE COVERAGE
     * ------------------------------------------------------------
     */

    float coverPercent =
        calculateEclipseCoverage(
            sunRadius,
            moonRadius,
            centerDistance
        );

    /*
     * Make the totality condition explicit.
     *
     * This guarantees that floating point rounding can never
     * result in something like 99.9% during totality.
     */
    if (
        nowSeconds >= TOTAL_BEGIN &&
        nowSeconds <= TOTAL_END
    ) {
        coverPercent = 100.0f;
    }

    /*
     * Light remaining is simply the uncovered portion
     * of the solar disk.
     */
    float lightPercent =
        100.0f - coverPercent;


    /*
     * ------------------------------------------------------------
     * DRAW SUN
     * ------------------------------------------------------------
     *
     * Always WHITE.
     */

    mDisplay.fillCircle(
        cx,
        cy,
        static_cast<int>(sunRadius),
        color
    );
    /*
     * Sun border.
     */
    mDisplay.drawCircle(
        cx,
        cy,
        static_cast<int>(sunRadius+1),
        !color
    );


    /*
     * ------------------------------------------------------------
     * DRAW MOON
     * ------------------------------------------------------------
     *
     * Always BLACK.
     */

    mDisplay.fillCircle(
        static_cast<int>(moonX),
        cy,
        static_cast<int>(moonRadius),
        !color
    );


    /*
     * ------------------------------------------------------------
     * DRAW BORDERS
     * ------------------------------------------------------------
     *
     * White outlines are drawn after both fills.
     *
     * The Moon border is deliberately drawn LAST so it remains
     * visible against the white Sun.
     */

    /*
     * Sun border.
     */
    mDisplay.drawCircle(
        cx,
        cy,
        static_cast<int>(sunRadius),
        color
    );

    /*
     * Moon border.
     */
    mDisplay.drawCircle(
        static_cast<int>(moonX),
        cy,
        static_cast<int>(moonRadius),
        color
    );

    mDisplay.fillRect(0, 0, 40, 200, 0);
    mDisplay.fillRect(160, 0, 40, 200, 0);
    mDisplay.fillRect(0, 0, 200, 40, 0);
    mDisplay.fillRect(0, 160, 200, 40, 0);

    /*
     * ------------------------------------------------------------
     * DETERMINE STATUS
     * ------------------------------------------------------------
     */

    const char* status;

    if (nowSeconds < PARTIAL_BEGIN) {

        status = "UPCOMING";

    } else if (nowSeconds < TOTAL_BEGIN) {

        status = "PARTIAL";
        const_cast<Core&>(mCore).mNextUpdate = 30;

    } else if (nowSeconds <= TOTAL_END) {

        status = "TOTALITY";
        const_cast<Core&>(mCore).mNextUpdate = 2;

    } else if (nowSeconds <= PARTIAL_END) {

        status = "PARTIAL";
        const_cast<Core&>(mCore).mNextUpdate = 30;

    } else {

        status = "ENDED";
    }


    /*
     * ------------------------------------------------------------
     * ALL TEXT IS DRAWN LAST
     * ------------------------------------------------------------
     */

    mDisplay.setFont(NULL);
    mDisplay.setTextSize(2);


    /*
     * ------------------------------------------------------------
     * TOP CENTER: STATUS
     * ------------------------------------------------------------
     */

    int16_t x1;
    int16_t y1;
    uint16_t w;
    uint16_t h;

    mDisplay.getTextBounds(
        status,
        0,
        0,
        &x1,
        &y1,
        &w,
        &h
    );

    mDisplay.setCursor(
        100 - (w / 2),
        5
    );

    mDisplay.print(status);

    /*
     * ------------------------------------------------------------
     * CURRENT TIME
     * ------------------------------------------------------------
     */

    const char* currentTime =
        "%02d:%02d:%02d";

    char timeBuffer[16];

    snprintf(
        timeBuffer,
        sizeof(timeBuffer),
        currentTime,
        mNow.Hour,
        mNow.Minute,
        mNow.Second
    );

    mDisplay.getTextBounds(
        timeBuffer,
        0,
        0,
        &x1,
        &y1,
        &w,
        &h
    );

    mDisplay.setCursor(
        100 - (w / 2),
        24
    );

    mDisplay.print(timeBuffer);

    mDisplay.setTextSize(1);


    /*
     * ------------------------------------------------------------
     * TOP LEFT: BEGIN
     * ------------------------------------------------------------
     */

    mDisplay.setCursor(5, 37);
    mDisplay.print("BEGIN");

    mDisplay.setCursor(5, 49);

    mDisplay.printf(
        "%02d:%02d:%02d",
        PARTIAL_BEGIN / 3600,
        (PARTIAL_BEGIN / 60) % 60,
        PARTIAL_BEGIN % 60
    );


    /*
     * ------------------------------------------------------------
     * TOP RIGHT: END
     * ------------------------------------------------------------
     */

    mDisplay.setCursor(157, 37);
    mDisplay.print("END");

    mDisplay.setCursor(140, 49);

    mDisplay.printf(
        "%02d:%02d:%02d",
        PARTIAL_END / 3600,
        (PARTIAL_END / 60) % 60,
        PARTIAL_END % 60
    );


    /*
     * ------------------------------------------------------------
     * BOTTOM LEFT: TOTALITY START
     * ------------------------------------------------------------
     */

    mDisplay.setCursor(5, 153);
    mDisplay.print("TOTAL ST");

    mDisplay.setCursor(5, 165);

    mDisplay.printf(
        "%02d:%02d:%02d",
        TOTAL_BEGIN / 3600,
        (TOTAL_BEGIN / 60) % 60,
        TOTAL_BEGIN % 60
    );


    /*
     * ------------------------------------------------------------
     * BOTTOM RIGHT: TOTALITY END
     * ------------------------------------------------------------
     */

    mDisplay.setCursor(140, 153);
    mDisplay.print("TOTAL EN");

    mDisplay.setCursor(140, 165);

    mDisplay.printf(
        "%02d:%02d:%02d",
        TOTAL_END / 3600,
        (TOTAL_END / 60) % 60,
        TOTAL_END % 60
    );


    /*
     * ------------------------------------------------------------
     * COVER / LIGHT
     * ------------------------------------------------------------
     */

    mDisplay.setCursor(5, 187);

    mDisplay.printf(
        "COVER %.1f%%",
        coverPercent
    );

    mDisplay.setCursor(125, 187);

    mDisplay.printf(
        "LIGHT %.1f%%",
        lightPercent
    );


    /*
     * ------------------------------------------------------------
     * DIRTY RECTANGLE
     * ------------------------------------------------------------
     */

    rects.emplace_back(
        0,
        0,
        200,
        200
    );

    return rects;
}