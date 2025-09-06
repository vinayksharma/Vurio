#include <gtest/gtest.h>
#include "database/database_manager.hpp"
#include "database/user_settings_service.hpp"

namespace MediaDedup
{
    namespace Test
    {

        TEST(UserSettingsServiceTest, CrudLifecycle)
        {
            std::string db_path = "test_user_settings.sqlite";
            // Ensure clean slate
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());

            UserSettingsService svc(dbm);
            ASSERT_TRUE(svc.initialize());

            // Upsert
            EXPECT_TRUE(svc.upsertSetting("theme", "dark"));
            // Get
            std::string value;
            EXPECT_TRUE(svc.getSetting("theme", value));
            EXPECT_EQ(value, std::string("dark"));
            // Update
            EXPECT_TRUE(svc.upsertSetting("theme", "light"));
            EXPECT_TRUE(svc.getSetting("theme", value));
            EXPECT_EQ(value, std::string("light"));
            // List
            auto map = svc.listSettings();
            ASSERT_TRUE(map.find("theme") != map.end());
            EXPECT_EQ(map["theme"], std::string("light"));
            // Delete
            EXPECT_TRUE(svc.deleteSetting("theme"));
            EXPECT_FALSE(svc.getSetting("theme", value));
        }

    }
} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}