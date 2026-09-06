#include "ShapefileProjection.h"
#include "NativeGdalLibrary.h"

#include <QLibrary>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>

namespace {
using namespace Shapefile;
// Exact C declarations checked against GDAL v3.8.4 ogr_srs_api.h/cpl_error.h
// and PROJ 9.4.0 src/proj.h. New OSR APIs are not uniformly CPL_STDCALL.
#if defined(_MSC_VER) && !defined(CPL_DISABLE_STDCALL)
#define APM_SHP_STDCALL __stdcall
#else
#define APM_SHP_STDCALL
#endif
struct ProjInfo {
    int major, minor, patch;
    const char *release, *version, *searchpath;
    const char *const *paths;
    size_t pathCount;
};
struct Api {
    std::unique_ptr<QLibrary> gdal, proj;
    void *(APM_SHP_STDCALL *newSrs)(const char *) = nullptr;
    void (APM_SHP_STDCALL *destroySrs)(void *) = nullptr;
    int (*importWkt)(void *, char **) = nullptr;
    int (*morphEsri)(void *) = nullptr;
    int (*validate)(void *) = nullptr;
    const char *(*getName)(void *) = nullptr;
    void (*axis)(void *, int) = nullptr;
    void (APM_SHP_STDCALL *errorReset)() = nullptr;
    const char *(APM_SHP_STDCALL *lastError)() = nullptr;
    ProjInfo (*info)() = nullptr;
    void *(*contextCreate)() = nullptr;
    void *(*contextDestroy)(void *) = nullptr;
    int (*network)(void *, int) = nullptr;
    int (*networkEnabled)(void *) = nullptr;
    void (*gridCache)(void *, int) = nullptr;
    void (*log)(void *, void *, void (*)(void *, int, const char *)) = nullptr;
    void *(*create)(void *, const char *) = nullptr;
    void *(*destroy)(void *) = nullptr;
    void *(*createTransform)(void *, const void *, const void *, void *, const char *const *) = nullptr;
    void *(*normalize)(void *, const void *) = nullptr;
    int (*type)(const void *) = nullptr;
    void *(*identify)(void *, const void *, const char *, const char *const *, int **) = nullptr;
    int (*listCount)(const void *) = nullptr;
    void *(*listGet)(void *, const void *, int) = nullptr;
    void (*listDestroy)(void *) = nullptr;
    void (*confidenceDestroy)(int *) = nullptr;
    const char *(*idAuthority)(const void *, int) = nullptr;
    const char *(*idCode)(const void *, int) = nullptr;
    int (*equivalent)(void *, const void *, const void *, int) = nullptr;
    void *(*coordinateSystem)(void *, const void *) = nullptr;
    int (*axisCount)(void *, const void *) = nullptr;
    int (*axisInfo)(void *, const void *, int, const char **, const char **,
                    const char **, double *, const char **, const char **, const char **) = nullptr;
    size_t (*transform)(void *, int, double *, size_t, size_t, double *, size_t, size_t,
                        double *, size_t, size_t, double *, size_t, size_t) = nullptr;
    int (*error)(const void *) = nullptr;
    int (*reset)(const void *) = nullptr;
    int (*contextError)(void *) = nullptr;
    const char *(*errorString)(void *, int) = nullptr;

    template<typename T> static bool symbol(QLibrary &library, T *out, const char *name) {
        *out = reinterpret_cast<T>(library.resolve(name)); return *out != nullptr;
    }
    bool load(QString *errorText) {
        for (const QString &candidate : nativeGdalLibraryCandidates()) {
            auto lib = std::make_unique<QLibrary>(candidate);
            lib->setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
            if (!lib->load()) continue;
            if (symbol(*lib, &newSrs, "OSRNewSpatialReference")
                && symbol(*lib, &destroySrs, "OSRDestroySpatialReference")
                && symbol(*lib, &importWkt, "OSRImportFromWkt")
                && symbol(*lib, &morphEsri, "OSRMorphFromESRI")
                && symbol(*lib, &validate, "OSRValidate")
                && symbol(*lib, &getName, "OSRGetName")
                && symbol(*lib, &axis, "OSRSetAxisMappingStrategy")
                && symbol(*lib, &errorReset, "CPLErrorReset")
                && symbol(*lib, &lastError, "CPLGetLastErrorMsg")) {
                gdal = std::move(lib); break;
            }
        }
        if (!gdal) { *errorText = QStringLiteral("Native GDAL runtime with OSR WKT support is unavailable. Install GDAL or set MISSIONPLANNER_GDAL_LIBRARY."); return false; }
        for (const QString &candidate : nativeProjLibraryCandidates()) {
            auto lib = std::make_unique<QLibrary>(candidate);
            lib->setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
            if (!lib->load() || !symbol(*lib, &info, "proj_info")) continue;
            const auto version = info();
            if (version.major < 9 || (version.major == 9 && version.minor < 2)) continue;
            if (symbol(*lib, &contextCreate, "proj_context_create")
                && symbol(*lib, &contextDestroy, "proj_context_destroy")
                && symbol(*lib, &network, "proj_context_set_enable_network")
                && symbol(*lib, &networkEnabled, "proj_context_is_network_enabled")
                && symbol(*lib, &gridCache, "proj_grid_cache_set_enable")
                && symbol(*lib, &log, "proj_log_func")
                && symbol(*lib, &create, "proj_create")
                && symbol(*lib, &destroy, "proj_destroy")
                && symbol(*lib, &createTransform, "proj_create_crs_to_crs_from_pj")
                && symbol(*lib, &normalize, "proj_normalize_for_visualization")
                && symbol(*lib, &type, "proj_get_type")
                && symbol(*lib, &identify, "proj_identify")
                && symbol(*lib, &listCount, "proj_list_get_count")
                && symbol(*lib, &listGet, "proj_list_get")
                && symbol(*lib, &listDestroy, "proj_list_destroy")
                && symbol(*lib, &confidenceDestroy, "proj_int_list_destroy")
                && symbol(*lib, &idAuthority, "proj_get_id_auth_name")
                && symbol(*lib, &idCode, "proj_get_id_code")
                && symbol(*lib, &equivalent, "proj_is_equivalent_to_with_ctx")
                && symbol(*lib, &coordinateSystem, "proj_crs_get_coordinate_system")
                && symbol(*lib, &axisCount, "proj_cs_get_axis_count")
                && symbol(*lib, &axisInfo, "proj_cs_get_axis_info")
                && symbol(*lib, &transform, "proj_trans_generic")
                && symbol(*lib, &error, "proj_errno")
                && symbol(*lib, &reset, "proj_errno_reset")
                && symbol(*lib, &contextError, "proj_context_errno")
                && symbol(*lib, &errorString, "proj_context_errno_string")) {
                proj = std::move(lib); return true;
            }
        }
        *errorText = QStringLiteral("Native PROJ 9.2 or newer is unavailable. It is required for strict ONLY_BEST offline reprojection; install its runtime, proj.db and needed grids, or set MISSIONPLANNER_PROJ_LIBRARY.");
        return false;
    }
};

struct Conversion {
    Api api;
    void *srs = nullptr, *context = nullptr, *source = nullptr, *target = nullptr, *operation = nullptr;
    QString detail, identifiedAuthority;
    ~Conversion() {
        if (operation) api.destroy(operation);
        if (source) api.destroy(source);
        if (target) api.destroy(target);
        if (context) api.contextDestroy(context);
        if (srs) api.destroySrs(srs);
    }
    QString failure(int code = 0) const {
        if (!detail.isEmpty()) return detail;
        if (!code && context) code = api.contextError(context);
        const char *message = context ? api.errorString(context, code) : api.lastError();
        return message && *message ? QString::fromUtf8(message).left(2048) : QStringLiteral("No usable coordinate operation or required local grid is available.");
    }
    bool sameCoordinateDomain(const void *candidate) {
        void *originalCs = api.coordinateSystem(context, source);
        void *candidateCs = api.coordinateSystem(context, candidate);
        bool same = originalCs && candidateCs;
        const int count = same ? api.axisCount(context, originalCs) : 0;
        same = same && count >= 2 && count <= 3 && count == api.axisCount(context, candidateCs);
        for (int i = 0; same && i < count; ++i) {
            const char *originalDirection = nullptr, *candidateDirection = nullptr;
            double originalUnit = 0, candidateUnit = 0;
            same = api.axisInfo(context, originalCs, i, nullptr, nullptr, &originalDirection,
                &originalUnit, nullptr, nullptr, nullptr)
                && api.axisInfo(context, candidateCs, i, nullptr, nullptr, &candidateDirection,
                    &candidateUnit, nullptr, nullptr, nullptr)
                && originalDirection && candidateDirection
                && QByteArray(originalDirection) == QByteArray(candidateDirection)
                && std::isfinite(originalUnit) && std::isfinite(candidateUnit)
                && originalUnit > 0 && candidateUnit > 0
                && std::abs(originalUnit - candidateUnit) <= 8 * std::numeric_limits<double>::epsilon()
                    * std::max(originalUnit, candidateUnit);
        }
        if (originalCs) api.destroy(originalCs);
        if (candidateCs) api.destroy(candidateCs);
        return same;
    }
    void identifyExactSource(const QByteArray &wkt, const void *original) {
        // Never replace an explicitly bound, grid-constrained, dynamic or
        // epoch-bearing CRS, even if PROJ collapses a no-op binding on import.
        // Conservatively skipping a token occurring in a quoted name is safe.
        const QByteArray upper = wkt.toUpper();
        for (const char *token : {"TOWGS84", "BOUNDCRS", "EXTENSION", "PARAMETERFILE",
                                 "GEOIDMODEL", "COORDINATEMETADATA", "EPOCH", "DYNAMIC"})
            if (upper.contains(token)) return;
        const int sourceType = api.type(original);
        // PJ_TYPE_GEOGRAPHIC_2D_CRS=12, GEOGRAPHIC_3D_CRS=13, PROJECTED_CRS=15.
        // In particular do not identify a BoundCRS through its base CRS.
        if (sourceType != 12 && sourceType != 13 && sourceType != 15) return;
        int *confidences = nullptr;
        void *matches = api.identify(context, original, "EPSG", nullptr, &confidences);
        int exactIndex = -1, exactCount = 0;
        const int count = matches ? api.listCount(matches) : 0;
        if (confidences && count > 0 && count <= 256) {
            for (int i = 0; i < count; ++i) {
                if (confidences[i] == 100) { exactIndex = i; ++exactCount; }
            }
        }
        void *candidate = exactCount == 1 ? api.listGet(context, matches, exactIndex) : nullptr;
        if (matches) api.listDestroy(matches);
        if (confidences) api.confidenceDestroy(confidences);
        if (!candidate) return;
        const char *authority = api.idAuthority(candidate, 0);
        const char *code = api.idCode(candidate, 0);
        const QString identifier = authority && code && QByteArray(authority) == "EPSG"
            ? QStringLiteral("EPSG:") + QString::fromLatin1(code) : QString();
        void *normalized = api.type(candidate) == sourceType && !identifier.isEmpty()
            ? api.normalize(context, candidate) : nullptr;
        api.destroy(candidate);
        // PJ_COMP_EQUIVALENT=1 checks the complete CRS, not just its datum.
        // Check after GIS axis normalization, then independently require the
        // same axis directions and units: grads/feet must not become degrees/metres.
        if (normalized && api.equivalent(context, source, normalized, 1)
            && sameCoordinateDomain(normalized)) {
            api.destroy(source); source = normalized; identifiedAuthority = identifier;
        } else if (normalized) api.destroy(normalized);
    }
    bool initialize(QByteArray wkt, QString *name, QString *errorText) {
        if (!api.load(errorText)) return false;
        srs = api.newSrs(nullptr);
        if (!srs) { *errorText = QStringLiteral("Cannot allocate the source spatial reference."); return false; }
        api.errorReset();
        char *cursor = wkt.data();
        if (api.importWkt(srs, &cursor) != 0 || !QByteArray(cursor).trimmed().isEmpty()
            || api.morphEsri(srs) != 0 || api.validate(srs) != 0) {
            *errorText = QStringLiteral("Invalid or unsupported PRJ WKT: ") + failure(); return false;
        }
        api.axis(srs, 0); // OAMS_TRADITIONAL_GIS_ORDER; PROJ CRS axes are normalized below.
        const char *sourceName = api.getName(srs);
        *name = sourceName ? QString::fromUtf8(sourceName) : QStringLiteral("Unnamed source CRS");
        context = api.contextCreate();
        if (!context) { *errorText = QStringLiteral("Cannot allocate a private PROJ context."); return false; }
        api.network(context, 0); api.gridCache(context, 0);
        api.log(context, &detail, [](void *data, int, const char *message) {
            *static_cast<QString *>(data) = QString::fromUtf8(message ? message : "").left(2048);
        });
        if (api.networkEnabled(context)) { *errorText = QStringLiteral("Cannot disable networking on the private PROJ context."); return false; }
        // Keep the original, GDAL-validated WKT. Serializing WKT1 TOWGS84
        // through WKT2 BOUNDCRS can change PROJ's transformation-source CRS
        // for a non-Greenwich prime meridian and lose its strict operation.
        // PROJ's native WKT parser supports ESRI aliases directly; no hand
        // rewriting, datum guessing or alternate ballpark operation is used.
        // A separately proven exact authority identification may repair an ESRI
        // alias without changing the normalized coordinate domain below.
        void *rawSource = api.create(context, wkt.constData());
        void *rawTarget = api.create(context, "EPSG:4326");
        if (rawSource) {
            source = api.normalize(context, rawSource);
            if (source) identifyExactSource(wkt, rawSource);
            api.destroy(rawSource);
        }
        if (rawTarget) { target = api.normalize(context, rawTarget); api.destroy(rawTarget); }
        if (!source || !target) { *errorText = QStringLiteral("Cannot initialize source/WGS84 CRS; check local proj.db: ") + failure(); return false; }
        detail.clear(); // Optional identification diagnostics must not mask operation failures.
        const char *options[] = {"ALLOW_BALLPARK=NO", "ONLY_BEST=YES", nullptr};
        // Normalize CRS axes BEFORE constructing the operation. This also
        // leaves all strict operation-selection options on the original PJ.
        operation = api.createTransform(context, source, target, nullptr, options);
        if (!operation) { *errorText = QStringLiteral("No strict offline transformation to WGS84; install the required local grids: ") + failure(); return false; }
        // No process-global OSRSetPROJEnableNetwork, environment changes,
        // default context, global error handler or network cache is modified.
        return true;
    }
};
bool wgs84(const Coordinate &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= -180 && p.x <= 180 && p.y >= -90 && p.y <= 90;
}
}

Shapefile::ProjectionResult ShapefileProjection::transform(const QString &requestedWkt,
    const QVector<Feature> &requestedFeatures, const Cancel &requestedCancel, const Progress &requestedProgress) {
    const QString wkt = requestedWkt;
    const QVector<Feature> input = requestedFeatures;
    const Cancel cancel = requestedCancel;
    const Progress progress = requestedProgress;
    ProjectionResult result;
    auto fail = [&](const QString &error) { result.features.clear(); result.error = error; return result; };
    auto interrupted = [&] {
        if (cancel && cancel()) { result.cancelled = true; return true; }
        return false;
    };
    if (wkt.size() > MaximumPrjBytes || wkt.toUtf8().size() > MaximumPrjBytes || wkt.contains(QChar('\0')))
        return fail(QStringLiteral("PRJ text exceeds 64 KiB or contains an embedded NUL."));
    if (input.size() > MaximumFeatures) return fail(QStringLiteral("Shapefile exceeds 20000 features."));
    qint64 total = 0;
    for (const auto &feature : input) {
        if (interrupted()) return fail(QStringLiteral("Shapefile projection cancelled."));
        total += feature.points.size();
        if (total > MaximumPoints) return fail(QStringLiteral("Shapefile exceeds two million points."));
    }
    if (interrupted()) return fail(QStringLiteral("Shapefile projection cancelled."));
    Conversion conversion;
    QString text = wkt.trimmed();
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    text = text.trimmed();
    const bool projected = !text.isEmpty();
    if (projected && !conversion.initialize(text.toUtf8(), &result.projectionName, &result.error)) return result;
    if (!conversion.identifiedAuthority.isEmpty())
        result.warnings.append(QStringLiteral("Source CRS identified exactly as %1 (100% confidence; coordinate axes and units preserved).")
            .arg(conversion.identifiedAuthority));
    if (projected) result.warnings.append(QStringLiteral("Offline GDAL/PROJ reprojection uses local CRS/grid data with ballpark transformations disabled. Numerical datum accuracy depends on the source CRS, available grids and coordinate epoch; this is not proof of survey accuracy."));
    else result.warnings.append(QStringLiteral("No PRJ: coordinates are treated as WGS84 longitude/latitude, matching Mission Planner. Their actual datum is not verified."));
    qint64 completed = 0;
    if (progress) progress(0, total, QStringLiteral("Reprojecting coordinates"));
    for (int featureIndex = 0; featureIndex < input.size(); ++featureIndex) {
        const auto &feature = input.at(featureIndex);
        Feature output; output.points.reserve(feature.points.size());
        for (int first = 0; first < feature.points.size(); first += 2048) {
            if (interrupted()) return fail(QStringLiteral("Shapefile projection cancelled."));
            const int last = std::min(first + 2048, feature.points.size());
            for (int n = first; n < last; ++n) {
                Coordinate p = feature.points.at(n);
                if (!std::isfinite(p.z)) p.z = 0; // MP10's missing/nonfinite altitude convention.
                if (projected && std::isfinite(p.x) && std::isfinite(p.y)) {
                    conversion.detail.clear(); conversion.api.reset(conversion.operation);
                    // Pointer-array ABI avoids passing PJ_COORD unions by value.
                    // Per-point errno preserves grid/no-operation errors instead
                    // of masking them behind a later bad-coordinate error.
                    conversion.api.transform(conversion.operation, 1, &p.x, sizeof(double), 1,
                        &p.y, sizeof(double), 1, &p.z, sizeof(double), 1, nullptr, 0, 0);
                    const int code = conversion.api.error(conversion.operation);
                    if (code && code != 2049 && code != 2050 && code != 2054)
                        return fail(QStringLiteral("Offline coordinate transformation failed at feature %1, point %2 (1-based; possibly a missing local grid or unavailable datum operation): ")
                            .arg(featureIndex + 1).arg(n + 1) + conversion.failure(code));
                    if (code) { ++result.discardedPoints; continue; }
                }
                if (wgs84(p)) output.points.append(p);
                else ++result.discardedPoints;
            }
            completed += last - first;
            if (progress) progress(completed, total, QStringLiteral("Reprojecting coordinates"));
        }
        result.features.append(std::move(output)); // Keep source feature boundaries, including now-empty features.
    }
    if (interrupted()) return fail(QStringLiteral("Shapefile projection cancelled."));
    if (result.discardedPoints) result.warnings.append(QStringLiteral("%1 invalid or out-of-domain coordinate(s) were omitted.").arg(result.discardedPoints));
    result.success = true;
    return result;
}
#undef APM_SHP_STDCALL
