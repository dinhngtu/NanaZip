// CompressCall.h

#ifndef ZIP7_INC_COMPRESS_CALL_H
#define ZIP7_INC_COMPRESS_CALL_H

#include "../../../Common/MyString.h"

// **************** 7-Zip ZS Modification Start ****************
// Removed in 7-Zip ZS.
// UString GetQuotedString(const UString &s);
// **************** 7-Zip ZS Modification End ****************

HRESULT CompressFiles(
    const UString &arcPathPrefix,
    const UString &arcName,
    const UString &arcType,
    bool addExtension,
    const UStringVector &names,
    bool email, bool showDialog, bool waitFinish);

// **************** NanaZip Modification Start ****************
// void ExtractArchives(const UStringVector &arcPaths, const UString &outFolder, bool showDialog, bool elimDup, UInt32 writeZone);
void ExtractArchives(const UStringVector &arcPaths, const UString &outFolder, bool showDialog, bool elimDup, UInt32 writeZone, bool smartExtract = false, bool openFolder = false);
// **************** NanaZip Modification End ****************
void TestArchives(const UStringVector &arcPaths, bool hashMode = false);

void CalcChecksum(const UStringVector &paths,
    const UString &methodName,
    const UString &arcPathPrefix,
    const UString &arcFileName);

void Benchmark(bool totalMode);

#endif
