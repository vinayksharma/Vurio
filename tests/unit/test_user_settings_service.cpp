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

        TEST(UserSettingsServiceTest, RegisterAndListMediaLocations)
        {
            std::string db_path = "test_media_locations.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            UserSettingsService svc(dbm);
            ASSERT_TRUE(svc.initialize());

            EXPECT_TRUE(svc.registerMediaLocation("/media/A"));
            EXPECT_TRUE(svc.registerMediaLocation("/media/B"));

            auto listed = svc.listMediaLocations();
            ASSERT_TRUE(listed.find("/media/a") != listed.end());
            ASSERT_TRUE(listed.find("/media/b") != listed.end());
            EXPECT_EQ(listed["/media/a"], std::string("/media/A"));
            EXPECT_EQ(listed["/media/b"], std::string("/media/B"));
        }

        TEST(UserSettingsServiceTest, OverwriteSamePathByCaseInsensitiveKey)
        {
            std::string db_path = "test_media_overwrite.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            UserSettingsService svc(dbm);
            ASSERT_TRUE(svc.initialize());

            EXPECT_TRUE(svc.registerMediaLocation("/Media/Lib"));
            EXPECT_TRUE(svc.registerMediaLocation("/media/lib"));

            auto listed = svc.listMediaLocations();
            ASSERT_TRUE(listed.find("/media/lib") != listed.end());
            EXPECT_EQ(listed["/media/lib"], std::string("/media/lib"));
        }

        TEST(UserSettingsServiceTest, DeregisterMediaLocation)
        {
            std::string db_path = "test_media_deregister.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            UserSettingsService svc(dbm);
            ASSERT_TRUE(svc.initialize());

            EXPECT_TRUE(svc.registerMediaLocation("/mnt/data"));
            auto listed1 = svc.listMediaLocations();
            ASSERT_TRUE(listed1.find("/mnt/data") != listed1.end());

            EXPECT_TRUE(svc.deregisterMediaLocation("/mnt/data"));
            auto listed2 = svc.listMediaLocations();
            EXPECT_TRUE(listed2.find("/mnt/data") == listed2.end());
        }

    }
} // namespace

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif