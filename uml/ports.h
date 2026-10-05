#pragma once
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct PortMapping { std::string protocol; unsigned host, guest; bool lan; };
inline std::vector<PortMapping> ParsePorts(const std::string& text) {
    std::vector<PortMapping> result; std::set<std::pair<std::string, unsigned>> bound;
    std::istringstream input(text); std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row(line); std::string protocol, host, guest, scope, extra;
        if (!(row >> protocol >> host >> guest >> scope) || row >> extra ||
            (protocol != "tcp" && protocol != "udp") || (scope != "local" && scope != "lan"))
            throw std::runtime_error("Each port rule needs TCP or UDP, two port numbers and local or LAN access");
        auto port = [](const std::string& value) -> unsigned {
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("Ports must be numbers between 1 and 65535");
            unsigned long n = std::stoul(value);
            if (!n || n > 65535) throw std::runtime_error("Ports must be between 1 and 65535");
            return n;
        };
        PortMapping m{protocol, port(host), port(guest), scope == "lan"};
        if (!bound.emplace(m.protocol, m.host).second) throw std::runtime_error("A phone port can have only one destination per protocol");
        result.push_back(m);
    }
    return result;
}
inline std::vector<std::string> PortArguments(const std::string& text) {
    std::vector<std::string> args;
    const auto rules = ParsePorts(text);
    if (rules.empty()) return {"--tcp-ports", "none", "--udp-ports", "none"};
    bool tcp = false, udp = false;
    for (const auto& rule : rules) {
        (rule.protocol == "tcp" ? tcp : udp) = true;
        for (const auto* address : rule.lan ? std::initializer_list<const char*>{"0.0.0.0", "::"} : std::initializer_list<const char*>{"127.0.0.1", "::1"}) {
            args.push_back(rule.protocol == "tcp" ? "--tcp-ports" : "--udp-ports");
            const char* target = std::string(address).find(':') == std::string::npos ? "10.0.2.15" : "fd00:676f:626c::15";
            args.push_back(std::string(address) + "/" + std::to_string(rule.host) + ":" + target + "/" + std::to_string(rule.guest));
        }
    }
    if (!tcp) args.insert(args.end(), {"--tcp-ports", "none"});
    if (!udp) args.insert(args.end(), {"--udp-ports", "none"});
    return args;
}
