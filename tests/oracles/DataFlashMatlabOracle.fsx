// Invoke the actual MP10 GUI service on private copies; never rewrite input logs.
// dotnet fsi --exec DataFlashMatlabOracle.fsx MP10_BIN NEW_OUTPUT_DIR INPUT [...]
open System
open System.IO
open System.Reflection
open System.Runtime.Loader
open System.Globalization
open System.Text.Json

if fsi.CommandLineArgs.Length < 4 then
    failwith "Expected MP10 bin directory, new output directory and BIN/LOG input(s)"
let bin = Path.GetFullPath(fsi.CommandLineArgs.[1])
let overlay = Path.Combine(bin, "ParameterMetaDataLocal.xml")
if not (File.Exists(overlay)) then
    failwith "Reference bin is missing the shipped mode metadata overlay; rebuild or use the current Release bin"
AssemblyLoadContext.Default.add_Resolving(
    Func<AssemblyLoadContext, AssemblyName, Assembly>(fun ctx name ->
        let path = Path.Combine(bin, name.Name + ".dll")
        if File.Exists(path) then ctx.LoadFromAssemblyPath(path) else null))
CultureInfo.DefaultThreadCurrentCulture <- CultureInfo.InvariantCulture
CultureInfo.DefaultThreadCurrentUICulture <- CultureInfo.GetCultureInfo("en-US")
let mp = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "MissionPlanner.dll"))
let exporter = mp.GetType("MissionPlanner.Services.DataFlashLog", true).GetMethod("ExportMatlab")
let outputRoot = Path.GetFullPath(fsi.CommandLineArgs.[2])
if Directory.Exists(outputRoot) || File.Exists(outputRoot) then
    failwith "Oracle output directory must be new"
Directory.CreateDirectory(outputRoot) |> ignore
// FSI's install directory is not the GUI's running directory. Point the real
// metadata loader at the bundled files and isolate any writable cache/settings.
let utilities = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "MissionPlanner.Utilities.dll"))
let settings = utilities.GetType("MissionPlanner.Utilities.Settings", true)
let flags = BindingFlags.Public ||| BindingFlags.NonPublic ||| BindingFlags.Static
let cache = Path.Combine(outputRoot, "metadata-cache")
Directory.CreateDirectory(cache) |> ignore
settings.GetField("_GetRunningDirectory", flags).SetValue(null, bin + string Path.DirectorySeparatorChar)
settings.GetField("CustomDataDirectory", flags).SetValue(null, cache)
settings.GetField("CustomUserDataDirectory", flags).SetValue(null, Path.Combine(outputRoot, "isolated-settings"))
let modes = mp.GetType("MissionPlanner.Services.FlightModeNames", true)
modes.GetMethod("Initialize", flags).Invoke(null, [||]) |> ignore
let mode = modes.GetMethod("Resolve", flags).Invoke(null, [|box "ArduCopter2"; box 3|]) :?> string
if mode <> "Auto" then failwithf "Actual bundled mode metadata unavailable: %A" mode
let turtle = modes.GetMethod("Resolve", flags).Invoke(null, [|box "ArduCopter2"; box 28|]) :?> string
if turtle <> "Turtle" then failwithf "Actual shipped mode overlay unavailable: %A" turtle
for source in fsi.CommandLineArgs |> Array.skip 3 do
    let target = Path.Combine(outputRoot, Path.GetFileName(source))
    File.Copy(source, target, false)
    let progress = ResizeArray<string>()
    let callback = Action<string>(fun value -> progress.Add(value))
    exporter.Invoke(null, [|box target; box callback|]) |> ignore
    let outputs = Directory.GetFiles(outputRoot, Path.GetFileName(target) + "-*.mat") |> Array.sort
    if outputs.Length <> 1 then failwithf "Expected one x64 output, got %d" outputs.Length
    File.WriteAllText(target + ".oracle.json", JsonSerializer.Serialize(
        {| source = Path.GetFileName(source); outputs = outputs |> Array.map Path.GetFileName
           progress = progress.ToArray(); is64Bit = Environment.Is64BitProcess
           referenceBin = bin; modeOverlaySha256 = Convert.ToHexString(
               System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(overlay))) |}))
    printfn "DATAFLASH_MAT_ORACLE:%s" outputs.[0]
