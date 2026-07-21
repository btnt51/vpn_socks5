#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <vpn/logger.h>

namespace {

class LoggerRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic_uint counter{0};
        directory_ = std::filesystem::temp_directory_path()
                   / ("vpn-logger-test-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }

    static logger::config MakeConfig(const std::filesystem::path& directory) {
        return logger::config{
            .log_directory = directory.string(),
            .settings = {{
                .module_name = "test",
                .filename = "test.log",
                .pattern = "%v",
                .level = "info",
                .rotation_hour = 12,
                .rotation_minute = 30,
            }},
        };
    }

    std::filesystem::path directory_;
};

TEST_F(LoggerRuntimeTest, CreatesFileInConfiguredDirectory) {
    auto runtime = logger::runtime::create(MakeConfig(directory_));

    ASSERT_TRUE(runtime.has_value()) << runtime.error();
    runtime.value()->get().log("test", logger_levels::e_info, "configured directory test");
    runtime.value().reset();

    bool contains_log_file = false;
    for (const auto& entry : std::filesystem::directory_iterator{directory_}) {
        contains_log_file |= entry.is_regular_file();
    }
    EXPECT_TRUE(contains_log_file);
}

TEST_F(LoggerRuntimeTest, ReturnsControlledErrorWhenFileSinkCannotBeCreated) {
    const auto regular_file = directory_ / "not-a-directory";
    std::ofstream{regular_file} << "content";

    const auto runtime = logger::runtime::create(MakeConfig(regular_file));

    ASSERT_FALSE(runtime.has_value());
    EXPECT_FALSE(runtime.error().empty());
}

}  // namespace
