// Sample clearance: keeps track of every piece of outside audio in a song (imported files,
// samples), who owns it, and whether it's cleared for release.
//
// Ynot's own recordings, generated ideas and Ynot sound-pack sounds don't need clearing.
// Everything imported starts as "Unknown" until the user marks it (their own, royalty-free,
// requested, cleared, denied). Before export, uncleared samples are listed, and a
// clearance sheet (CSV) can be sent to labels and publishers.
#pragma once

#include <string>
#include <vector>

#include "ynot/model.hpp"

namespace ynot {

const char* clearanceStatusId(ClearanceStatus s);    // "unknown", "original", ... (stored)
const char* clearanceStatusName(ClearanceStatus s);  // "Not cleared yet", "My own audio", ...
ClearanceStatus clearanceStatusFromId(const std::string& id);
// Needs attention before release: unknown, requested (not yet answered) or denied.
bool needsClearance(ClearanceStatus s);

// Status for media with no credit, from where it came from.
ClearanceStatus defaultClearance(const MediaRef& m);

struct SampleUse {
    std::string mediaId;  // the original (stretches and chops are traced back to it)
    std::string name;     // credit title, imported file name, or "Sample N"
    std::vector<std::string> trackNames;
    double secondsUsed = 0;   // total length of clips using it (a sampler counts its sample once)
    int64_t firstTick = -1;   // first place it's heard (-1 = only in a sampler)
    ClearanceStatus status = ClearanceStatus::Unknown;
    bool inSampler = false;
};

// Every outside or recorded source the song uses, in order of first use.
std::vector<SampleUse> clearanceReport(const Project& p);
int unclearedCount(const Project& p);

const SampleCredit* findCredit(const Project& p, const std::string& mediaId);
// Adds or replaces the credit for c.mediaId.
void setCredit(Project& p, const SampleCredit& c);
// Imports remember the file they came from (shown in the clearance list).
void noteImportedFile(Project& p, const std::string& mediaId, const std::string& fileName,
                      ClearanceStatus status = ClearanceStatus::Unknown);
// New media made from other media (a clip sent to the sampler) inherits its credit.
void inheritCredit(Project& p, const std::string& newMediaId, const std::string& fromMediaId);

// Clearance sheet: one row per sample with its owners, status and where it's used.
std::string clearanceSheetCsv(const Project& p);

}  // namespace ynot
