$projname = "cnewsetup"
$output_path = "$PSScriptRoot\bin\debug"
Push-Location "$output_path"

# cimgui 1.74
$cimgui_source_path = "$PSScriptRoot\source\cimgui"
$cimgui_source_files = (Get-ChildItem -File -Path $cimgui_source_path) | Where-Object {$_.Extension -eq ".h" -or ".c" -or "cpp"}

$last_cimgui_compilation_output = (Get-Item "$output_path\cimgui.lib")
foreach($file in $cimgui_source_files)
{
    $file_was_modified = ($file.LastWriteTime) -gt ($last_cimgui_compilation_output.LastWriteTime)
    
    if($file_was_modified)
    {
        Write-Host $file.Name "was modified, recompiling cimgui.lib" -ForegroundColor Magenta
        $pre_compilation = (Get-Date)

        clang++.exe -std=c++14 -g -c `
        -D "IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1" `
        -D 'IMGUI_IMPL_API="extern "C" __declspec(dllexport)"' `
        -D "cimgui_EXPORTS" `
        "$PSScriptRoot\source\cimgui\cimgui.cpp" `
        "$PSScriptRoot\source\cimgui\imgui.cpp" `
        "$PSScriptRoot\source\cimgui\imgui_demo.cpp" `
        "$PSScriptRoot\source\cimgui\imgui_draw.cpp" `
        "$PSScriptRoot\source\cimgui\imgui_widgets.cpp"

        $post_compilation = (Get-Date)
        $compile_time = New-TimeSpan –Start $pre_compilation –End $post_compilation
        $pre_linking = (Get-Date)

        # The libraries (.lib) are specified in the source code using pragmas. Here we simply specify where they are with -libpath
        Write-Host "Linking (lib) imgui.lib" -ForegroundColor Magenta
        llvm-lib.exe -machine:X64 `
        "$output_path\cimgui.o" `
        "$output_path\imgui.o" `
        "$output_path\imgui_demo.o" `
        "$output_path\imgui_draw.o" `
        "$output_path\imgui_widgets.o"

        $post_linking = (Get-Date)
        $link_time = New-TimeSpan –Start $pre_linking –End $post_linking
        Write-Host "Compilation took" $compile_time.TotalMilliseconds"ms" -ForegroundColor Magenta
        Write-Host "Linking took" $link_time.TotalMilliseconds"ms" -ForegroundColor Magenta
        Write-Host "Total" ($link_time.TotalMilliseconds + $compile_time.TotalMilliseconds)"ms" -ForegroundColor Magenta
        break
    }
}

#cmetrics_gui
#$cmetricsgui_source_path = "$PSScriptRoot\source\metrics_gui"

#clang++.exe -std=c++14 -g -c `
#    "$cmetricsgui_source_path\cmetrics_gui.cpp"

#lld-link.exe -dll -debug -subsystem:windows -defaultlib:libcmt `
 #   -libpath:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.18362.0\um\x64" `
  #  -libpath:"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.23.28105\lib\x64" `
   # -libpath:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.18362.0\ucrt\x64" `
    #"$output_path\cmetrics_gui.o"

#llvm-lib.exe -machine:X64 `
#   "$output_path\cmetrics_gui.o"

#cwinpix
$cwinpix_source_path = "$PSScriptRoot\source\WinPixEventRuntime"

clang++.exe -std=c++14 -g -c `
    -D "USE_PIX" `
    "$cwinpix_source_path\cwinpix.cpp"

llvm-lib.exe -machine:X64 `
"$output_path\cwinpix.o"
        
#pch
$pch_source = Get-Item("$PSScriptRoot\source\cnewsetup.h")
$pch = Get-Item("$output_path\cnewsetup.h.pch")

if($pch_source.LastWriteTime -gt $pch.LastWriteTime)
{
    Write-Host $pch_source.Name "was modified, recompiling cnewsetup.h.pch" -ForegroundColor Yellow
    clang.exe -g -x c-header "$PSScriptRoot\source\cnewsetup.h" `
    -D "CIMGUI_DEFINE_ENUMS_AND_STRUCTS" `
    -D "CINTERFACE" `
    -o "$output_path\cnewsetup.h.pch"
}


# game_code
$gamecode_source_path = "$PSScriptRoot\source"
#$gamecode_source_files = (Get-ChildItem -File -Path $gamecode_source_path) | Where-Object {$_.Extension -eq ".h" -or ".c" -or "cpp"}
$gamecode_source_files = @((Get-Item "$PSScriptRoot\source\game_code.c"), (Get-Item "$PSScriptRoot\source\imgui_impl_dx12.c"), (Get-Item "$PSScriptRoot\source\imgui_impl_win32.c"))
$last_gamecode_compilation_output = (Get-Item "$output_path\game_code.dll")

foreach($file in $gamecode_source_files)
{
    $file_was_modified = ($file.LastWriteTime) -gt ($last_gamecode_compilation_output.LastWriteTime)
        
    if($last_gamecode_compilation_output.Exists)
    {
        if(!$file_was_modified)
        {
            continue
        }
        else
        {
            Write-Host $file.Name "was modified, recompiling game_code.dll" -ForegroundColor DarkGreen
        }
    }
    else
    {
        Write-Host "game_code.dll not found, compiling game_code.dll" -ForegroundColor DarkGreen
    }
            
    $pre_compilation = (Get-Date)
    clang.exe -std=c11 -g -c -Weverything -include "$PSScriptRoot\source\cnewsetup.h" `
    -D "CIMGUI_DEFINE_ENUMS_AND_STRUCTS" `
    -D "CINTERFACE" `
    "$PSScriptRoot\source\game_code.c" `
    "$PSScriptRoot\source\imgui_impl_dx12.c" `
    "$PSScriptRoot\source\imgui_impl_win32.c"

    $post_compilation = (Get-Date)
    $compile_time = New-TimeSpan –Start $pre_compilation –End $post_compilation
    $pre_linking = (Get-Date)

    Write-Host "Linking game_code.dll" -ForegroundColor DarkGreen
    lld-link.exe -dll -debug -subsystem:windows -defaultlib:libcmt `
    -libpath:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.18362.0\um\x64" `
    -libpath:"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.23.28105\lib\x64" `
    -libpath:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.18362.0\ucrt\x64" `
    -libpath:"$output_path" `
    "$output_path\game_code.o" `
    "$output_path\imgui_impl_win32.o" `
    "$output_path\imgui_impl_dx12.o"

    $post_linking = (Get-Date)
    $link_time = New-TimeSpan –Start $pre_linking –End $post_linking
    Write-Host "Compilation took" $compile_time.TotalMilliseconds"ms" -ForegroundColor DarkGreen
    Write-Host "Linking took" $link_time.TotalMilliseconds"ms" -ForegroundColor DarkGreen
    Write-Host "Total" ($link_time.TotalMilliseconds + $compile_time.TotalMilliseconds)"ms" -ForegroundColor DarkGreen

    break
}

# executable
Write-Host "Compiling $projname.exe" -ForegroundColor Blue
clang.exe -std=c11 -g -c -Weverything -include "$PSScriptRoot\source\cnewsetup.h" `
-D "CIMGUI_DEFINE_ENUMS_AND_STRUCTS" `
-D "CINTERFACE" `
"$PSScriptRoot\source\main.c"

Write-Host "Linking $projname.exe" -ForegroundColor Blue
lld-link.exe -defaultlib:libcmt -debug `
 -out:"$projname.exe" `
 -libpath:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.18362.0\um\x64" `
 -libpath:"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.23.28105\lib\x64" `
 -libpath:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.18362.0\ucrt\x64"`
  "$output_path\main.o"

Pop-Location
return "$output_path\$projname.exe"
