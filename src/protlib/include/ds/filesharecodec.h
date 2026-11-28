#pragma once

#include "darkspeak.qpb.h"
#include <QByteArrayView>

namespace ds::prot::fileshare {
inline constexpr qsizetype max_control_bytes = 262144;
inline constexpr qsizetype max_path_bytes = 4096;
inline constexpr qsizetype max_component_bytes = 765;
inline constexpr qsizetype max_cursor_bytes = 2048;
inline constexpr qsizetype max_page_entries = 100;
inline constexpr size_t max_pending_requests = 16;

bool isRequest(const protocol::Envelope &envelope);
bool isResponse(const protocol::Envelope &envelope);
// Checks raw UTF-8, cardinality, nesting and field types before Qt constructs
// strings. Ordinary non-FileShare envelopes keep their existing validation.
bool validateWire(QByteArrayView bytes);
bool validPath(const protocol::VirtualPath &path, bool allow_utf8);
bool validate(const protocol::Envelope &envelope, bool allow_utf8);
bool matches(const protocol::Envelope &request, const protocol::Envelope &response);
}
