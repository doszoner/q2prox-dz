#pragma once

#include <string>
#include <vector>

struct ProbeOptions {
    std::string signal;
    std::string ice_file;
    std::string alias;
    int timeout_seconds = 30;
    bool stop_after_alias_channel_ready = false;
    std::vector<int> echo_sizes;
    int echo_count = 0;
};

ProbeOptions parse_probe_options(int argc, char **argv);
bool valid_alias(const std::string &alias);
bool valid_ice_json(const std::string &json);
