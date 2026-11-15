// ftpflow -- stage 4 trial compression + stage 5 bandwidth cost model.
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// Stage 4 engine order:
//   1. zstd level 1 (streamed) when built with FTPFLOW_HAVE_ZSTD.
//   2. ntdll RtlCompressBuffer with XPRESS_HUFF -- the same kernel-side
//      codec NTFS compression uses, zero external dependency, present on
//      every Windows 8+ machine. Loaded dynamically from the always-mapped
//      ntdll.dll; if the call is unavailable we degrade gracefully.
//
// The trial is bounded: at most kTrialMaxBytes of the sample are compressed,
// which both estimates the ratio and measures the local throughput that
// stage 5 needs.
#include "ftpflow/compress_decide.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if FTPFLOW_HAVE_ZSTD
#include <zstd.h>
#endif

namespace ftpflow {
namespace {

constexpr std::size_t kTrialMaxBytes = 256 * 1024;

using NTSTATUS = LONG;

using RtlGetCompressionWorkSpaceSizeFn =
    NTSTATUS(WINAPI*)(USHORT, PULONG, PULONG);
using RtlCompressBufferFn =
    NTSTATUS(WINAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);

constexpr USHORT kFmtXpressHuff = 0x0004;  // COMPRESSION_FORMAT_XPRESS_HUFF
constexpr USHORT kFmtXpress     = 0x0003;  // COMPRESSION_FORMAT_XPRESS
constexpr USHORT kFmtLznt1      = 0x0002;  // COMPRESSION_FORMAT_LZNT1
constexpr NTSTATUS kStatusSuccess = 0;
constexpr NTSTATUS kStatusBufferTooSmall = 0xC000002AL;

// Resolves the ntdll entry points once. ntdll.dll is already mapped in
// every user process, so GetModuleHandleW cannot fail in practice.
struct RtlApi {
    RtlGetCompressionWorkSpaceSizeFn getWorkspaceSize = nullptr;
    RtlCompressBufferFn compressBuffer = nullptr;
    USHORT preferredFormat = 0;   // 0 = nothing worked
    ULONG workspaceBytes = 0;
    bool formatProbed = false;

    static const RtlApi& instance() {
        static RtlApi api;
        if (!api.formatProbed) api.probe();
        return api;
    }

private:
    void probe() {
        formatProbed = true;
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return;
        getWorkspaceSize = reinterpret_cast<RtlGetCompressionWorkSpaceSizeFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetCompressionWorkSpaceSize")));
        compressBuffer = reinterpret_cast<RtlCompressBufferFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlCompressBuffer")));
        if (!getWorkspaceSize || !compressBuffer) return;

        for (USHORT fmt : {kFmtXpressHuff, kFmtXpress, kFmtLznt1}) {
            ULONG buf = 0, frag = 0;
            if (getWorkspaceSize(fmt, &buf, &frag) == kStatusSuccess) {
                preferredFormat = fmt;
                workspaceBytes = buf;
                return;
            }
        }
    }
};

double secondsSince(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

TrialResult trialViaRtl(const std::uint8_t* data, std::size_t len) {
    TrialResult r;
    const RtlApi& api = RtlApi::instance();
    if (api.preferredFormat == 0) return r;  // engine unavailable

    len = std::min(len, kTrialMaxBytes);
    // Buffer for output plus slack; on incompressible input RtlCompressBuffer
    // returns STATUS_BUFFER_TOO_SMALL, which we read as "no gain".
    const std::size_t outCap = len + len / 8 + 4096;
    std::vector<std::uint8_t> out(outCap);
    std::vector<std::uint8_t> workspace(api.workspaceBytes);

    const auto t0 = std::chrono::steady_clock::now();
    ULONG finalSize = 0;
    const NTSTATUS st = api.compressBuffer(
        api.preferredFormat,
        const_cast<PUCHAR>(data), static_cast<ULONG>(len),
        out.data(), static_cast<ULONG>(outCap),
        4096 /*UncompressedChunkSize*/, &finalSize, workspace.data());
    const double elapsed = secondsSince(t0);

    r.ok = true;
    r.engineIsZstd = false;
    r.sampleBytes = len;
    if (st == kStatusSuccess) {
        r.compressedBytes = finalSize < len ? finalSize : len;
        r.ratio = static_cast<double>(r.compressedBytes) /
                  static_cast<double>(len);
    } else {
        // STATUS_BUFFER_TOO_SMALL (or any other failure) on incompressible
        // input: compression would not shrink it.
        r.compressedBytes = len;
        r.ratio = 1.0;
    }
    if (elapsed > 1e-9) {
        r.measuredCompressBps = static_cast<double>(len) / elapsed;
    }
    return r;
}

std::string fmtBytes(double bytes) {
    char buf[40];
    if (bytes < 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.0f KiB", bytes / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MiB", bytes / (1024.0 * 1024.0));
    }
    return buf;
}

std::string fmtPct(double frac) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f%%", frac * 100.0);
    return buf;
}

}  // namespace

#if FTPFLOW_HAVE_ZSTD
// zstd level 1 over the bounded sample. One-shot ZSTD_compress with an
// exactly-sized output buffer; zstd returns an error (not partial output)
// when the input is incompressible, which we read as "no gain".
TrialResult trialViaZstd(const std::uint8_t* data, std::size_t len) {
    TrialResult r;
    len = std::min(len, kTrialMaxBytes);

    const std::size_t bound = ZSTD_compressBound(len);
    std::vector<std::uint8_t> out(bound);

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t produced = ZSTD_compress(
        out.data(), out.size(), data, len, /*level=*/1);
    const double elapsed = secondsSince(t0);

    r.ok = true;
    r.engineIsZstd = true;
    r.sampleBytes = len;
    if (!ZSTD_isError(produced) && produced < len) {
        r.compressedBytes = produced;
        r.ratio = static_cast<double>(produced) /
                  static_cast<double>(len);
    } else {
        // ZSTD_error_dstSize_tooSmall / srcSize_wrong / incompressible:
        // treating the sample as unshrinkable keeps the funnel safe.
        r.compressedBytes = len;
        r.ratio = 1.0;
    }
    if (elapsed > 1e-9) {
        r.measuredCompressBps = static_cast<double>(len) / elapsed;
    }
    return r;
}
#endif

TrialResult trialCompress(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) return {};
#if FTPFLOW_HAVE_ZSTD
    return trialViaZstd(data, len);
#else
    return trialViaRtl(data, len);
#endif
}

Decision decideByCostModel(std::uint64_t fileSizeBytes, double ratio,
                           double compressBps, const CostModel& cm) {
    Decision d;
    d.stage = 5;
    if (fileSizeBytes == 0) {
        d.verdict = Verdict::Store;
        d.reason = "empty file";
        return d;
    }
    if (ratio >= 0.98) {
        d.verdict = Verdict::Store;
        d.reason = "trial ratio ~1.0: no gain";
        return d;
    }
    if (ratio < 0.0) ratio = 0.0;

    const double S = static_cast<double>(fileSizeBytes);
    const double saved = (1.0 - ratio) * S;
    const double gain = saved / S;

    if (saved < static_cast<double>(cm.minSavingBytes)) {
        d.verdict = Verdict::Store;
        d.reason = "saving " + fmtBytes(saved) + " below " +
                   fmtBytes(static_cast<double>(cm.minSavingBytes)) +
                   " (small files take the aggregation path)";
        return d;
    }
    if (gain * 100.0 < cm.minGainPercent) {
        d.verdict = Verdict::Store;
        d.reason = "gain " + fmtPct(gain) + " below threshold " +
                   fmtPct(cm.minGainPercent / 100.0);
        return d;
    }

    const double B = cm.bandwidthBps > 0 ? cm.bandwidthBps : 20.0 * 1024 * 1024;
    const double C = compressBps > 0 ? compressBps : 250.0 * 1024 * 1024;
    const double D = cm.decompressBps > 0 ? cm.decompressBps : 4.0 * C;

    const double timeSaved = saved / B;
    const double timeCost = S / C + ratio * S / D;

    if (timeSaved > cm.safetyMargin * timeCost) {
        d.verdict = Verdict::Compress;
        d.reason = "save " + fmtBytes(saved) + " (" + fmtPct(gain) +
                   "), " + fmtPct(timeSaved / std::max(timeCost, 1e-9)) +
                   " return on CPU";
    } else {
        d.verdict = Verdict::Store;
        d.reason = "link too fast: saving " + fmtBytes(saved) +
                   " not worth CPU (" + fmtPct(timeSaved / std::max(timeCost, 1e-9)) +
                   " return)";
    }
    return d;
}

Decision decideFullFunnel(const FileProbe& probe, const CostModel& cm) {
    Decision d = decideCompressibility(probe);  // stages 1-3

    // Store / undecidable defaults stand; only Compress candidates go to
    // the expense of a trial + cost gate.
    if (d.verdict != Verdict::Compress) return d;

    // Stage 4: trial on the sample. No sample at all (unopenable file or
    // pure extension-driven call) -> keep the stage 1-3 verdict, but still
    // run the cost math with a conservative text-ish ratio of 0.5.
    double ratio = 0.5;
    double C = cm.compressBps;
    if (probe.sample.size() >= 1024 || probe.head.size() >= 1024) {
        const std::vector<std::uint8_t>& s =
            probe.sample.size() >= 1024 ? probe.sample : probe.head;
        TrialResult t = trialCompress(s.data(), s.size());
        if (t.ok) {
            ratio = t.ratio;
            // Explicit model throughput wins over the measurement; the
            // measurement only fills in when the caller left it at 0.
            if (cm.compressBps <= 0 && t.measuredCompressBps > 0) {
                C = t.measuredCompressBps;
            }
        }
    }
    if (probe.fileSize == 0 && probe.sample.empty()) {
        // No size info: keep the prior verdict (UI probed metadata only).
        return d;
    }
    return decideByCostModel(probe.fileSize, ratio, C, cm);
}

}  // namespace ftpflow
