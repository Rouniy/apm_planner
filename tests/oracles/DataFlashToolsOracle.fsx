// Invoke the actual built MP10 services on private copies, never the source log.
// dotnet fsi --exec DataFlashToolsOracle.fsx MP10_BIN NEW_OUTPUT_DIR INPUT [...]
open System
open System.IO
open System.Reflection
open System.Runtime.Loader
open System.Globalization
open System.Collections
open System.Text.Json

if fsi.CommandLineArgs.Length < 4 then
    failwith "Expected MP10 bin directory, a new output directory and DataFlash inputs"
let bin = Path.GetFullPath(fsi.CommandLineArgs.[1])
AssemblyLoadContext.Default.add_Resolving(
    Func<AssemblyLoadContext, AssemblyName, Assembly>(fun ctx name ->
        let path = Path.Combine(bin, name.Name + ".dll")
        if File.Exists(path) then ctx.LoadFromAssemblyPath(path) else null))
CultureInfo.DefaultThreadCurrentCulture <- CultureInfo.InvariantCulture
CultureInfo.DefaultThreadCurrentUICulture <- CultureInfo.GetCultureInfo("en-US")
let mp = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "MissionPlanner.dll"))
let analyzer = mp.GetType("MissionPlanner.Services.LogAnalyzer", true)
let dataflash = mp.GetType("MissionPlanner.Services.DataFlashLog", true)
let analyze = analyzer.GetMethod("Analyze", [| typeof<string> |])
let outRoot = Path.GetFullPath(fsi.CommandLineArgs.[2])
if Directory.Exists(outRoot) || File.Exists(outRoot) then
    failwith "Oracle output directory must not already exist"
Directory.CreateDirectory(outRoot) |> ignore
let property (item: obj) name = item.GetType().GetProperty(name).GetValue(item)
for source in fsi.CommandLineArgs |> Array.skip 3 do
    let path = Path.Combine(outRoot, Path.GetFileName(source))
    File.Copy(source, path, false)
    let result = analyze.Invoke(null, [| box path |]) :?> IEnumerable
    let rows =
        result |> Seq.cast<obj> |> Seq.map(fun item ->
            {| name = property item "Name" :?> string
               status = (property item "Status").ToString().ToUpperInvariant()
               message = property item "Message" :?> string |}) |> Seq.toArray
    File.WriteAllText(path + ".analysis.json", JsonSerializer.Serialize(rows))
    // The actual reference writer currently throws: it starts gpx in the
    // empty namespace then redefines xmlns on that same element. Preserve
    // this evidence, but independently expose its real ReadTrack and exact
    // timestamp formatting so valid Qt XML can still be compared semantically.
    try
        dataflash.GetMethod("ExportGpx").Invoke(null, [|box path; box (path + ".gpx")|]) |> ignore
    with error ->
        let detail = if isNull error.InnerException then error else error.InnerException
        File.WriteAllText(path + ".gpx-error.txt", detail.ToString())
        printfn "GPX_REFERENCE_ERROR:%s" detail.Message
    let track = dataflash.GetMethod("ReadTrack").Invoke(null, [|box path|]) :?> IEnumerable
    let field (item: obj) name = item.GetType().GetField(name).GetValue(item)
    let points =
        track |> Seq.cast<obj> |> Seq.map(fun item ->
            {| lat = field item "Item1" :?> double
               lon = field item "Item2" :?> double
               ele = field item "Item3" :?> double
               time = (field item "Item4" :?> DateTime).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture) |}) |> Seq.toArray
    let jsonOptions = JsonSerializerOptions(NumberHandling = System.Text.Json.Serialization.JsonNumberHandling.AllowNamedFloatingPointLiterals)
    File.WriteAllText(path + ".track.json", JsonSerializer.Serialize(points, jsonOptions))
    dataflash.GetMethod("ExportKml").Invoke(null, [|box path; box (path + ".kml")|]) |> ignore
    printfn "DATAFLASH_ORACLE:%s tests=%d" path rows.Length
