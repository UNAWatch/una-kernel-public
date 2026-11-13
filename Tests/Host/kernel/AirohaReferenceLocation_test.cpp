/**
 * @file AirohaReferenceLocation_test.cpp
 * @brief Which reference locations count as "no location".
 *
 * Saving (0, 0, 0) clears the GPS reference location kept in the RTC backup
 * registers; any other location is kept and offered to the GNSS chip when it
 * asks for position aiding. Latitude and longitude are signed, so what matters
 * is that only the all-zero marker clears it, including for real fixes south
 * and west of (0, 0) at or below sea level.
 */

#include <gtest/gtest.h>

#include "AG3335/AirohaType.hpp"

using Airoha::isUnsetReferenceLocation;

namespace {

TEST(AirohaReferenceLocation, ZeroIsTheMarker)
{
    EXPECT_TRUE(isUnsetReferenceLocation(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(isUnsetReferenceLocation(-0.0f, -0.0f, -0.0f));
    EXPECT_TRUE(isUnsetReferenceLocation(0.0000005f, -0.0000005f, 0.0f));
}

TEST(AirohaReferenceLocation, SouthWestAtOrBelowSeaLevelIsKept)
{
    EXPECT_FALSE(isUnsetReferenceLocation(-22.97f, -43.18f, -4.0f));    // Rio de Janeiro, beach
    EXPECT_FALSE(isUnsetReferenceLocation(-34.60f, -58.38f, 0.0f));     // Buenos Aires, altitude unknown
    EXPECT_FALSE(isUnsetReferenceLocation(-0.5f, -0.5f, -1.0f));
}

TEST(AirohaReferenceLocation, OtherQuadrantsAreKept)
{
    EXPECT_FALSE(isUnsetReferenceLocation(51.50f, -0.12f, 11.0f));      // London
    EXPECT_FALSE(isUnsetReferenceLocation(-33.89f, 151.27f, -3.0f));    // Sydney, beach
    EXPECT_FALSE(isUnsetReferenceLocation(52.30f, 4.60f, -5.0f));       // Dutch polder
    EXPECT_FALSE(isUnsetReferenceLocation(40.70f, -74.00f, 10.0f));     // New York
}

TEST(AirohaReferenceLocation, AnyNonZeroCoordinateIsKept)
{
    EXPECT_FALSE(isUnsetReferenceLocation(0.0f, 0.0f, 5.0f));
    EXPECT_FALSE(isUnsetReferenceLocation(0.0f, 0.001f, 0.0f));
    EXPECT_FALSE(isUnsetReferenceLocation(-0.001f, 0.0f, 0.0f));
}

} // namespace
