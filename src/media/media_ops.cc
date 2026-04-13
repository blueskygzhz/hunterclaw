// 对齐 OC: src/media/image-ops.ts (677行) + mime.ts (211行) + parse.ts (354行) + store.ts (509行)
#include "hunterclaw/media/media_system.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <map>
#include <set>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace media {
namespace ops {

// 对齐 OC mime.ts: MIME maps
static const std::map<std::string, std::string> EXT_BY_MIME = {
    {"image/jpeg", ".jpg"}, {"image/png", ".png"}, {"image/webp", ".webp"},
    {"image/gif", ".gif"}, {"image/heic", ".heic"},
    {"audio/mpeg", ".mp3"}, {"audio/wav", ".wav"}, {"audio/flac", ".flac"},
    {"audio/ogg", ".ogg"}, {"audio/opus", ".opus"}, {"audio/aac", ".aac"},
    {"video/mp4", ".mp4"}, {"video/quicktime", ".mov"},
    {"application/pdf", ".pdf"}, {"application/json", ".json"},
    {"application/zip", ".zip"}, {"text/plain", ".txt"}, {"text/markdown", ".md"},
};

static const std::set<std::string> AUDIO_EXTS = {".aac",".flac",".m4a",".mp3",".ogg",".opus",".wav"};

// 对齐 OC: detectMime (magic bytes)
std::string detect_mime(const uint8_t* buf, size_t len) {
    if (len < 4) return "";
    if (buf[0]==0x89 && buf[1]==0x50 && buf[2]==0x4E && buf[3]==0x47) return "image/png";
    if (buf[0]==0xFF && buf[1]==0xD8 && buf[2]==0xFF) return "image/jpeg";
    if (buf[0]=='G' && buf[1]=='I' && buf[2]=='F' && buf[3]=='8') return "image/gif";
    if (len>=12 && buf[0]=='R' && buf[1]=='I' && buf[2]=='F' && buf[3]=='F'
        && buf[8]=='W' && buf[9]=='E' && buf[10]=='B' && buf[11]=='P') return "image/webp";
    if (buf[0]=='%' && buf[1]=='P' && buf[2]=='D' && buf[3]=='F') return "application/pdf";
    if (buf[0]=='P' && buf[1]=='K') return "application/zip";
    if (buf[0]==0x1F && buf[1]==0x8B) return "application/gzip";
    if ((buf[0]==0xFF && (buf[1]&0xE0)==0xE0) || (buf[0]=='I'&&buf[1]=='D'&&buf[2]=='3'))
        return "audio/mpeg";
    if (buf[0]=='O' && buf[1]=='g' && buf[2]=='g' && buf[3]=='S') return "audio/ogg";
    if (buf[0]=='f' && buf[1]=='L' && buf[2]=='a' && buf[3]=='C') return "audio/flac";
    if (len>=12 && buf[0]=='R'&&buf[1]=='I'&&buf[2]=='F'&&buf[3]=='F'
        && buf[8]=='W'&&buf[9]=='A'&&buf[10]=='V'&&buf[11]=='E') return "audio/wav";
    if (len>=8 && buf[4]=='f'&&buf[5]=='t'&&buf[6]=='y'&&buf[7]=='p') return "video/mp4";
    return "";
}

std::string extension_for_mime(const std::string& mime) {
    auto it = EXT_BY_MIME.find(mime); return it != EXT_BY_MIME.end() ? it->second : "";
}

bool is_audio_file(const std::string& p) {
    std::string ext = fs::path(p).extension().string();
    std::string low; for (char c:ext) low+=static_cast<char>(std::tolower(c));
    return AUDIO_EXTS.count(low)>0;
}

// 对齐 OC image-ops.ts: Image Metadata
struct ImageMeta { int w=0, h=0; bool ok=false; };

static ImageMeta read_png_meta(const uint8_t* b, size_t len) {
    ImageMeta m; if (len<24||b[0]!=0x89||b[1]!=0x50) return m;
    m.w=(b[16]<<24)|(b[17]<<16)|(b[18]<<8)|b[19];
    m.h=(b[20]<<24)|(b[21]<<16)|(b[22]<<8)|b[23];
    m.ok=m.w>0&&m.h>0; return m;
}

static ImageMeta read_jpeg_meta(const uint8_t* b, size_t len) {
    ImageMeta m; if (len<3||b[0]!=0xFF||b[1]!=0xD8) return m;
    size_t i=2;
    while (i+4<len) {
        if (b[i]!=0xFF) break;
        uint8_t mk=b[i+1]; uint16_t l=(b[i+2]<<8)|b[i+3];
        if ((mk==0xC0||mk==0xC2)&&i+9<len) {
            m.h=(b[i+5]<<8)|b[i+6]; m.w=(b[i+7]<<8)|b[i+8];
            m.ok=m.w>0&&m.h>0; return m;
        }
        i+=2+l;
    }
    return m;
}

ImageMeta read_image_metadata(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    uint8_t hdr[1024]; f.read(reinterpret_cast<char*>(hdr),1024);
    size_t n=static_cast<size_t>(f.gcount());
    auto m=read_png_meta(hdr,n); if (m.ok) return m;
    return read_jpeg_meta(hdr,n);
}

std::vector<int> build_resize_grid(int max_side, int start) {
    std::vector<int> c={start,1800,1600,1400,1200,1000,800};
    std::vector<int> g; std::set<int> s;
    for (int v:c) { int cl=std::min(max_side,v); if (cl>0&&s.insert(cl).second) g.push_back(cl); }
    std::sort(g.begin(),g.end(),std::greater<int>()); return g;
}

// 对齐 OC parse.ts: MEDIA Token Parser
struct MediaSegment { enum T{TEXT,MEDIA} type; std::string text; std::string url; };

std::string normalize_media_source(const std::string& src) {
    return src.find("file://")==0 ? src.substr(7) : src;
}

std::vector<MediaSegment> parse_media_output(const std::string& text) {
    std::vector<MediaSegment> segs;
    std::regex re(R"(\bMEDIA:\s*`?([^\n]+)`?)");
    auto it=std::sregex_iterator(text.begin(),text.end(),re);
    auto end=std::sregex_iterator();
    size_t prev=0;
    for (;it!=end;++it) {
        size_t pos=static_cast<size_t>(it->position());
        if (pos>prev) segs.push_back({MediaSegment::TEXT, text.substr(prev,pos-prev), ""});
        std::string raw=(*it)[1].str();
        while (!raw.empty()&&(raw.back()=='`'||raw.back()=='"'||raw.back()=='\'')) raw.pop_back();
        segs.push_back({MediaSegment::MEDIA, "", normalize_media_source(raw)});
        prev=pos+it->length();
    }
    if (prev<text.size()) segs.push_back({MediaSegment::TEXT, text.substr(prev), ""});
    return segs;
}

// 对齐 OC store.ts: sanitizeFilename + cleanOldMedia
std::string sanitize_filename(const std::string& name) {
    std::string r;
    for (char c:name) { if (std::isalnum(c)||c=='.'||c=='-'||c=='_'||(unsigned char)c>=0x80) r+=c; else r+='_'; }
    r=std::regex_replace(r, std::regex("_+"), "_");
    size_t s=r.find_first_not_of('_'), e=r.find_last_not_of('_');
    if (s==std::string::npos) return "";
    r=r.substr(s,e-s+1); if (r.size()>60) r=r.substr(0,60); return r;
}

int clean_old_media(const std::string& dir, int64_t max_age_ms) {
    if (max_age_ms<=0) max_age_ms=120000; int rm=0;
    try { if (!fs::exists(dir)) return 0;
        auto now=std::chrono::system_clock::now();
        for (auto& e:fs::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            auto age=std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()-e.last_write_time().time_since_epoch()).count();
            if (age>max_age_ms) { fs::remove(e.path()); ++rm; }
        }
    } catch (...) {}
    return rm;
}

}  // namespace ops
}  // namespace media
}  // namespace hunterclaw
