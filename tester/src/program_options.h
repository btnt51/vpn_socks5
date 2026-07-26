#pragma once

#include <expected>
#include <string>

#include <boost/program_options.hpp>

#include "runner.h"

namespace tester {

enum class command {
    help,
    list,
    run,
};

struct settings {
    command selected_command{command::help};
    run_config config;
};

class program_options {
public:
    program_options();

    std::expected<settings, std::string> parse(int argc, char* argv[]) const;
    std::string help() const;

private:
    boost::program_options::options_description options_;
    boost::program_options::positional_options_description positional_;
};

} // namespace tester
