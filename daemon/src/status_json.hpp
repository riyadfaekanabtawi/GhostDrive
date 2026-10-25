#pragma once

#include <cstdio>
#include <string>
#include <string_view>

#include "ghostdrive/daemon/service.hpp"

namespace ghostdrive::daemon {

// Appends `text` as a JSON string literal. Bytes are passed through unchanged except the ones
// JSON requires escaping, so valid UTF-8 stays valid.
inline void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

// The `ghostdrive status --json` text: one JSON object and a newline. An empty `mountpoint`
// (not mounted) and an absent rendezvous server, observed address or admission ID are encoded
// as null.
inline std::string format_status_json(const ServiceStatus& status, const std::string& mountpoint) {
    std::string out = "{";
    auto key = [&](std::string_view name) {
        if (out.size() > 1 && out.back() != '{' && out.back() != '[') out += ',';
        append_json_string(out, name);
        out += ':';
    };
    auto string_or_null = [&](std::string_view name, const std::string& value) {
        key(name);
        if (value.empty()) {
            out += "null";
        } else {
            append_json_string(out, value);
        }
    };
    auto boolean = [&](std::string_view name, bool value) {
        key(name);
        out += value ? "true" : "false";
    };

    key("device");
    append_json_string(out, status.device_name);
    key("id");
    append_json_string(out, status.fingerprint);
    string_or_null("mountpoint", mountpoint);
    key("port");
    out += std::to_string(status.port);

    key("rendezvous");
    if (status.rendezvous.empty()) {
        out += "null";
    } else {
        out += '{';
        key("address");
        append_json_string(out, status.rendezvous);
        boolean("connected", status.observed.has_value());
        key("observed");
        if (status.observed) {
            append_json_string(out, status.observed->address().to_string() + ":" +
                                        std::to_string(status.observed->port()));
        } else {
            out += "null";
        }
        out += '}';
    }

    string_or_null("removed_by", status.removed_by);
    string_or_null("discovery_error", status.discovery_error);

    key("admission");
    if (status.admission.empty()) {
        out += "null";
    } else {
        out += '{';
        key("id");
        append_json_string(out, status.admission);
        key("generation");
        out += std::to_string(status.admission_generation);
        key("earlier");
        out += '[';
        for (const auto& id : status.earlier_admissions) {
            if (out.back() != '[') out += ',';
            append_json_string(out, id);
        }
        out += ']';
        boolean("members_caught_up", status.members_caught_up);
        out += '}';
    }

    key("peers");
    out += '[';
    for (const auto& p : status.peers) {
        if (out.back() != '[') out += ',';
        out += '{';
        key("name");
        append_json_string(out, p.name);
        key("id");
        append_json_string(out, core::fingerprint(p.key));
        string_or_null("address", p.address);
        boolean("online", p.connected);
        boolean("relayed", p.relayed);
        out += '}';
    }
    out += "]}\n";
    return out;
}

}  // namespace ghostdrive::daemon
