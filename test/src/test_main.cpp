#include "nestor/version.hpp"

#include <gtest/gtest.h>

namespace nestor::test {

TEST(VersionTest, ProjectNameIsCorrect) { EXPECT_EQ(kProjectName, "Nestor"); }

TEST(VersionTest, VersionIsSet) { EXPECT_FALSE(kVersion.empty()); }

TEST(VersionTest, GetVersionReturnsVersion) { EXPECT_STREQ(get_version(), kVersion.data()); }

TEST(VersionTest, GetProjectNameReturnsName) {
  EXPECT_STREQ(get_project_name(), kProjectName.data());
}

}  // namespace nestor::test
