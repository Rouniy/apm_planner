// Compare actual MP10 checks on ordered, explicitly specified sample models.
// dotnet fsi --exec LogAnalyzerModelOracle.fsx MP10_BIN CASES.json NEW_OUTPUT.json
open System
open System.IO
open System.Reflection
open System.Runtime.Loader
open System.Globalization
open System.Collections
open System.Collections.Generic
open System.Text.Json

if fsi.CommandLineArgs.Length <> 4 then failwith "Expected MP10_BIN CASES.json NEW_OUTPUT.json"
let bin = Path.GetFullPath(fsi.CommandLineArgs.[1])
AssemblyLoadContext.Default.add_Resolving(
    Func<AssemblyLoadContext, AssemblyName, Assembly>(fun ctx name ->
        let path = Path.Combine(bin, name.Name + ".dll")
        if File.Exists(path) then ctx.LoadFromAssemblyPath(path) else null))
CultureInfo.DefaultThreadCurrentCulture <- CultureInfo.InvariantCulture
CultureInfo.DefaultThreadCurrentUICulture <- CultureInfo.GetCultureInfo("en-US")
let mp = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(bin, "MissionPlanner.dll"))
let analyzer = mp.GetType("MissionPlanner.Services.LogAnalyzer", true)
let dataType = mp.GetType("MissionPlanner.Services.LogAnalyzerData", true)
let sampleType = mp.GetType("MissionPlanner.Services.LogAnalyzerSample", true)
let vehicleType = mp.GetType("MissionPlanner.Services.LogAnalyzerVehicleType", true)
let listType = typedefof<List<_>>.MakeGenericType([| sampleType |])
let readonlyList = typedefof<IReadOnlyList<_>>.MakeGenericType([| sampleType |])
let recordsType = typedefof<Dictionary<_,_>>.MakeGenericType([|typeof<string>; readonlyList|])
let analyze = analyzer.GetMethod("Analyze", [|dataType|])
let number (v: JsonElement) =
    if v.ValueKind = JsonValueKind.String then Double.Parse(v.GetString(), CultureInfo.InvariantCulture)
    else v.GetDouble()
let numbers (pairs: JsonElement) =
    let result = Dictionary<string,double>(StringComparer.OrdinalIgnoreCase)
    for pair in pairs.EnumerateArray() do result.Add(pair.[0].GetString(), number pair.[1])
    result
let property (item: obj) name = item.GetType().GetProperty(name).GetValue(item)
use input = JsonDocument.Parse(File.ReadAllText(fsi.CommandLineArgs.[2]))
let output = ResizeArray<obj>()
for case in input.RootElement.EnumerateArray() do
    let records = Activator.CreateInstance(recordsType) :?> IDictionary
    for group in case.GetProperty("records").EnumerateArray() do
        let samples = Activator.CreateInstance(listType) :?> IList
        for row in group.GetProperty("samples").EnumerateArray() do
            let text = Dictionary<string,string>(StringComparer.OrdinalIgnoreCase)
            for pair in row.GetProperty("texts").EnumerateArray() do text.Add(pair.[0].GetString(), pair.[1].GetString())
            let sample = Activator.CreateInstance(sampleType,
                [|box (row.GetProperty("line").GetInt32()); box (number (row.GetProperty("time")))
                  box (numbers (row.GetProperty("values"))); box text|])
            samples.Add(sample) |> ignore
        records.Add(group.GetProperty("type").GetString(), samples)
    let data = Activator.CreateInstance(dataType,
        [|box (case.GetProperty("lineCount").GetInt32())
          Enum.Parse(vehicleType, case.GetProperty("vehicleType").GetString())
          box records; box (numbers (case.GetProperty("parameters")))|])
    let tests = analyze.Invoke(null, [|data|]) :?> IEnumerable
    let rows = tests |> Seq.cast<obj> |> Seq.map(fun item ->
        {| name = property item "Name" :?> string
           status = (property item "Status").ToString().ToUpperInvariant()
           message = property item "Message" :?> string |}) |> Seq.toArray
    output.Add(box {| name = case.GetProperty("name").GetString(); tests = rows |})
use destination = new FileStream(fsi.CommandLineArgs.[3], FileMode.CreateNew, FileAccess.Write)
JsonSerializer.Serialize(destination, output)
printfn "MODEL_ORACLE: cases=%d checks=%d" output.Count (output.Count * 17)
