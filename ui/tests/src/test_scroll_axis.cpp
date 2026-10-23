#include "tests/includes/test_framework.h"

#include "msimeui/ScrollAxis.h"

using namespace msimeui;

// Expected values are worked out by hand.

TEST_CASE(scroll_axis_wheel_page_scroll_moves_most_of_a_viewport)
{
    REQUIRE_NEAR(WheelNotchDistance(WHEEL_PAGESCROLL, 60.0f, 180.0f), 157.5f);
    // Never less than a notch at the default setting.
    REQUIRE_NEAR(WheelNotchDistance(WHEEL_PAGESCROLL, 60.0f, 40.0f), 60.0f);
}

TEST_CASE(scroll_axis_glide_eases_out_from_start_to_target)
{
    const ScrollGlide glide = ScrollGlide::Start(0.0f, 0.0f, 100.0f, 1000.0, 260.0);
    REQUIRE_NEAR(glide.durationMs, 260.0);
    REQUIRE_NEAR(glide.OffsetAt(900.0), 0.0f);
    REQUIRE_NEAR(glide.OffsetAt(1000.0), 0.0f);
    // Halfway in time: 1 - 0.5^3 of the way.
    REQUIRE_NEAR(glide.OffsetAt(1130.0), 87.5f);
    REQUIRE_NEAR(glide.OffsetAt(1260.0), 100.0f);
    REQUIRE_NEAR(glide.OffsetAt(5000.0), 100.0f);
    REQUIRE(!glide.IsDoneAt(1259.0));
    REQUIRE(glide.IsDoneAt(1260.0));
}

TEST_CASE(scroll_axis_glide_velocity_is_fastest_at_the_start)
{
    const ScrollGlide glide = ScrollGlide::Start(0.0f, 0.0f, 100.0f, 1000.0, 260.0);
    REQUIRE_NEAR(glide.VelocityAt(1000.0), 300.0f / 260.0f);
    REQUIRE_NEAR(glide.VelocityAt(1130.0), 75.0f / 260.0f);
    REQUIRE_NEAR(glide.VelocityAt(1260.0), 0.0f);
    const ScrollGlide back = ScrollGlide::Start(100.0f, 0.0f, 0.0f, 0.0, 260.0);
    REQUIRE(back.VelocityAt(0.0) < 0.0f);
}

TEST_CASE(scroll_axis_glide_that_starts_slower_than_the_content_moves_is_shortened)
{
    // Moving at 2 DIPs/ms towards a target 60 away: a 260 ms glide would start at
    // 3 * 60 / 260 per ms, slower, so it is cut to 3 * 60 / 2 = 90 ms and starts at 2.
    const ScrollGlide glide = ScrollGlide::Start(50.0f, 2.0f, 110.0f, 0.0, 260.0);
    REQUIRE_NEAR(glide.durationMs, 90.0);
    REQUIRE_NEAR(glide.VelocityAt(0.0), 2.0f);
    REQUIRE_NEAR(glide.OffsetAt(90.0), 110.0f);
}

TEST_CASE(scroll_axis_glide_keeps_its_duration_when_moving_away_or_slowly)
{
    REQUIRE_NEAR(ScrollGlide::Start(50.0f, -2.0f, 110.0f, 0.0, 260.0).durationMs, 260.0);
    REQUIRE_NEAR(ScrollGlide::Start(50.0f, 0.1f, 110.0f, 0.0, 260.0).durationMs, 260.0);
}

TEST_CASE(scroll_axis_shortened_glide_lasts_at_least_one_millisecond)
{
    REQUIRE_NEAR(ScrollGlide::Start(50.0f, 2.0f, 50.001f, 0.0, 260.0).durationMs, 1.0);
}
