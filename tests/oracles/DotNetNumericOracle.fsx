// Pin the active .NET runtime's numeric text and unchecked conv.i4 semantics.
open System
open System.Globalization
open System.Reflection.Emit
CultureInfo.DefaultThreadCurrentCulture <- CultureInfo.InvariantCulture
let convert = new DynamicMethod("uncheckedInt", typeof<int>, [|typeof<double>|])
let il = convert.GetILGenerator()
il.Emit(OpCodes.Ldarg_0)
il.Emit(OpCodes.Conv_I4)
il.Emit(OpCodes.Ret)
let cast = convert.CreateDelegate(typeof<Func<double,int>>) :?> Func<double,int>
for value in [|Double.NaN; Double.PositiveInfinity; Double.NegativeInfinity; 1e10; -1e10; 3.9|] do
    printfn "CAST %s -> %d" (value.ToString("R")) (cast.Invoke(value))
for value in [|1.25; 0.125; 2.675; 1.005; 580.5; 0.00125; -0.125; -0.0|] do
    printfn "FORMAT %s 0:%s 0.0:%s 0.00:%s P1:%s" (value.ToString("R"))
        (value.ToString("0")) (value.ToString("0.0")) (value.ToString("0.00")) (value.ToString("P1"))
