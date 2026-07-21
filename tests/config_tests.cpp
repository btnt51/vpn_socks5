#include <atomic>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <gtest/gtest.h>

#include "server/config.h"

namespace {

class ConfigFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic_uint counter{0};
        directory_ = std::filesystem::temp_directory_path()
                   / ("vpn-config-test-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }

    void WriteConfig(std::string_view contents) const {
        std::ofstream file{directory_ / "loggers.json"};
        ASSERT_TRUE(file.is_open());
        file << contents;
        ASSERT_TRUE(file.good());
    }

    std::filesystem::path directory_;
};

constexpr std::string_view valid_config = R"json(
{
  "loggers": [{
    "module_name": "server",
    "filename": "server.log",
    "pattern": "[%l] %v",
    "level": "info",
    "rotation_hour": 23,
    "rotation_minute": 59
  }]
}
)json";

TEST_F(ConfigFileTest, ReadsValidLoggerConfig) {
    WriteConfig(valid_config);

    const auto result = config::load_config_file(directory_);

    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 1);
    EXPECT_EQ(result->front().module_name, "server");
    EXPECT_EQ(result->front().filename, "server.log");
    EXPECT_EQ(result->front().pattern, "[%l] %v");
    EXPECT_EQ(result->front().level, "info");
    EXPECT_EQ(result->front().rotation_hour, 23);
    EXPECT_EQ(result->front().rotation_minute, 59);
}

TEST(ConfigLoader, RejectsMissingConfigDirectory) {
    const auto path = std::filesystem::temp_directory_path()
                    / "vpn-config-test-directory-that-does-not-exist";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);

    const auto result = config::load_config_file(path);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("does not exist"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMissingConfigFile) {
    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("loggers.json"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMalformedJson) {
    WriteConfig(R"json({"loggers": [})json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("parsing config file"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsNonObjectRoot) {
    WriteConfig(R"json([])json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Root of config must be an object");
}

TEST_F(ConfigFileTest, RejectsMissingLoggersArray) {
    WriteConfig(R"json({})json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Required field '$.loggers' is missing");
}

TEST_F(ConfigFileTest, RejectsLoggerWithWrongFieldType) {
    WriteConfig(R"json({
      "loggers": [{
        "module_name": 42,
        "filename": "server.log",
        "pattern": "%v",
        "level": "info",
        "rotation_hour": 12,
        "rotation_minute": 30
      }]
    })json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "[0].'module_name' be string");
}

TEST_F(ConfigFileTest, RejectsDuplicateModuleNames) {
    WriteConfig(R"json({
      "loggers": [
        {
          "module_name": "server", "filename": "server.log",
          "pattern": "%v", "level": "info",
          "rotation_hour": 12, "rotation_minute": 30
        },
        {
          "module_name": "server", "filename": "other.log",
          "pattern": "%v", "level": "debug",
          "rotation_hour": 0, "rotation_minute": 0
        }
      ]
    })json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'module_name' `server` already exists"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsUnsafeLogFilename) {
    WriteConfig(R"json({
      "loggers": [{
        "module_name": "server", "filename": "../server.log",
        "pattern": "%v", "level": "info",
        "rotation_hour": 12, "rotation_minute": 30
      }]
    })json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("plain relative filename"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsEmptyLogLevel) {
    WriteConfig(R"json({
      "loggers": [{
        "module_name": "server", "filename": "server.log",
        "pattern": "%v", "level": "",
        "rotation_hour": 12, "rotation_minute": 30
      }]
    })json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'level' must be not empty"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsNonStringLogLevel) {
    WriteConfig(R"json({
      "loggers": [{
        "module_name": "server", "filename": "server.log",
        "pattern": "%v", "level": 1,
        "rotation_hour": 12, "rotation_minute": 30
      }]
    })json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "[0].'level' be string");
}

TEST_F(ConfigFileTest, RejectsUnknownLogLevel) {
    WriteConfig(R"json({
      "loggers": [{
        "module_name": "server", "filename": "server.log",
        "pattern": "%v", "level": "verbose",
        "rotation_hour": 12, "rotation_minute": 30
      }]
    })json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'level' must bet one of"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsOutOfRangeRotationTime) {
    WriteConfig(R"json({
      "loggers": [{
        "module_name": "server", "filename": "server.log",
        "pattern": "%v", "level": "info",
        "rotation_hour": 24, "rotation_minute": 0
      }]
    })json");

    const auto result = config::load_config_file(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'rotation_hour' must be in range [0, 23]"), std::string::npos);
}

} // namespace
