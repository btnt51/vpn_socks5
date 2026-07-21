#include <atomic>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <gtest/gtest.h>

#include <vpn/config.h>

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

    void WriteConfig(std::string_view contents, bool add_log_directory = true) const {
        std::string config{contents};
        const auto root_start = config.find_first_not_of(" \t\r\n");
        if (add_log_directory and root_start != std::string::npos and config[root_start] == '{' and
            config.find("\"log_directory\"") == std::string::npos) {
            const auto first_property = config.find_first_not_of(" \t\r\n", root_start + 1);
            const auto separator = first_property != std::string::npos and config[first_property] == '}' ? "" : ",";
            config.insert(root_start + 1,
                "\n  \"log_directory\": \"" + directory_.generic_string() + "\"" + separator);
        }

        std::ofstream file{directory_ / "loggers.json"};
        ASSERT_TRUE(file.is_open());
        file << config;
        ASSERT_TRUE(file.good());
    }

    void WriteServerConfig(std::string_view contents) const {
        if (not std::filesystem::exists(directory_ / "loggers.json")) {
            WriteConfig(R"json({
              "loggers": [{
                "module_name": "server",
                "filename": "server.log",
                "pattern": "%v",
                "level": "info",
                "rotation_hour": 12,
                "rotation_minute": 30
              }]
            })json");
        }

        std::ofstream file{directory_ / "server.json"};
        ASSERT_TRUE(file.is_open());
        file << contents;
        ASSERT_TRUE(file.good());
    }

    void WriteValidServerConfig() const {
        WriteServerConfig(R"json({
          "server": {"ip": "127.0.0.1", "port": 1080}
        })json");
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
    WriteValidServerConfig();

    const auto result = config::load_config(directory_);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->loggers.log_directory, directory_.generic_string());
    ASSERT_EQ(result->loggers.settings.size(), 1);
    EXPECT_EQ(result->loggers.settings.front().module_name, "server");
    EXPECT_EQ(result->loggers.settings.front().filename, "server.log");
    EXPECT_EQ(result->loggers.settings.front().pattern, "[%l] %v");
    EXPECT_EQ(result->loggers.settings.front().level, "info");
    EXPECT_EQ(result->loggers.settings.front().rotation_hour, 23);
    EXPECT_EQ(result->loggers.settings.front().rotation_minute, 59);
}

TEST_F(ConfigFileTest, RejectsMissingLogDirectory) {
    WriteConfig(valid_config, false);
    WriteValidServerConfig();

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Required field '$.log_directory' is missing"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsNonStringLogDirectory) {
    WriteConfig(R"json({
      "log_directory": 42,
      "loggers": []
    })json");
    WriteValidServerConfig();

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'$.log_directory' must be string"), std::string::npos);
}

TEST(ConfigLoader, RejectsMissingConfigDirectory) {
    const auto path = std::filesystem::temp_directory_path()
                    / "vpn-config-test-directory-that-does-not-exist";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);

    const auto result = config::load_config(path);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("does not exist"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMissingConfigFile) {
    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("loggers.json"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMalformedJson) {
    WriteConfig(R"json({"loggers": [})json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("parsing config file"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsNonObjectRoot) {
    WriteConfig(R"json([])json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Root of config must be an object"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMissingLoggersArray) {
    WriteConfig(R"json({})json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Required field '$.loggers' is missing"), std::string::npos);
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

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("[0].'module_name' be string"), std::string::npos);
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

    const auto result = config::load_config(directory_);

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

    const auto result = config::load_config(directory_);

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

    const auto result = config::load_config(directory_);

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

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("[0].'level' be string"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsUnknownLogLevel) {
    WriteConfig(R"json({
      "loggers": [{
        "module_name": "server", "filename": "server.log",
        "pattern": "%v", "level": "verbose",
        "rotation_hour": 12, "rotation_minute": 30
      }]
    })json");

    const auto result = config::load_config(directory_);

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

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'rotation_hour' must be in range [0, 23]"), std::string::npos);
}

TEST_F(ConfigFileTest, ReadsValidServerConfig) {
    WriteServerConfig(R"json({
      "server": {"ip": "127.0.0.1", "port": 1080}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->server.address, "127.0.0.1");
    EXPECT_EQ(result->server.port, 1080);
}

TEST_F(ConfigFileTest, ReadsValidIpv6ServerConfig) {
    WriteServerConfig(R"json({
      "server": {"ip": "2001:db8::1", "port": 443}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->server.address, "2001:db8::1");
    EXPECT_EQ(result->server.port, 443);
}

TEST_F(ConfigFileTest, RejectsMissingServerConfigFile) {
    WriteConfig(valid_config);

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("server.json"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMalformedServerJson) {
    WriteServerConfig(R"json({"server": {)json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("parsing config file"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMissingServerObject) {
    WriteServerConfig(R"json({})json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Required field '$.server' is missing"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsNonObjectServerConfig) {
    WriteServerConfig(R"json({"server": []})json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'server' must be an object"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMissingServerIp) {
    WriteServerConfig(R"json({
      "server": {"port": 1080}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("must contains 'ip'"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsNonStringServerIp) {
    WriteServerConfig(R"json({
      "server": {"ip": 127, "port": 1080}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'ip' must be a string"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsInvalidServerIp) {
    WriteServerConfig(R"json({
      "server": {"ip": "999.999.999.999", "port": 1080}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Could not make address"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsMissingServerPort) {
    WriteServerConfig(R"json({
      "server": {"ip": "127.0.0.1"}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("must contains 'port'"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsNonIntegerServerPort) {
    WriteServerConfig(R"json({
      "server": {"ip": "127.0.0.1", "port": "1080"}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'port' must be an integer"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsServerPortBelowRange) {
    WriteServerConfig(R"json({
      "server": {"ip": "127.0.0.1", "port": -1}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'port' must be in range [0, 65535]"), std::string::npos);
}

TEST_F(ConfigFileTest, RejectsServerPortAboveRange) {
    WriteServerConfig(R"json({
      "server": {"ip": "127.0.0.1", "port": 65536}
    })json");

    const auto result = config::load_config(directory_);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("'port' must be in range [0, 65535]"), std::string::npos);
}

}  // namespace
