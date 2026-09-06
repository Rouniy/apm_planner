// Verification only: invoke the actual built Mission Planner 10 service.
// dotnet fsi --exec TlogMatlabOracle.fsx MP10_BIN NEW_OUTPUT_DIR INPUT.tlog [...]
open System
open System.IO
open System.Reflection
open System.Runtime.Loader
open System.Globalization
if fsi.CommandLineArgs.Length < 4 then
    failwith "Expected MP10 bin directory, a separate output directory and at least one TLOG"
let bin = Path.GetFullPath(fsi.CommandLineArgs.[1])
AssemblyLoadContext.Default.add_Resolving(
    Func<AssemblyLoadContext, AssemblyName, Assembly>(fun ctx name ->
        let path = Path.Combine(bin, name.Name + ".dll")
        if File.Exists(path) then ctx.LoadFromAssemblyPath(path) else null))
CultureInfo.DefaultThreadCurrentCulture <- CultureInfo.InvariantCulture
CultureInfo.DefaultThreadCurrentUICulture <- CultureInfo.GetCultureInfo("en-US")
let utilities = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "MissionPlanner.Utilities.dll"))
let matlab = utilities.GetType("MissionPlanner.Log.MatLab", true)
let outputRoot = Path.GetFullPath(fsi.CommandLineArgs.[2])
Directory.CreateDirectory(outputRoot) |> ignore
for source in fsi.CommandLineArgs |> Array.skip 3 do
    let target = Path.Combine(outputRoot, Path.GetFileName(source))
    if File.Exists(target) || File.Exists(target + ".mat") then
        failwith "Oracle outputs must be new; existing files are never replaced"
    File.Copy(source, target, false)
    matlab.GetMethod("tlog", BindingFlags.Public ||| BindingFlags.Static).Invoke(null, [|box target|]) |> ignore
    printfn "MAT_ORACLE:%s" (target + ".mat")
printfn "MIN_DATE_ORACLE:%.17g" (matlab.GetMethod("GetMatLabSerialDate").Invoke(null, [|box DateTime.MinValue|]) :?> double)
for value in [| "2023-06-01T12:34:56.789Z"; "2024-01-15T00:00:00Z"; "2024-03-01T00:00:00Z"; "2025-06-01T00:00:00Z"; "2025-09-06T00:00:00Z" |] do
    let utc = DateTime.Parse(value, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal ||| DateTimeStyles.AssumeUniversal)
    let serial = matlab.GetMethod("GetMatLabSerialDate").Invoke(null, [|box (utc.ToLocalTime())|]) :?> double
    printfn "DATE_ORACLE:%s %.17g" value serial
