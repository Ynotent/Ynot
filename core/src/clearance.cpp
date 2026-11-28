#include "ynot/clearance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>

#include "ynot/sampler.hpp"

namespace ynot {

const char* clearanceStatusId(ClearanceStatus s) {
    switch (s) {
        case ClearanceStatus::Unknown: return "unknown";
        case ClearanceStatus::Original: return "original";
        case ClearanceStatus::RoyaltyFree: return "royaltyFree";
        case ClearanceStatus::Requested: return "requested";
        case ClearanceStatus::Cleared: return "cleared";
        case ClearanceStatus::Denied: return "denied";
    }
    return "unknown";
}

const char* clearanceStatusName(ClearanceStatus s) {
    switch (s) {
        case ClearanceStatus::Unknown: return "Not cleared yet";
        case ClearanceStatus::Original: return "My own audio";
        case ClearanceStatus::RoyaltyFree: return "Royalty-free";
        case ClearanceStatus::Requested: return "Clearance requested";
        case ClearanceStatus::Cleared: return "Cleared";
        case ClearanceStatus::Denied: return "Denied: replace it";
    }
    return "Not cleared yet";
}

ClearanceStatus clearanceStatusFromId(const std::string& id) {
    for (auto s : {ClearanceStatus::Unknown, ClearanceStatus::Original, ClearanceStatus::RoyaltyFree,
                   ClearanceStatus::Requested, ClearanceStatus::Cleared, ClearanceStatus::Denied})
        if (id == clearanceStatusId(s)) return s;
    return ClearanceStatus::Unknown;
}

bool needsClearance(ClearanceStatus s) {
    return s == ClearanceStatus::Unknown || s == ClearanceStatus::Requested || s == ClearanceStatus::Denied;
}

ClearanceStatus defaultClearance(const MediaRef& m) {
    if (m.origin == "import" || m.origin == "sample") return ClearanceStatus::Unknown;
    if (m.origin == "pack") return ClearanceStatus::RoyaltyFree;
    return ClearanceStatus::Original;  // recordings, Ynot-generated ideas, renders
}

const SampleCredit* findCredit(const Project& p, const std::string& mediaId) {
    for (const auto& c : p.credits)
        if (c.mediaId == mediaId) return &c;
    return nullptr;
}

void setCredit(Project& p, const SampleCredit& c) {
    for (auto& x : p.credits)
        if (x.mediaId == c.mediaId) {
            x = c;
            return;
        }
    p.credits.push_back(c);
}

void noteImportedFile(Project& p, const std::string& mediaId, const std::string& fileName, ClearanceStatus status) {
    if (mediaId.empty()) return;
    SampleCredit c;
    if (const auto* old = findCredit(p, mediaId)) c = *old;  // the same file imported again
    else c.status = status;
    c.mediaId = mediaId;
    if (c.fileName.empty()) c.fileName = fileName;
    setCredit(p, c);
}

namespace {
ClearanceStatus statusOf(const Project& p, const std::string& mediaId) {
    if (const auto* c = findCredit(p, mediaId)) return c->status;
    const auto* m = p.findMedia(mediaId);
    return m ? defaultClearance(*m) : ClearanceStatus::Unknown;
}
}  // namespace

void inheritCredit(Project& p, const std::string& newMediaId, const std::string& fromMediaId) {
    SampleCredit c;
    if (const auto* src = findCredit(p, fromMediaId)) c = *src;
    else c.status = statusOf(p, fromMediaId);
    c.mediaId = newMediaId;
    setCredit(p, c);
}

std::vector<SampleUse> clearanceReport(const Project& p) {
    std::vector<SampleUse> out;
    std::map<std::string, size_t> index;
    auto use = [&](const std::string& mediaId, const std::string& trackName) -> SampleUse& {
        auto it = index.find(mediaId);
        if (it == index.end()) {
            SampleUse u;
            u.mediaId = mediaId;
            u.status = statusOf(p, mediaId);
            const auto* c = findCredit(p, mediaId);
            const auto* m = p.findMedia(mediaId);
            const bool recording = m && m->origin == "recording";
            int n = 1;  // numbered within its kind
            for (const auto& o : out) {
                const auto* om = p.findMedia(o.mediaId);
                n += ((om && om->origin == "recording") == recording) ? 1 : 0;
            }
            u.name = c && !c->title.empty()        ? c->title + (c->artist.empty() ? "" : " - " + c->artist)
                     : c && !c->fileName.empty()  ? c->fileName
                     : recording                  ? "Your recording " + std::to_string(n)
                                                  : "Sample " + std::to_string(n);
            index[mediaId] = out.size();
            out.push_back(u);
            it = index.find(mediaId);
        }
        auto& u = out[it->second];
        if (std::find(u.trackNames.begin(), u.trackNames.end(), trackName) == u.trackNames.end())
            u.trackNames.push_back(trackName);
        return u;
    };
    for (const auto* t : p.orderedTracks()) {
        for (const auto& c : t->audioClips) {
            const std::string root = c.stretchSourceMediaId.empty() ? c.mediaId : c.stretchSourceMediaId;
            const auto* m = p.findMedia(root);
            if (!m || m->origin == "freeze" || m->origin == "bounce") continue;
            auto& u = use(root, t->name);
            u.secondsUsed += p.tickToSeconds(static_cast<double>(c.startTick + c.lengthTick)) -
                             p.tickToSeconds(static_cast<double>(c.startTick));
            if (u.firstTick < 0 || c.startTick < u.firstTick) u.firstTick = c.startTick;
        }
        if (const auto* s = samplerOf(p, t->id)) {
            const std::string id = s->params.getString("mediaId", "");
            if (const auto* m = p.findMedia(id)) {
                auto& u = use(id, t->name);
                if (!u.inSampler) u.secondsUsed += static_cast<double>(m->frames) / std::max(1.0, m->sampleRate);
                u.inSampler = true;
            }
        }
    }
    // Things you can hear first come first; sampler-only sources go last.
    std::stable_sort(out.begin(), out.end(), [](const SampleUse& a, const SampleUse& b) {
        const int64_t x = a.firstTick < 0 ? std::numeric_limits<int64_t>::max() : a.firstTick, y = b.firstTick < 0 ? std::numeric_limits<int64_t>::max() : b.firstTick;
        return x < y;
    });
    return out;
}

int unclearedCount(const Project& p) {
    int n = 0;
    for (const auto& u : clearanceReport(p)) n += needsClearance(u.status) ? 1 : 0;
    return n;
}

namespace {
std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char ch : s) out += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    return out + "\"";
}

std::string timeText(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", total / 60, total % 60);
    return buf;
}
}  // namespace

std::string clearanceSheetCsv(const Project& p) {
    std::ostringstream o;
    o << "Song," << csvField(p.name) << "\n";
    o << "Sample,Original title,Original artist,Master owner (label),Publisher,Taken from (time in original),"
         "Used on tracks,First heard at (song time),Length used,Status,Licence / agreement,Notes,Imported file\n";
    for (const auto& u : clearanceReport(p)) {
        const auto* c = findCredit(p, u.mediaId);
        std::string tracks;
        for (const auto& t : u.trackNames) tracks += (tracks.empty() ? "" : "; ") + t;
        o << csvField(u.name) << ',' << csvField(c ? c->title : "") << ',' << csvField(c ? c->artist : "") << ','
          << csvField(c ? c->masterOwner : "") << ',' << csvField(c ? c->publisher : "") << ','
          << csvField(c ? c->sourceTime : "") << ',' << csvField(tracks) << ','
          << (u.firstTick < 0 ? std::string("sampler") : timeText(p.tickToSeconds(static_cast<double>(u.firstTick)))) << ','
          << timeText(u.secondsUsed) << ',' << csvField(clearanceStatusName(u.status)) << ','
          << csvField(c ? c->license : "") << ',' << csvField(c ? c->notes : "") << ','
          << csvField(c ? c->fileName : "") << "\n";
    }
    return o.str();
}

}  // namespace ynot
