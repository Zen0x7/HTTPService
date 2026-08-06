#include <gtest/gtest.h>

#include <httpservice/version.hpp>

TEST(Version, ReturnsProjectVersion)
{
  EXPECT_EQ(httpservice::get_version(), "0.1.0");
}
