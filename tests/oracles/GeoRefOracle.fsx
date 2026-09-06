// GeoRefOracle.fsx - drive the ACTUAL Mission Planner 10 GeoRefViewModel by reflection.
// Independent read-only oracle helper (Claude, c335). No algorithm is re-implemented here:
// matching, corrections, base adjustment, reports and EXIF writes all run inside the
// reference assemblies. Inputs are copied into a new output directory and never modified.
//
//   dotnet fsi --exec GeoRefOracle.fsx MP10_BIN NEW_OUTPUT_DIR LOG PHOTO_DIR [OPTIONS_JSON]
//
// OPTIONS_JSON (file path or inline JSON object), lowerCamel keys per the c336 contract, all optional:
//   mode                          "cam" | "trig" | "offset"   default "cam" (MP10 default UseCamMessages)
//   timeOffsetSeconds             double   TimeOffsetSeconds                       default 0
//   useGps2                       bool     UseGps2                                 default false
//   shutterLagMilliseconds        int      ShutterLagMilliseconds                  default 0
//   useAmslAltitude               bool     UseAmslAltitude                         default false
//   useGpsAltitude                bool     UseGpsAltitude                          default false
//   baseAltitudeAdjustmentMeters  double   BaseAltitudeAdjustmentMeters            default 0
// Oracle-only keys:
//   estimate          bool     run EstimateOffsetAsync first (the VM itself updates TimeOffsetSeconds) default false
//   stages            bool     dump private reader/matcher/correction results and the matches array    default true
//   geotag            bool     run the public GeoTagAsync workflow                                     default true
//   staleGeotagNames  [str]    file names to pre-create under photos/geotagged before GeoTag (stale-copy path)
//
// Output: NEW_OUTPUT_DIR/oracle.json with
//   matches[]  {name, sourcePath (original PHOTO_DIR file), copyPath, outputPath, timeUtc (ISO ms Z),
//               latitude, longitude, altitude, relAlt, gpsAlt, roll, pitch, yaw}  from the reference
//               DoWork* + ApplyCameraCorrections + ApplyBaseAltitudeAdjustment list (same list GeoTagAsync tags)
//   estimate   {offsetSeconds, status, outputLog}   when estimate=true
//   geotag     status, outputLog, Results grid, location.txt/kml text, geotagged files with SHA-256
//   stages     ListPhotos + EXIF times, ReadGps/Cam/TrigMsgInLog dictionaries, matched/corrected/adjusted dumps
// plus settings, environment, reference assembly hashes; NEW_OUTPUT_DIR/input/<log>; NEW_OUTPUT_DIR/photos/
// (copied photos, plus photos/geotagged/ written by the reference code).
// Exit codes: 0 ok, 2 reference members missing (listed in oracle.json), 1 failure.
// Time zone: EXIF times are Kind Unspecified in MP10 and converted with the process zone;
// run this oracle and the Qt comparison under the same TZ environment variable.

open System
open System.Collections
open System.Collections.Generic
open System.Diagnostics
open System.Globalization
open System.IO
open System.Reflection
open System.Runtime.Loader
open System.Security.Cryptography
open System.Text.Encodings.Web
open System.Text.Json
open System.Text.Json.Serialization
open System.Threading.Tasks

let started = DateTime.UtcNow
let clock = Stopwatch.StartNew()
let args = fsi.CommandLineArgs
if args.Length < 5 then
    failwith "Expected MP10 bin directory, new output directory, LOG file, PHOTO directory and optional options JSON"

let bin = Path.GetFullPath(args.[1])
let outputRoot = Path.GetFullPath(args.[2])
let logSource = Path.GetFullPath(args.[3])
let photoSource = Path.GetFullPath(args.[4])
let optionsArg = if args.Length > 5 then args.[5] else "{}"

if Directory.Exists(outputRoot) || File.Exists(outputRoot) then
    failwith "Oracle output directory must be new"
if not (File.Exists(logSource)) then failwith ("Log file not found: " + logSource)
if not (Directory.Exists(photoSource)) then failwith ("Photo directory not found: " + photoSource)
for name in [ "MissionPlanner.dll"; "MissionPlanner.Utilities.dll"; "ExifLibrary.dll"
              "MetadataExtractor.dll"; "Avalonia.Base.dll"; "CommunityToolkit.Mvvm.dll" ] do
    if not (File.Exists(Path.Combine(bin, name))) then
        failwith ("Reference bin is missing " + name)

// ---------------------------------------------------------------- helpers
let sha256Hex (path: string) =
    Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant()

let jsonOptions =
    let o = JsonSerializerOptions(WriteIndented = true)
    o.NumberHandling <- JsonNumberHandling.AllowNamedFloatingPointLiterals
    o.Encoder <- JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    o

let dict (pairs: (string * obj) list) =
    let d = Dictionary<string, obj>()
    for (k, v) in pairs do d.[k] <- v
    d

let describeDateTime (dt: DateTime) : obj =
    let epoch = DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)
    let utc = dt.ToUniversalTime()
    dict [ "iso", box (dt.ToString("o", CultureInfo.InvariantCulture))
           "kind", box (dt.Kind.ToString())
           "utc", box (utc.ToString("yyyy-MM-ddTHH:mm:ss.fffffffZ", CultureInfo.InvariantCulture))
           "utcMs", box (if dt = DateTime.MinValue then 0L else int64 (Math.Round((utc - epoch).TotalMilliseconds)))
           "isMinValue", box (dt = DateTime.MinValue) ]

let rec describeValue (v: obj) : obj =
    match v with
    | null -> null
    | :? DateTime as dt -> describeDateTime dt
    | :? string | :? bool | :? int | :? int64 | :? float | :? float32 | :? uint8 | :? int16 -> v
    | :? IDictionary as d ->
        let rows = List<obj>()
        for key in d.Keys do
            rows.Add(dict [ "key", describeValue key; "value", describeValue d.[key] ])
        box rows
    | :? IEnumerable as e ->
        let rows = List<obj>()
        for x in e do rows.Add(describeValue x)
        box rows
    | _ ->
        let t = v.GetType()
        let d = Dictionary<string, obj>()
        d.["$type"] <- t.Name
        for f in t.GetFields(BindingFlags.Public ||| BindingFlags.Instance) do
            d.[f.Name] <- describeValue (f.GetValue(v))
        for p in t.GetProperties(BindingFlags.Public ||| BindingFlags.Instance) do
            if p.CanRead && p.GetIndexParameters().Length = 0 then
                d.[p.Name] <- describeValue (p.GetValue(v))
        box d

let sortedDictionaryRows (v: obj) : obj =
    // Dictionary<long, VehicleLoc>: emit rows sorted by ms key so two runs compare stably.
    match v with
    | :? IDictionary as d ->
        let rows =
            Seq.cast<obj> d.Keys
            |> Seq.map (fun key -> Convert.ToInt64(key), describeValue d.[key])
            |> Seq.sortBy fst
            |> Seq.map (fun (k, value) -> box (dict [ "ms", box k; "value", value ]))
            |> List<obj>
        box rows
    | _ -> describeValue v

let fileRecord (path: string) : obj =
    dict [ "path", box path; "size", box (FileInfo(path).Length); "sha256", box (sha256Hex path) ]

let awaitTask (o: obj) =
    match o with
    | :? Task as t -> t.GetAwaiter().GetResult()
    | _ -> ()

// ---------------------------------------------------------------- options
let optionsText =
    if File.Exists(optionsArg) then File.ReadAllText(optionsArg) else optionsArg
let optionsDoc = JsonDocument.Parse(optionsText)
let root = optionsDoc.RootElement
let tryProp (name: string) =
    let mutable e = Unchecked.defaultof<JsonElement>
    if root.ValueKind = JsonValueKind.Object && root.TryGetProperty(name, &e) then Some e else None
let optString name def = match tryProp name with Some e when e.ValueKind = JsonValueKind.String -> e.GetString() | _ -> def
let optBool name def = match tryProp name with Some e when e.ValueKind = JsonValueKind.True -> true | Some e when e.ValueKind = JsonValueKind.False -> false | _ -> def
let optDouble name def = match tryProp name with Some e when e.ValueKind = JsonValueKind.Number -> e.GetDouble() | _ -> def
let optInt name def = match tryProp name with Some e when e.ValueKind = JsonValueKind.Number -> e.GetInt32() | _ -> def
let optStrings name =
    match tryProp name with
    | Some e when e.ValueKind = JsonValueKind.Array -> [ for x in e.EnumerateArray() do if x.ValueKind = JsonValueKind.String then yield x.GetString() ]
    | _ -> []

let mode = (optString "mode" "cam").ToLowerInvariant()
if mode <> "cam" && mode <> "trig" && mode <> "offset" then failwith "mode must be cam, trig or offset"
let offsetSeconds = optDouble "timeOffsetSeconds" 0.0
let useGps2 = optBool "useGps2" false
let shutterLagMs = optInt "shutterLagMilliseconds" 0
let useAmsl = optBool "useAmslAltitude" false
let useGpsAlt = optBool "useGpsAltitude" false
let baseAltitude = optDouble "baseAltitudeAdjustmentMeters" 0.0
for key in [ "offsetSeconds"; "shutterLagMs"; "baseAltitudeM" ] do
    if (tryProp key).IsSome then failwith ("Unknown option " + key + "; use the c336 lowerCamel contract keys")
let runEstimate = optBool "estimate" false
let runStages = optBool "stages" true
let runGeotag = optBool "geotag" true
let staleNames = optStrings "staleGeotagNames"

// ---------------------------------------------------------------- reference loading
AssemblyLoadContext.Default.add_Resolving(
    Func<AssemblyLoadContext, AssemblyName, Assembly>(fun ctx name ->
        let path = Path.Combine(bin, name.Name + ".dll")
        if File.Exists(path) then ctx.LoadFromAssemblyPath(path) else null))
CultureInfo.DefaultThreadCurrentCulture <- CultureInfo.InvariantCulture
CultureInfo.DefaultThreadCurrentUICulture <- CultureInfo.GetCultureInfo("en-US")
CultureInfo.CurrentCulture <- CultureInfo.InvariantCulture

Directory.CreateDirectory(outputRoot) |> ignore
let flags = BindingFlags.Public ||| BindingFlags.NonPublic ||| BindingFlags.Static ||| BindingFlags.Instance
let utilities = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "MissionPlanner.Utilities.dll"))
let settings = utilities.GetType("MissionPlanner.Utilities.Settings", true)
let cache = Path.Combine(outputRoot, "metadata-cache")
Directory.CreateDirectory(cache) |> ignore
settings.GetField("_GetRunningDirectory", flags).SetValue(null, bin + string Path.DirectorySeparatorChar)
settings.GetField("CustomDataDirectory", flags).SetValue(null, cache)
settings.GetField("CustomUserDataDirectory", flags).SetValue(null, Path.Combine(outputRoot, "isolated-settings"))

let mp = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "MissionPlanner.dll"))
let vmType = mp.GetType("MissionPlanner.ViewModels.GeoRefViewModel", true)
let georefType = utilities.GetType("MissionPlanner.GeoRef.GeoRefImageBase", true)

// Member inventory: report anything the reference build lacks instead of guessing.
let instancePrivate = [ "DoWorkCam"; "DoWorkTrig"; "DoWorkGpsOffset"; "ApplyCameraCorrections"
                        "ApplyBaseAltitudeAdjustment"; "WriteReports"; "ListPhotos"; "GetPhotoTime" ]
let staticPrivate = [ "ReadGpsMsgInLog"; "ReadCamMsgInLog"; "ReadTrigMsgInLog"; "LookForLocation"
                      "ToMillis"; "GetTimeFromGps"; "EstimateOffset"; "AdjustAltitude" ]
let publicInstance = [ "GeoTagAsync"; "EstimateOffsetAsync" ]
let properties = [ "LogPath"; "PhotoDir"; "TimeOffsetSeconds"; "UseCamMessages"; "UseTrigMessages"; "UseGps2"
                   "ShutterLagMilliseconds"; "UseAmslAltitude"; "UseGpsAltitude"; "BaseAltitudeAdjustmentMeters"
                   "Busy"; "Status"; "OutputLog"; "Results" ]
let findMethod (t: Type) (name: string) (bf: BindingFlags) =
    t.GetMethods(bf) |> Array.tryFind (fun m -> m.Name = name)
let missing = List<string>()
for n in instancePrivate do
    if (findMethod vmType n (BindingFlags.NonPublic ||| BindingFlags.Instance)).IsNone then missing.Add("GeoRefViewModel." + n + " (private instance)")
for n in staticPrivate do
    if (findMethod vmType n (BindingFlags.NonPublic ||| BindingFlags.Static)).IsNone then missing.Add("GeoRefViewModel." + n + " (private static)")
for n in publicInstance do
    if (findMethod vmType n (BindingFlags.Public ||| BindingFlags.Instance)).IsNone then missing.Add("GeoRefViewModel." + n + " (public)")
for n in properties do
    if isNull (vmType.GetProperty(n, BindingFlags.Public ||| BindingFlags.Instance)) then missing.Add("GeoRefViewModel." + n + " (property)")
if (findMethod georefType "WriteCoordinatesToImage" (BindingFlags.Public ||| BindingFlags.Instance)).IsNone then
    missing.Add("GeoRefImageBase.WriteCoordinatesToImage (public)")

let assemblyRecord (name: string) : obj =
    let p = Path.Combine(bin, name)
    dict [ "path", box p; "size", box (FileInfo(p).Length); "sha256", box (sha256Hex p)
           "fileVersion", box (FileVersionInfo.GetVersionInfo(p).FileVersion)
           "lastWriteUtc", box (File.GetLastWriteTimeUtc(p).ToString("o")) ]

let report = Dictionary<string, obj>()
report.["oracle"] <- dict [ "script", box (Path.GetFullPath(args.[0])); "scriptSha256", box (sha256Hex (Path.GetFullPath(args.[0])))
                            "version", box "c335-1"; "startedUtc", box (started.ToString("o")) ]
report.["reference"] <- dict [ "bin", box bin
                               "assemblies", box (dict [ for n in [ "MissionPlanner.dll"; "MissionPlanner.Utilities.dll"; "ExifLibrary.dll"; "MetadataExtractor.dll"; "Avalonia.Base.dll" ] -> n, assemblyRecord n ]) ]
report.["missingMembers"] <- box missing

let writeReport () =
    report.["durationMs"] <- box clock.ElapsedMilliseconds
    File.WriteAllText(Path.Combine(outputRoot, "oracle.json"), JsonSerializer.Serialize(report, jsonOptions))
    printfn "GEOREF_ORACLE:%s" (Path.Combine(outputRoot, "oracle.json"))

if missing.Count > 0 then
    report.["error"] <- box "Reference build lacks members listed in missingMembers"
    writeReport ()
    exit 2

// ---------------------------------------------------------------- inputs (private copies)
let inputDir = Path.Combine(outputRoot, "input")
Directory.CreateDirectory(inputDir) |> ignore
let logCopy = Path.Combine(inputDir, Path.GetFileName(logSource))
File.Copy(logSource, logCopy, false)
let photoDir = Path.Combine(outputRoot, "photos")
Directory.CreateDirectory(photoDir) |> ignore
let photoRecords = List<obj>()
for f in Directory.GetFiles(photoSource) |> Array.sort do
    let target = Path.Combine(photoDir, Path.GetFileName(f))
    File.Copy(f, target, false)
    photoRecords.Add(dict [ "name", box (Path.GetFileName(f)); "size", box (FileInfo(target).Length); "sha256", box (sha256Hex target) ])
report.["inputs"] <- dict [ "logSource", box logSource; "logCopy", fileRecord logCopy
                            "photoSource", box photoSource; "photoDir", box photoDir; "photos", box photoRecords ]

// ---------------------------------------------------------------- view model access
let makeVm () =
    let vm = Activator.CreateInstance(vmType)
    let set (name: string) (value: obj) = vmType.GetProperty(name).SetValue(vm, value)
    set "LogPath" (box logCopy)
    set "PhotoDir" (box photoDir)
    set "TimeOffsetSeconds" (box offsetSeconds)
    set "UseCamMessages" (box (mode = "cam"))
    set "UseTrigMessages" (box (mode = "trig"))
    set "UseGps2" (box useGps2)
    set "ShutterLagMilliseconds" (box shutterLagMs)
    set "UseAmslAltitude" (box useAmsl)
    set "UseGpsAltitude" (box useGpsAlt)
    set "BaseAltitudeAdjustmentMeters" (box baseAltitude)
    vm
let get (vm: obj) (name: string) = vmType.GetProperty(name).GetValue(vm)
let callInstance (vm: obj) (name: string) (a: obj[]) =
    (findMethod vmType name (BindingFlags.NonPublic ||| BindingFlags.Instance)).Value.Invoke(vm, a)
let callStatic (name: string) (a: obj[]) =
    (findMethod vmType name (BindingFlags.NonPublic ||| BindingFlags.Static)).Value.Invoke(null, a)
let callPublic (vm: obj) (name: string) =
    (findMethod vmType name (BindingFlags.Public ||| BindingFlags.Instance)).Value.Invoke(vm, [||])
let snapshotSettings (vm: obj) : obj =
    dict [ for n in properties do if n <> "Results" then yield n, describeValue (get vm n) ]

// Avalonia dispatcher: Append() posts to Dispatcher.UIThread when not on the UI thread.
// Without a platform Avalonia falls back to a null dispatcher whose thread check passes;
// record the actual answer and flush any queued jobs so OutputLog is complete.
let avaloniaBase = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "Avalonia.Base.dll"))
let dispatcherType = avaloniaBase.GetType("Avalonia.Threading.Dispatcher", false)
let dispatcher () = if isNull dispatcherType then null else dispatcherType.GetProperty("UIThread").GetValue(null)
let dispatcherInfo () : obj =
    try
        let d = dispatcher ()
        if isNull d then box "Dispatcher type not found" else
        let check = dispatcherType.GetMethod("CheckAccess").Invoke(d, [||])
        let flushed =
            match dispatcherType.GetMethods() |> Array.tryFind (fun m -> m.Name = "RunJobs") with
            | Some m ->
                (try m.Invoke(d, Array.init (m.GetParameters().Length) (fun _ -> null)) |> ignore; "RunJobs invoked"
                 with ex -> "RunJobs failed: " + ex.GetBaseException().Message)
            | None -> "RunJobs not found"
        dict [ "checkAccess", check; "flush", box flushed ]
    with ex -> box ("dispatcher probe failed: " + ex.GetBaseException().Message)

let environmentInfo () : obj =
    let tz = TimeZoneInfo.Local
    dict [ "is64Bit", box Environment.Is64BitProcess; "os", box (Environment.OSVersion.ToString())
           "runtime", box (Environment.Version.ToString()); "timeZoneId", box tz.Id
           "baseUtcOffset", box (tz.BaseUtcOffset.ToString()); "currentUtcOffset", box (tz.GetUtcOffset(DateTime.UtcNow).ToString())
           "tzEnv", box (Environment.GetEnvironmentVariable("TZ")); "culture", box CultureInfo.CurrentCulture.Name
           "dispatcher", dispatcherInfo () ]
report.["environment"] <- environmentInfo ()
report.["options"] <- dict [ "mode", box mode; "timeOffsetSeconds", box offsetSeconds; "useGps2", box useGps2
                             "shutterLagMilliseconds", box shutterLagMs; "useAmslAltitude", box useAmsl; "useGpsAltitude", box useGpsAlt
                             "baseAltitudeAdjustmentMeters", box baseAltitude; "estimate", box runEstimate; "stages", box runStages
                             "geotag", box runGeotag; "staleGeotagNames", box staleNames ]

let field (o: obj) (name: string) : obj =
    let f = o.GetType().GetField(name, BindingFlags.Public ||| BindingFlags.NonPublic ||| BindingFlags.Instance)
    if isNull f then null else f.GetValue(o)
let matchRow (p: obj) : obj =
    // Root probe shape: sourcePath, outputPath, timeUtc (ISO ms), latitude, longitude, altitude, roll, pitch, yaw.
    let copyPath = field p "Path" :?> string
    let name = Path.GetFileName(copyPath)
    let time = field p "Time" :?> DateTime
    dict [ "name", box name
           "sourcePath", box (Path.Combine(photoSource, name))
           "copyPath", box copyPath
           "outputPath", box (Path.Combine(photoDir, "geotagged", Path.GetFileNameWithoutExtension(copyPath) + "_geotag" + Path.GetExtension(copyPath)))
           "timeUtc", box (time.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture))
           "timeKind", box (time.Kind.ToString())
           "latitude", field p "Lat"; "longitude", field p "Lon"; "altitude", field p "AltAMSL"
           "relAlt", field p "RelAlt"; "gpsAlt", field p "GPSAlt"
           "roll", field p "Roll"; "pitch", field p "Pitch"; "yaw", field p "Yaw" ]

try
    // ------------------------------------------------------------ stage dumps (private methods, pure)
    if runStages then
        let vm = makeVm ()
        let sw = Stopwatch.StartNew()
        let gpsMsg = if useGps2 then "GPS2" else "GPS"
        let stages = Dictionary<string, obj>()
        stages.["settings"] <- snapshotSettings vm
        let photos = callInstance vm "ListPhotos" [||]
        let photoRows = List<obj>()
        for p in (photos :?> IEnumerable) do
            let path = p :?> string
            let t = callInstance vm "GetPhotoTime" [| box path |] :?> DateTime
            photoRows.Add(dict [ "path", box path; "exifTime", describeDateTime t ])
        stages.["listPhotos"] <- box photoRows
        stages.["readGpsMsgInLog"] <- sortedDictionaryRows (callStatic "ReadGpsMsgInLog" [| box logCopy; box gpsMsg |])
        stages.["readCamMsgInLog"] <- sortedDictionaryRows (callStatic "ReadCamMsgInLog" [| box logCopy |])
        stages.["readTrigMsgInLog"] <- sortedDictionaryRows (callStatic "ReadTrigMsgInLog" [| box logCopy |])
        // Same dispatch and guards as GeoTagAsync (orchestration only; every step is the reference method).
        let pics =
            match mode with
            | "cam" -> callInstance vm "DoWorkCam" [||]
            | "trig" -> callInstance vm "DoWorkTrig" [||]
            | _ -> callInstance vm "DoWorkGpsOffset" [||]
        let count = (pics :?> ICollection).Count
        stages.["matched"] <- describeValue pics
        if (mode = "cam" || mode = "trig") && count > 0 then
            callInstance vm "ApplyCameraCorrections" [| pics |] |> ignore
            stages.["corrected"] <- describeValue pics
        if count > 0 then
            callInstance vm "ApplyBaseAltitudeAdjustment" [| pics; box baseAltitude |] |> ignore
            stages.["adjusted"] <- describeValue pics
        stages.["outputLog"] <- get vm "OutputLog"
        stages.["durationMs"] <- box sw.ElapsedMilliseconds
        report.["stages"] <- box stages
        let matches = List<obj>()
        for p in (pics :?> IEnumerable) do matches.Add(matchRow p)
        report.["matches"] <- box matches

    // ------------------------------------------------------------ actual public workflows
    let vm = makeVm ()
    if runEstimate then
        let sw = Stopwatch.StartNew()
        awaitTask (callPublic vm "EstimateOffsetAsync")
        dispatcherInfo () |> ignore
        let statusText = get vm "Status" :?> string
        report.["estimate"] <- dict [ "offsetSeconds", (if statusText.StartsWith("Estimated offset") then get vm "TimeOffsetSeconds" else null)
                                      "status", box statusText; "outputLog", get vm "OutputLog"
                                      "timeOffsetSeconds", get vm "TimeOffsetSeconds"; "busy", get vm "Busy"
                                      "durationMs", box sw.ElapsedMilliseconds ]
    if runGeotag then
        let geotaggedDir = Path.Combine(photoDir, "geotagged")
        if not staleNames.IsEmpty then
            Directory.CreateDirectory(geotaggedDir) |> ignore
            for n in staleNames do
                if String.IsNullOrWhiteSpace(n) || Path.GetFileName(n) <> n || n = "." || n = ".." then
                    failwith "Stale fixture names must be plain file names"
                File.WriteAllText(Path.Combine(geotaggedDir, n), "stale")
        let before = snapshotSettings vm
        let sw = Stopwatch.StartNew()
        awaitTask (callPublic vm "GeoTagAsync")
        let dispatcherAfter = dispatcherInfo ()
        let results = List<obj>()
        for r in (get vm "Results" :?> IEnumerable) do results.Add(describeValue r)
        let outputs = List<obj>()
        if Directory.Exists(geotaggedDir) then
            for f in Directory.GetFiles(geotaggedDir) |> Array.sort do outputs.Add(fileRecord f)
        let textOrNull (p: string) : obj = if File.Exists(p) then box (File.ReadAllText(p)) else null
        report.["geotag"] <- dict [ "settingsBefore", before; "settingsAfter", snapshotSettings vm
                                    "status", get vm "Status"; "outputLog", get vm "OutputLog"; "busy", get vm "Busy"
                                    "results", box results; "outputs", box outputs
                                    "locationTxt", textOrNull (Path.Combine(geotaggedDir, "location.txt"))
                                    "locationKml", textOrNull (Path.Combine(geotaggedDir, "location.kml"))
                                    "dispatcherAfter", dispatcherAfter; "durationMs", box sw.ElapsedMilliseconds ]
    report.["error"] <- null
    writeReport ()
    exit 0
with ex ->
    report.["error"] <- box (ex.ToString())
    writeReport ()
    exit 1
