#include "ds/filesharecodec.h"
#include <QStringDecoder>
#include <limits>

namespace ds::prot::fileshare {
using namespace ds::protocol;
namespace {
bool enumEncoding(VirtualPathEncodingGadget::VirtualPathEncoding e, bool allow_utf8) {
    return int(e) == 2 || (int(e) == 1 && allow_utf8);
}
bool component(QStringView value, int encoding) {
    if (value.isEmpty() || value.size() > max_component_bytes * 3 || !value.isValidUtf16()) return false;
    QByteArray logical;
    if (encoding == 2) {
        auto hex = [](QChar c) { auto u = c.unicode(); return u >= '0' && u <= '9' ? u-'0' : u >= 'A' && u <= 'F' ? u-'A'+10 : -1; };
        for (qsizetype i = 0; i < value.size(); ++i) {
            auto c = value[i].unicode();
            if (c == '%') {
                if (i + 2 >= value.size()) return false;
                int a = hex(value[i+1]), b = hex(value[i+2]);
                if (a < 0 || b < 0) return false;
                c = (a << 4) | b; i += 2;
                if (c >= 32 && c <= 126 && c != '%') return false;
            } else if (c < 32 || c > 126) return false;
            logical.append(char(c));
        }
    } else logical = value.toUtf8();
    if (logical.size() > max_component_bytes) return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(logical);
    // Round-trip catches buffered incomplete sequences and an initial BOM that
    // QStringDecoder would otherwise strip, as well as replacement decoding.
    if (decoder.hasError() || decoded.toUtf8() != logical || decoded == "." || decoded == "..") return false;
    for (auto scalar : decoded.toUcs4()) {
        const auto category = QChar::category(scalar);
        if (!scalar || scalar == '/' || category == QChar::Other_Control || category == QChar::Other_Format) return false;
    }
    return true;
}
bool node(const QByteArray &id, bool optional = false) { return (optional && id.isEmpty()) || id.size() == 32; }
bool hash(const QByteArray &id) { return id.isEmpty() || id.size() == 32; }
bool transfer(const QByteArray &id) { return id.size() == 16 && id != QByteArray(16, '\0'); }
bool size(quint64 n) { return n <= quint64(std::numeric_limits<qint64>::max()); }
bool status(FileShareStatusGadget::FileShareStatus s) { return int(s) >= 1 && int(s) <= 9; }
bool entry(const DirectoryEntry &e, bool allow_utf8, int encoding) {
    if (!node(e.nodeId()) || !e.hasName() || !enumEncoding(e.name().encoding(), allow_utf8) ||
        int(e.name().encoding()) != encoding || !component(e.name().value(), encoding) ||
        (int(e.type()) != 1 && int(e.type()) != 2) || (int(e.state()) != 1 && int(e.state()) != 2) || !hash(e.sha256())) return false;
    if (int(e.type()) == 2 && (e.hasSize() || e.hasModifiedUtcDay() || !e.sha256().isEmpty())) return false;
    if (e.hasSize() && !size(e.size())) return false;
    // Fixed portable civil-date interval, independent of host clock/OS.
    if (e.hasModifiedUtcDay() && (e.modifiedUtcDay() < -719162 || e.modifiedUtcDay() > 2932896)) return false;
    return int(e.state()) != 2 || e.sha256().isEmpty();
}
bool samePath(const VirtualPath &a, const VirtualPath &b) { return a.encoding() == b.encoding() && a.path() == b.path(); }

// Minimal bounded protobuf walk. Only FileShare has this strict schema: unknown
// fields and duplicate singular values cannot smuggle hidden response data.
bool varint(QByteArrayView b, qsizetype &p, quint64 &n) {
    n = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
        if (p == b.size()) return false;
        auto c = quint8(b[p++]);
        if (shift == 63 && c > 1) return false;
        n |= quint64(c & 127) << shift;
        if (!(c & 128)) return true;
    }
    return false;
}
enum class Shape { envelope, path, name, entry, list_request, list_result, stat_request, stat_result, download, download_result, cancel, cancel_result };
struct Field { int wire = -1; bool text = false; bool nested = false; Shape shape = Shape::path; bool repeated = false; qsizetype maximum = max_control_bytes; };
Field field(Shape s, unsigned f) {
    auto number = [] { return Field{0}; };
    auto bytes = [](qsizetype maximum) { Field v{2}; v.maximum = maximum; return v; };
    auto child = [](Shape shape, bool repeated = false) { Field v{2}; v.nested = true; v.shape = shape; v.repeated = repeated; return v; };
    switch (s) {
    case Shape::envelope:
        if (f >= 1 && f <= 3) return number();
        if (f >= 32 && f <= 39) {
            constexpr Shape shapes[]{Shape::list_request,Shape::list_result,Shape::stat_request,Shape::stat_result,Shape::download,Shape::download_result,Shape::cancel,Shape::cancel_result};
            return child(shapes[f-32]);
        }
        break;
    case Shape::path: case Shape::name:
        if (f == 1) return number();
        if (f == 2) { auto v = bytes(s == Shape::path ? max_path_bytes : max_component_bytes*3); v.text = true; return v; }
        break;
    case Shape::entry:
        if (f == 1) return bytes(32);
        if (f == 2) return child(Shape::name);
        if (f >= 3 && f <= 6) return number();
        if (f == 7) return bytes(32);
        break;
    case Shape::list_request:
        if (f == 1) return child(Shape::path);
        if (f == 2 || f == 3) return number();
        if (f == 4) return bytes(max_cursor_bytes);
        break;
    case Shape::list_result:
        if (f == 1 || f == 5 || f == 7) return number();
        if (f == 2) return child(Shape::path);
        if (f == 3) return child(Shape::entry,true);
        if (f == 4) return bytes(max_cursor_bytes);
        if (f == 6) return bytes(16);
        break;
    case Shape::stat_request: case Shape::download:
        if (f == 1) return child(Shape::path);
        if (f == 2 || (f == 3 && s == Shape::download)) return bytes(32);
        if (f == 4 && s == Shape::download) return number();
        break;
    case Shape::stat_result:
        if (f == 1) return number();
        if (f == 2) return child(Shape::entry);
        if (f == 3) return child(Shape::path);
        break;
    case Shape::download_result:
        if (f == 1) return number();
        if (f == 2) return bytes(16);
        break;
    case Shape::cancel: case Shape::cancel_result:
        if (f == 1) return number();
        break;
    }
    return {};
}
bool scan(QByteArrayView b, Shape s, unsigned &budget) {
    quint64 seen = 0; unsigned entries = 0, bodies = 0, correlations = 0;
    qsizetype p = 0;
    while (p < b.size()) {
        if (!budget--) return false;
        quint64 tag = 0, n = 0;
        if (!varint(b,p,tag) || !(tag >> 3) || (tag >> 3) > 63) return false;
        auto f = unsigned(tag >> 3); auto spec = field(s,f);
        if (spec.wire != int(tag & 7) || ((seen & (quint64(1)<<f)) && !spec.repeated)) return false;
        seen |= quint64(1)<<f;
        if (s == Shape::envelope && ((f >= 32 && ++bodies > 1) || ((f == 2 || f == 3) && ++correlations > 1))) return false;
        if (spec.repeated && ++entries > max_page_entries) return false;
        if (spec.wire == 0) {
            if (!varint(b,p,n)) return false;
            // Reject narrowing/coercion before generated protobuf setters can
            // turn huge enums/uint32s or non-boolean integers into valid values.
            if ((s == Shape::path || s == Shape::name) && f == 1 && (n < 1 || n > 2)) return false;
            if (((s == Shape::list_request && f == 2) || (s == Shape::list_result && f == 5)) && n > 1) return false;
            if (s == Shape::list_request && f == 3 && n > max_page_entries) return false;
            if (s == Shape::list_result && f == 7 && n != 2) return false;
            if ((s == Shape::list_result || s == Shape::stat_result || s == Shape::download_result || s == Shape::cancel_result) && f == 1 && (n < 1 || n > 9)) return false;
            if (s == Shape::entry && (f == 3 || f == 4) && (n < 1 || n > 2)) return false;
            if (s == Shape::envelope && f == 1 && n != 3) return false;
            continue;
        }
        if (!varint(b,p,n) || n > quint64(b.size()-p) || n > quint64(spec.maximum)) return false;
        auto value = b.sliced(p,qsizetype(n)); p += qsizetype(n);
        if (spec.text) { QStringDecoder decoder(QStringDecoder::Utf8); const QString text = decoder(value); if (decoder.hasError() || text.toUtf8() != value) return false; }
        if (spec.nested && !scan(value,spec.shape,budget)) return false;
    }
    return true;
}
}
bool isRequest(const Envelope &e) { return e.hasListDirectoryRequest() || e.hasStatVirtualRequest() || e.hasRequestDownload() || e.hasCancelFileShareRequest(); }
bool isResponse(const Envelope &e) { return e.hasListDirectoryResult() || e.hasStatVirtualResult() || e.hasRequestDownloadResult() || e.hasCancelFileShareResult(); }
bool validateWire(QByteArrayView b) {
    // Detect the feature by raw field number before invoking its stricter walker.
    qsizetype p = 0; bool feature = false;
    while (p < b.size()) {
        quint64 tag = 0, n = 0;
        if (!varint(b,p,tag)) return false;
        feature |= (tag >> 3) >= 32 && (tag >> 3) <= 39;
        switch (tag & 7) {
        case 0: if (!varint(b,p,n)) return false; break;
        case 1: if (b.size()-p < 8) return false; p += 8; break;
        case 5: if (b.size()-p < 4) return false; p += 4; break;
        case 2: if (!varint(b,p,n) || n > quint64(b.size()-p)) return false; p += qsizetype(n); break;
        default: return false;
        }
    }
    if (!feature) return true;
    unsigned budget = 2048;
    return b.size() <= max_control_bytes && scan(b,Shape::envelope,budget);
}
bool validPath(const VirtualPath &p, bool allow_utf8) {
    const auto value = p.path();
    if (!enumEncoding(p.encoding(),allow_utf8) || value.isEmpty() || value.size() > max_path_bytes || value.toUtf8().size() > max_path_bytes || value.front() != '/') return false;
    if (value == "/") return true;
    auto parts = QStringView(value).sliced(1).split('/');
    if (parts.size() > 64) return false;
    for (auto part : parts) if (!component(part,int(p.encoding()))) return false;
    return true;
}
bool validate(const Envelope &e, bool allow_utf8) {
    if (e.protocolVersion() != 3 || (!isRequest(e) && !isResponse(e))) return false;
    if (isRequest(e) ? (!e.hasRequestId() || !e.requestId()) : (!e.hasResponseToRequestId() || !e.responseToRequestId())) return false;
    if (e.hasListDirectoryRequest()) {
        const auto r = e.listDirectoryRequest();
        return r.hasDirectory() && validPath(r.directory(),allow_utf8) && r.pageSize() <= max_page_entries && r.continuation().size() <= max_cursor_bytes && (!r.recursive() || (!r.pageSize() && r.continuation().isEmpty()));
    }
    if (e.hasStatVirtualRequest()) { const auto r = e.statVirtualRequest(); return r.hasPath() && validPath(r.path(),allow_utf8) && node(r.knownNodeId(),true); }
    if (e.hasRequestDownload()) { const auto r = e.requestDownload(); return r.hasPath() && validPath(r.path(),allow_utf8) && node(r.knownNodeId(),true) && hash(r.expectedSha256()) && (!r.hasExpectedSize() || size(r.expectedSize())); }
    if (e.hasCancelFileShareRequest()) { auto id = e.cancelFileShareRequest().requestToCancel(); return id && id < e.requestId(); }
    if (e.hasCancelFileShareResult()) return status(e.cancelFileShareResult().status()) && int(e.cancelFileShareResult().status()) != 3;
    if (e.hasRequestDownloadResult()) { const auto r = e.requestDownloadResult(); return status(r.status()) && int(r.status()) != 3 && (int(r.status()) == 1 ? transfer(r.transferId()) : r.transferId().isEmpty()); }
    if (e.hasStatVirtualResult()) {
        const auto r = e.statVirtualResult();
        if (!status(r.status()) || int(r.status()) == 3) return false;
        if (int(r.status()) != 1) return !r.hasEntry() && !r.hasPath();
        return r.hasPath() && validPath(r.path(),allow_utf8) && r.hasEntry() && entry(r.entry(),allow_utf8,int(r.path().encoding()));
    }
    const auto r = e.listDirectoryResult();
    if (!status(r.status())) return false;
    if (int(r.status()) != 1) return !r.hasDirectory() && r.entries().isEmpty() && r.continuation().isEmpty() && !r.complete() && r.catalogTransferId().isEmpty() && !r.catalogFormatVersion();
    if (!r.hasDirectory() || !validPath(r.directory(),allow_utf8)) return false;
    if (!r.catalogTransferId().isEmpty() || r.catalogFormatVersion()) return transfer(r.catalogTransferId()) && r.catalogFormatVersion() == 2 && r.entries().isEmpty() && r.continuation().isEmpty() && !r.complete();
    if (r.entries().size() > max_page_entries || r.continuation().size() > max_cursor_bytes || (r.complete() != r.continuation().isEmpty())) return false;
    for (const auto &item : r.entries()) if (!entry(item,allow_utf8,int(r.directory().encoding()))) return false;
    return true;
}
bool matches(const Envelope &request, const Envelope &response) {
    if (request.requestId() != response.responseToRequestId()) return false;
    if (request.hasListDirectoryRequest() && response.hasListDirectoryResult()) {
        const auto a = request.listDirectoryRequest(); const auto b = response.listDirectoryResult();
        if (int(b.status()) != 1) return int(b.status()) != 3 || a.recursive();
        return samePath(a.directory(),b.directory()) && a.recursive() == !b.catalogTransferId().isEmpty() && (a.recursive() || b.entries().size() <= (a.pageSize() ? a.pageSize() : max_page_entries));
    }
    if (request.hasStatVirtualRequest() && response.hasStatVirtualResult()) {
        const auto a = request.statVirtualRequest(); const auto b = response.statVirtualResult();
        return int(b.status()) != 1 || (samePath(a.path(),b.path()) && (a.knownNodeId().isEmpty() || a.knownNodeId() == b.entry().nodeId()));
    }
    return (request.hasRequestDownload() && response.hasRequestDownloadResult()) || (request.hasCancelFileShareRequest() && response.hasCancelFileShareResult());
}
}
